/*
 * http.h — HTTP types and parsing interface
 */

#ifndef _HTTP_H_
#define _HTTP_H_

#include <stddef.h>
#include <stdint.h>

typedef struct Transport Transport;

// HTTP methods we can handle
typedef enum {
	HTTP_GET  = 0,
	HTTP_HEAD = 1,
	HTTP_OTHER,          // Anything we don't support (POST, PUT, etc.)
} HttpMethod;

// Parsed HTTP request — all fields are null-terminated strings
typedef struct {
	HttpMethod method;       // GET, HEAD, or OTHER
	char       method_str[16]; // Raw method string (e.g. "GET", "POST", "DELETE")
	char       path[4096];  // URL-decoded path, no query string
	char       query[2048]; // URL-decoded query string (empty if none)
	char       version[16]; // e.g. "HTTP/1.1"

	char       host[256];          // Host header
	char       auth[512];          // Authorization header (full value)

	char       connection[32];         // Connection header (e.g. "keep-alive", "close")

	char       if_none_match[128];      // If-None-Match header (for ETag validation)
	char       if_modified_since[64];   // If-Modified-Since header

	int64_t    range_start;  // Range start (-1 = not specified / suffix)
	int64_t    range_end;    // Range end   (-1 = open-ended)

	char       raw[8192];   // Raw request data (request line + all headers)
	size_t     raw_len;     // Length of raw data
} HttpRequest;

// Parse a complete HTTP request from the transport
// Returns 0 on success, -1 on error
int http_parse_request(Transport *t, HttpRequest *out);

// Low-level response helpers (used internally by response.c)
void http_send_status(Transport *t, int code, const char *reason, const char *body);
void http_send_redirect(Transport *t, const char *location);

// URL percent-decoding utility
void http_url_decode(char *dst, size_t dst_size, const char *src);

// Lookup helpers
const char *http_status_text(int code);
const char *http_method_str(HttpMethod m);

#endif  // _HTTP_H_
