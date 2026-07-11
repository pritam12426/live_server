/*
 * http.c — HTTP request parsing and low-level response helpers
 *
 * Parses the request line + headers from a Transport into an HttpRequest,
 * and provides utility functions for status/redirect responses.
 */

#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"
#include "transport.h"

// In-place convert a string to lowercase
static void str_lower(char *s)
{
	for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

// In-place trim leading/trailing whitespace, returning the new start
static char *str_trim(char *s)
{
	while (isspace((unsigned char)*s)) s++;
	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)*(end - 1))) end--;
	*end = '\0';
	return s;
}

// Decode percent-encoded URL into dst (e.g. %20 → space, + → space)
void http_url_decode(char *dst, size_t dst_size, const char *src)
{
	size_t di = 0;
	while (*src && di + 1 < dst_size) {
		if (*src == '%' && isxdigit((unsigned char)src[1])
						&& isxdigit((unsigned char)src[2])) {
			char hex[3] = { src[1], src[2], '\0' };
			dst[di++] = (char)strtol(hex, NULL, 16);
			src += 3;
		} else if (*src == '+') {
			dst[di++] = ' ';
			src++;
		} else {
			dst[di++] = *src++;
		}
	}
	dst[di] = '\0';
}

// Read HTTP headers in buffered chunks until \r\n\r\n is found.
// Returns total bytes read (including the blank line), or -1 on error.
static ssize_t read_headers(Transport *t, char *buf, size_t max)
{
	size_t n  = 0;

	while (n + 1 < max) {
		// Read in chunks to minimize syscalls; leave room for partial match
		size_t want = max - n;
		if (want > 4096) want = 4096;
		ssize_t rc = transport_read(t, buf + n, want);
		if (rc <= 0) return -1;
		n += (size_t)rc;

		// Scan the tail of the buffer for \r\n\r\n
		size_t scan = (n >= 4) ? n - 4 : 0;
		for (size_t i = scan; i + 3 < n; i++) {
			if (buf[i] == '\r' && buf[i+1] == '\n'
				&& buf[i+2] == '\r' && buf[i+3] == '\n') {
				n = i + 4;
				buf[n] = '\0';
				return (ssize_t)n;
			}
		}
	}
	buf[n] = '\0';
	return (ssize_t)n;
}

// Parse the first line of an HTTP request (e.g. "GET /path HTTP/1.1")
// Returns 0 on success, -1 on malformed input
static int parse_request_line(char *line, HttpRequest *r)
{
	char *sp1 = strchr(line, ' ');
	if (!sp1) return -1;
	*sp1 = '\0';

	// Store the raw method string (log what the client actually sent)
	snprintf(r->method_str, sizeof(r->method_str), "%s", line);
	// Uppercase it so logging is consistent
	for (char *p = r->method_str; *p; p++)
		*p = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);

	// Method
	if      (strcmp(line, "GET")  == 0) r->method = HTTP_GET;
	else if (strcmp(line, "HEAD") == 0) r->method = HTTP_HEAD;
	else {
		LOG_INFO("Unsupported method: %s", line);
		r->method = HTTP_OTHER;
	}

	char *uri = sp1 + 1;
	char *sp2 = strchr(uri, ' ');
	if (!sp2) return -1;
	*sp2 = '\0';

	// Version
	snprintf(r->version, sizeof(r->version), "%s", sp2 + 1);
	char *cr = strchr(r->version, '\r');
	if (cr) *cr = '\0';

	// Split URI into path and query string
	char *q = strchr(uri, '?');
	if (q) {
		*q = '\0';
		snprintf(r->query, sizeof(r->query), "%s", q + 1);
	} else {
		r->query[0] = '\0';
	}

	// URL-decode the path
	http_url_decode(r->path, sizeof(r->path), uri);

	LOG_DEBUG("Request line: %s %s → %s (qs=%s) %s",
	          http_method_str(r->method), uri, r->path, r->query, r->version);

	// Basic path-traversal check at parse time (defence-in-depth)
	if (strstr(r->path, "..")) {
		LOG_WARN("Path traversal attempt blocked: %s", r->path);
		return -1;
	}

	return 0;
}

