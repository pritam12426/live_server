/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * file_send.c — File sending with range/sendfile support
 *
 * This module handles sending files over HTTP with full semantics:
 * - ETag/If-None-Match conditional requests (304 Not Modified)
 * - If-Modified-Since conditional requests (304 Not Modified)
 * - Range requests (206 Partial Content) with proper Content-Range headers
 * - sendfile() zero-copy on Linux for non-range GET requests
 * - LiveReload script injection for HTML files
 * - Keep-alive connection handling
 */

#include "file_send.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include "project_config.h"
#include <sys/sendfile.h>
#elif defined(__APPLE__)
#include <sys/uio.h>
#endif

#include "log.h"
#include "mime.h"
#include "response.h"
#include "thread_buffer.h"
#include "transport.h"
#include "file_etag.h"
#include "file_livereload.h"

/**
 * Format a timestamp as HTTP-date (RFC 7231) in UTC.
 */
static void http_date(time_t t, char *buf, size_t len)
{
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(buf, len, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

#if defined(__linux__)
/**
 * Generate current date in RFC 1123 format for HTTP headers (Linux sendfile path).
 * sendfile() bypasses response_send(), so we build the Date header manually.
 */
static void rfc1123_date(char *buf, size_t len)
{
	time_t    now = time(NULL);
	struct tm tm;
	gmtime_r(&now, &tm);
	strftime(buf, len, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}
#endif

/**
 * Log an HTTP request in combined log format.
 * Extended variant includes response size and MIME type.
 */
static void log_request(const char        *client_ip,
                        int                client_port,
                        const HttpRequest *req,
                        int                status,
                        long long          bytes,
                        const char        *mime)
{
	if (bytes >= 0 && mime) {
		LOG_INFO("%s:%d \"%s %s %s\" %d - (%lld bytes, %s)",
		         client_ip,
		         client_port,
		         req->method_str,
		         req->path,
		         req->version,
		         status,
		         bytes,
		         mime);
	} else {
		LOG_INFO("%s:%d \"%s %s %s\" %d",
		         client_ip,
		         client_port,
		         req->method_str,
		         req->path,
		         req->version,
		         status);
	}
}

/**
 * Send a file with full HTTP semantics.
 *
 * Handles:
 *   - ETag generation and If-None-Match matching
 *   - If-Modified-Since conditional requests
 *   - Range requests (bytes=start-end) with 206 Partial Content
 *   - sendfile() zero-copy on Linux for regular GET (no range)
 *   - LiveReload script injection for text/html
 *   - Keep-alive vs close Connection header
 *
 * @param t                 Transport handle for I/O
 * @param client_ip         Client IP address (for logging)
 * @param client_port       Client port (for logging)
 * @param req               Parsed HTTP request
 * @param fs_path           Absolute filesystem path to serve
 * @param pre_stat          Optional pre-stat() result (used for index.html)
 * @param livereload_mode   LiveReload injection mode
 * @param print_request     Whether to log this request
 * @param keep_alive        Whether to send Connection: keep-alive
 * @return HTTP status code
 */
int file_send_file(Transport *t,
                   const char *client_ip,
                   int client_port,
                   const HttpRequest *req,
                   const char *fs_path,
                   const struct stat *pre_stat,
                   LivereloadMode livereload_mode,
                   int print_request,
                   int keep_alive)
{
	/* Use pre-stat result if provided (e.g., for index.html), else stat the file */
	struct stat st;
	if (pre_stat) {
		st = *pre_stat;
	} else if (stat(fs_path, &st) != 0) {
		response_error(t, 404, "Not Found", "File not found.");
		if (print_request)
			log_request(client_ip, client_port, req, 404, -1, NULL);
		return 404;
	}

	const char *mime    = mime_from_path(fs_path);
	int         is_html = (strstr(mime, "text/html") != NULL);

	/* Generate ETag: "mtime-hex-size-hex" (both lowercase hex, quoted) */
	char etag[64];
	build_etag(&st, etag, sizeof etag);

	/* Last-Modified header in HTTP-date format */
	char last_modified[64];
	http_date(st.st_mtime, last_modified, sizeof last_modified);

	/* If-None-Match: return 304 if ETag matches */
	if (req->if_none_match[0] && etag_match(etag, req->if_none_match)) {
		LOG_INFO("%s:%d \"%s\" 304 (ETag match)", client_ip, client_port, req->path);
		char extra[256];
		snprintf(extra, sizeof extra, "ETag: %s\r\nLast-Modified: %s\r\n", etag, last_modified);
		response_send(t, 304, "Not Modified", NULL, extra, NULL, 0, keep_alive, 1);
		if (print_request)
			log_request(client_ip, client_port, req, 304, -1, NULL);
		return 304;
	}

	/* If-Modified-Since: return 304 if file not modified since given date */
	if (req->if_modified_since[0] && !req->if_none_match[0]) {
		struct tm tm_ims = { 0 };
		if (strptime(req->if_modified_since, "%a, %d %b %Y %H:%M:%S GMT", &tm_ims)) {
			time_t ims_t = timegm(&tm_ims);
			if (st.st_mtime <= ims_t) {
				LOG_INFO("%s:%d \"%s\" 304 (If-Modified-Since)", client_ip, client_port, req->path);
				char extra[256];
				snprintf(extra,
				         sizeof extra,
				         "ETag: %s\r\nLast-Modified: %s\r\n",
				         etag,
				         last_modified);
				response_send(t, 304, "Not Modified", NULL, extra, NULL, 0, keep_alive, 1);
				if (print_request)
					log_request(client_ip, client_port, req, 304, -1, NULL);
				return 304;
			}
		}
	}

	long file_size = (long) st.st_size;
	LOG_INFO("%s:%d \"%s %s\" (%s, %ld bytes)",
	         client_ip, client_port, req->method_str, req->path, mime, file_size);

	/* Open file for reading */
	int fd = open(fs_path, O_RDONLY);
	if (fd < 0) {
		LOG_WARN("Failed to open file: %s (fd=%d)", req->path, transport_fd(t));
		response_error(t, 403, "Forbidden", "Cannot open file.");
		if (print_request)
			log_request(client_ip, client_port, req, 403, -1, NULL);
		return 403;
	}

	/*
	 * LiveReload injection: for HTML files, inject script before </body>
	 * This reads the entire file into a thread-local buffer, modifies it,
	 * then sends via response_send().
	 */
	if (is_html && file_livereload_should_inject(mime, livereload_mode)) {
		LOG_DEBUG("Injecting live-reload script into %s", req->path);
		size_t script_len = strlen("<script>\n"
			"(function(){\n"
			"	var es = new EventSource('/livereload');\n"
			"		es.onmessage = function(e) {\n"
			"		if (e.data === 'reload') {\n"
			"			window.location.reload();\n"
			"		} else if (e.data === 'hard-reload') {\n"
			"			var url = new URL(window.location.href);\n"
			"			url.searchParams.set('_lr', Date.now());\n"
			"			window.location.replace(url.toString());\n"
			"		}\n"
			"	};\n"
			"})();\n"
			"</script>\n");
		char  *body       = thread_buffer_get_body((size_t) file_size + script_len + 32);
		if (!body) {
			close(fd);
			response_error(t, 500, "Internal Server Error", "Out of memory.");
			return 500;
		}

		ssize_t n = read(fd, body, (size_t) file_size);
		close(fd);
		size_t body_len = (n > 0) ? (size_t) n : 0;
		body[body_len]  = '\0';

		size_t out_len = 0;
		file_livereload_inject(body, body_len, body, 4096, &out_len);

		char extra[512];
		snprintf(extra,
		         sizeof extra,
		         "ETag: %s\r\nLast-Modified: %s\r\nCache-Control: no-store\r\n",
		         etag,
		         last_modified);
		response_send(t, 200, "OK", mime, extra, body, out_len, keep_alive,
		              req->method != HTTP_HEAD);
		if (print_request)
			log_request(client_ip, client_port, req, 200, (long long) out_len, mime);
		return 200;
	}

	/* Parse Range header if present */
	long range_first = 0, range_last = file_size - 1;
	int  is_range = 0;

	if (req->range_start != -1) {
		if (req->range_start < 0) {
			/* Suffix range: bytes=-N means last N bytes */
			range_first = file_size + req->range_start;
			range_last  = file_size - 1;
		} else {
			/* Start/end range: bytes=start-end */
			range_first = (long) req->range_start;
			range_last  = (req->range_end >= 0) ? (long) req->range_end : file_size - 1;
		}

		/* Clamp to file boundaries */
		if (range_first < 0)
			range_first = 0;
		if (range_last >= file_size)
			range_last = file_size - 1;

		/* Validate range */
		if (range_first > range_last || range_first >= file_size) {
			close(fd);
			LOG_WARN("Range not satisfiable: %ld-%ld (file size %ld)",
			         range_first,
			         range_last,
			         file_size);
			char extra[128];
			snprintf(extra, sizeof extra, "Content-Range: bytes */%ld\r\n", file_size);
			response_send(t, 416, "Range Not Satisfiable", NULL, extra, NULL, 0, keep_alive, 1);
			if (print_request)
				log_request(client_ip, client_port, req, 416, -1, NULL);
			return 416;
		}
		LOG_INFO("Range request: bytes=%ld-%ld of %ld", range_first, range_last, file_size);
		is_range = 1;
	}

	long send_len = range_last - range_first + 1;
	off_t offset = (off_t) range_first;

#if defined(__linux__)
	/*
	 * Linux sendfile() path: zero-copy kernel transfer from file to socket.
	 * Only used for non-range GET requests (HEAD uses fallback).
	 */
	if (!is_range && req->method != HTTP_HEAD) {
		char date[64];
		rfc1123_date(date, sizeof date);
		char extra[512];
		snprintf(extra,
		         sizeof extra,
		         "ETag: %s\r\nLast-Modified: %s\r\nAccept-Ranges: bytes\r\n",
		         etag,
		         last_modified);
		char hdr[4096];
		int  hdr_len = snprintf(hdr,
		                        sizeof hdr,
		                        "HTTP/1.1 200 OK\r\n"
		                        "Date: %s\r\n"
		                        "Server: " MAIN_BINARY "/" PROJECT_VERSION "\r\n"
		                        "Content-Type: %s\r\n"
		                        "Content-Length: %ld\r\n"
		                        "%s"
		                        "Connection: %s\r\n"
		                        "\r\n",
		                        date,
		                        mime,
		                        file_size,
		                        extra,
		                        keep_alive ? "keep-alive" : "close");
		if (hdr_len < 0 || (size_t)hdr_len >= sizeof hdr)
			hdr_len = (int)(sizeof hdr - 1);

		/* Write header directly via transport */
		transport_write(t, hdr, (size_t)hdr_len);

		/* sendfile() transfers file data directly in kernel */
		ssize_t sent = sendfile(transport_fd(t), fd, &offset, (size_t)file_size);
		close(fd);

		if (sent < 0) {
			LOG_PERROR("sendfile failed on fd=%d", transport_fd(t));
			return 500;
		}

		if (print_request)
			log_request(client_ip, client_port, req, 200, (long long) sent, mime);
		return 200;
	}
#endif

	/* Fallback path: read into buffer, then send via response_send().
	 * Used for: range requests, HEAD, non-Linux, or if sendfile unavailable. */
	if (range_first > 0)
		lseek(fd, offset, SEEK_SET);

	char *body = thread_buffer_get_body((size_t) send_len);
	if (!body) {
		close(fd);
		response_error(t, 500, "Internal Server Error", "Out of memory.");
		return 500;
	}

	ssize_t nread = read(fd, body, (size_t) send_len);
	close(fd);

	if (nread <= 0) {
		response_error(t, 500, "Internal Server Error", "Read error.");
		return 500;
	}
	size_t body_len = (size_t) nread;
	char extra[512];
	if (is_range) {
		snprintf(extra,
		         sizeof extra,
		         "ETag: %s\r\nLast-Modified: %s\r\n"
		         "Accept-Ranges: bytes\r\n"
		         "Content-Range: bytes %ld-%ld/%ld\r\n",
		         etag,
		         last_modified,
		         range_first,
		         range_last,
		         file_size);
	} else {
		snprintf(extra,
		         sizeof extra,
		         "ETag: %s\r\nLast-Modified: %s\r\nAccept-Ranges: bytes\r\n",
		         etag,
		         last_modified);
	}

	int status = is_range ? 206 : 200;
	response_send(t,
	              status,
	              is_range ? "Partial Content" : "OK",
	              mime,
	              extra,
	              body,
	              body_len,
	              keep_alive,
	              req->method != HTTP_HEAD);
	if (print_request)
		log_request(client_ip, client_port, req, status, (long long) body_len, mime);
	return status;
}