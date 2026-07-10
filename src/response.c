/*
 * response.c — High-level HTTP response builders
 *
 * Provides functions for sending full HTTP responses (including error
 * pages, redirects, and Server-Sent Events) through a Transport.
 */

#include "response.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "project_config.h"
#include "transport.h"

// Write len bytes, looping to handle partial writes
// Returns the number of bytes actually written
static size_t write_all(Transport *t, const char *buf, size_t len)
{
	size_t written = 0;
	while (written < len) {
		ssize_t n = transport_write(t, buf + written, len - written);
		if (n <= 0) {
			LOG_PERROR("write failed on fd=%d after %zu/%zu bytes", transport_fd(t), written, len);
			break;
		}
		written += (size_t) n;
	}
	return written;
}

// Format the current time as an RFC 1123 date string (for Date header)
static void rfc1123_date(char *buf, size_t len)
{
	time_t    now = time(NULL);
	struct tm tm;
	gmtime_r(&now, &tm);
	strftime(buf, len, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

// Send a complete HTTP response with headers and optional body
void response_send(Transport  *t,
                   int         status,
                   const char *status_text,
                   const char *mime,
                   const char *extra_hdrs,
                   const char *body,
                   size_t      body_len,
                   int         keep_alive,
                   int         send_body)
{
	char date[64];
	rfc1123_date(date, sizeof date);

	const char *conn = keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";

	// Build the full response header block
	char hdr[4096];
	int  hdr_len = snprintf(hdr,
                            sizeof hdr,
                            "HTTP/1.1 %d %s\r\n"
	                        "Date: %s\r\n"
	                        "Server: " MAIN_BINARY "/" PROJECT_VERSION "\r\n"
	                        "%s%s%s"            // Content-Type header (if mime provided)
	                        "Content-Length: %zu\r\n"
	                        "%s"                // extra headers (e.g. ETag, Location)
	                        "%s"                // Connection header
	                        "\r\n",
                            status,
                            status_text,
                            date,
                            mime ? "Content-Type: " : "",
                            mime ? mime : "",
                            mime ? "\r\n" : "",
                            body_len,
                            extra_hdrs ? extra_hdrs : "",
                            conn);

	LOG_DEBUG("HTTP %d %s → fd=%d (%d header + %zu body bytes)",
	          status,
	          status_text,
	          transport_fd(t),
	          hdr_len,
	          body_len);
	if ((size_t)hdr_len > sizeof(hdr)) hdr_len = (int)sizeof(hdr);
	write_all(t, hdr, (size_t) hdr_len);
	if (send_body && body && body_len > 0)
		write_all(t, body, body_len);
}

// Send an error page with a styled HTML body (status + detail message)
// The inline CSS supports light/dark mode via prefers-color-scheme
void response_error(Transport *t, int status, const char *status_text, const char *detail)
{
	char body[4096];
	int  blen = snprintf(
        body,
        sizeof(body),
        "<!DOCTYPE html>"
	     "<html lang='en'>"
	     "<head>"
	     "<meta charset='utf-8'>"
	     "<meta name='viewport' content='width=device-width, initial-scale=1'>"
	     "<title>%d %s</title>"
	     "<style>"
	     ":root {"
	     "--bg: #ffffff;"
	     "--text: #222222;"
	     "--muted: #555555;"
	     "--accent: #0066cc;"
	     "--border: #cccccc;"
	     "}"
	     "body.dark {"
	     "--bg: #1a1a1a;"
	     "--text: #dddddd;"
	     "--muted: #aaaaaa;"
	     "--accent: #66aaff;"
	     "--border: #444444;"
	     "}"
	     "* { box-sizing: border-box; margin: 0; padding: 0; }"
	     "body {"
	     "font-family: Arial, Helvetica, sans-serif;"
	     "background: var(--bg);"
	     "color: var(--text);"
	     "min-height: 100vh;"
	     "display: flex;"
	     "justify-content: center;"
	     "align-items: center;"
	     "padding: 20px;"
	     "}"
	     ".card {"
	     "width: 100%%;"
	     "max-width: 600px;"
	     "background: var(--bg);"
	     "border: 1px solid var(--border);"
	     "padding: 30px;"
	     "}"
	     ".code {"
	     "font-size: 2.5rem;"
	     "font-weight: bold;"
	     "color: var(--accent);"
	     "}"
	     ".title {"
	     "margin-top: 10px;"
	     "font-size: 1.3rem;"
	     "font-weight: bold;"
	     "}"
	     ".detail {"
	     "margin-top: 12px;"
	     "color: var(--muted);"
	     "line-height: 1.5;"
	     "}"
	     ".footer {"
	     "margin-top: 24px;"
	     "padding-top: 14px;"
	     "border-top: 1px solid var(--border);"
	     "color: var(--muted);"
	     "font-size: 0.85rem;"
	     "}"
	     "</style>"
	     "<script>"
	     "function updateTheme() {"
	     "const dark = window.matchMedia('(prefers-color-scheme: dark)').matches;"
	     "document.body.classList.toggle('dark', dark);"
	     "}"
	     "window.addEventListener('DOMContentLoaded', updateTheme);"
	     "window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', updateTheme);"
	     "</script>"
	     "</head>"
	     "<body>"
	     "<div class='card'>"
	     "<div class='code'>%d</div>"
	     "<div class='title'>%s</div>"
	     "<div class='detail'>%s</div>"
	     "<div class='footer'>" MAIN_BINARY "/" PROJECT_VERSION "</div>"
	     "</div>"
	     "</body>"
	     "</html>",
        status,
        status_text,
        status,
        status_text,
        detail ? detail : "An unexpected error occurred.");

	if ((size_t)blen > sizeof(body)) blen = (int)sizeof(body);
	LOG_DEBUG("Sending error response: %d %s — %s", status, status_text, detail ? detail : "");
	response_send(t, status, status_text, "text/html; charset=utf-8", NULL, body, (size_t) blen, 0, 1);
}

// Send a 301 redirect to the given URL
void response_redirect(Transport *t, const char *location)
{
	char extra[512];
	snprintf(extra, sizeof extra, "Location: %s\r\n", location);
	response_send(t, 301, "Moved Permanently", NULL, extra, NULL, 0, 0, 1);
}
// SSE helpers are in livereload.c (they write directly via transport_write).
// These three wrappers were removed as dead code — no caller used them.