// Parse a single header line into the HttpRequest struct
// (e.g. "If-None-Match: ..." → r->if_none_match)
static void parse_header(char *line, HttpRequest *r)
{
	char *colon = strchr(line, ':');
	if (!colon) return;

	*colon = '\0';
	char *name  = str_trim(line);
	char *value = str_trim(colon + 1);

	// Normalise header name to lowercase for matching
	char lname[256];
	snprintf(lname, sizeof(lname), "%s", name);
	str_lower(lname);

	if (strcmp(lname, "host") == 0) {
		snprintf(r->host, sizeof(r->host), "%s", value);
	} else if (strcmp(lname, "authorization") == 0) {
		LOG_DEBUG("Header: Authorization: Basic ****");
		snprintf(r->auth, sizeof(r->auth), "%s", value);
	} else if (strcmp(lname, "connection") == 0) {
		LOG_DEBUG("Header: Connection: %s", value);
		snprintf(r->connection, sizeof(r->connection), "%s", value);
	} else if (strcmp(lname, "if-none-match") == 0) {
		LOG_DEBUG("Header: If-None-Match: %s", value);
		snprintf(r->if_none_match, sizeof(r->if_none_match), "%s", value);
	} else if (strcmp(lname, "if-modified-since") == 0) {
		LOG_DEBUG("Header: If-Modified-Since: %s", value);
		snprintf(r->if_modified_since, sizeof(r->if_modified_since), "%s", value);
	} else if (strcmp(lname, "range") == 0) {
		// Parse "bytes=start-end" — supports both "bytes=N-" and "bytes=N-M"
		if (strncmp(value, "bytes=", 6) == 0) {
			char *dash = strchr(value + 6, '-');
			if (dash) {
				errno = 0;
				r->range_start = strtoll(value + 6, NULL, 10);
				if (errno != 0)
					r->range_start = -1;
				if (dash == value + 6) {
					// "bytes=-N" → suffix range (last N bytes)
					// range_start is already set to -N by strtoll above
				} else {
					r->range_end = (*(dash + 1) != '\0')
								   ? strtoll(dash + 1, NULL, 10)
								   : -1;
					if (errno != 0)
						r->range_end = -1;
				}
				LOG_DEBUG("Header: Range: bytes=%lld-%lld",
				          (long long)r->range_start, (long long)r->range_end);
			}
		}
	}
}

// Parse the full HTTP request (request line + headers) from a transport
// Returns 0 on success, -1 on failure
int http_parse_request(Transport *t, HttpRequest *out)
{
	memset(out, 0, sizeof(*out));
	out->range_start = -1;
	out->range_end   = -1;

	// Read raw bytes up to the end of headers
	ssize_t len = read_headers(t, out->raw, sizeof(out->raw) - 1);
	if (len <= 0) {
		LOG_WARN("Failed to read request headers on fd=%d", transport_fd(t));
		return -1;
	}
	out->raw_len = (size_t)len;

	// Work on a mutable copy of the raw buffer
	char buf[sizeof(out->raw)];
	memcpy(buf, out->raw, (size_t)(len + 1));

	char *line = buf;
	char *next;
	bool  first = true;

	// Walk through \r\n-separated lines
	while (*line) {
		next = strstr(line, "\r\n");
		if (next) { *next = '\0'; next += 2; }
		else       { next = line + strlen(line); }

		if (first) {
			// First line is the request line (method, URI, version)
			if (parse_request_line(line, out) != 0) {
				LOG_WARN("Bad request line on fd=%d: %s", transport_fd(t), line);
				return -1;
			}
			first = false;
		} else if (*line != '\0') {
			// Subsequent lines are headers
			parse_header(line, out);
		}
		line = next;
	}
	LOG_DEBUG("Parsed request: %s %s %s (fd=%d)",
			  http_method_str(out->method), out->path, out->version, transport_fd(t));
	return 0;
}

// Convert an HttpMethod enum value to its string representation
const char *http_method_str(HttpMethod m)
{
	switch (m) {
	case HTTP_GET:  return "GET";
	case HTTP_HEAD: return "HEAD";
	default:        return "?";
	}
}

// Return the standard reason phrase for a given HTTP status code
const char *http_status_text(int code)
{
	switch (code) {
		case 200: return "OK";
		case 206: return "Partial Content";
		case 301: return "Moved Permanently";
		case 304: return "Not Modified";
		case 400: return "Bad Request";
		case 401: return "Unauthorized";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 416: return "Range Not Satisfiable";
		case 500: return "Internal Server Error";
		default:  return "Unknown";
	}
}

// Send a minimal HTTP status response (text/html body)
void http_send_status(Transport *t, int code, const char *reason, const char *body)
{
	char buf[4096];
	size_t body_len = body ? strlen(body) : 0;
	int n = snprintf(buf, sizeof(buf),
		"HTTP/1.1 %d %s\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"Content-Length: %zu\r\n"
		"Connection: close\r\n"
		"\r\n",
		code, reason, body_len);
	if ((size_t)n > sizeof(buf)) n = (int)sizeof(buf) - 1;
	transport_write(t, buf, (size_t)n);
	if (body && body_len)
		transport_write(t, body, body_len);
}

// Send an HTTP 301 redirect to the given location
void http_send_redirect(Transport *t, const char *location)
{
	char buf[4096];
	int n = snprintf(buf, sizeof(buf),
		"HTTP/1.1 301 Moved Permanently\r\n"
		"Location: %s\r\n"
		"Content-Length: 0\r\n"
		"Connection: close\r\n"
		"\r\n",
		location);
	if ((size_t)n > sizeof(buf)) n = (int)sizeof(buf) - 1;
	transport_write(t, buf, (size_t)n);
}
