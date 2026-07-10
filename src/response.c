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
#include "header_cache.h"

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
	char hdr[4096];
	int  hdr_len;

	if (mime) {
		hdr_len = header_cache_build(hdr, sizeof hdr,
		                             status, status_text,
		                             mime, body_len,
		                             extra_hdrs, keep_alive);
	} else {
		hdr_len = header_cache_build_no_type(hdr, sizeof hdr,
		                                     status, status_text,
		                                     body_len,
		                                     extra_hdrs, keep_alive);
	}

	if (hdr_len < 0) {
		LOG_ERROR("Header buffer too small");
		return;
	}

	LOG_DEBUG("HTTP %d %s -> fd=%d (%d header + %zu body bytes)",
	          status, status_text, transport_fd(t), hdr_len, body_len);

	if (send_body && body && body_len > 0) {
		// Use writev for single syscall
		struct iovec iov[2] = {
			{ .iov_base = hdr, .iov_len = (size_t)hdr_len },
			{ .iov_base = (void *)body, .iov_len = body_len }
		};
		transport_writev(t, iov, 2);
	} else {
		transport_write(t, hdr, (size_t) hdr_len);
	}
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
