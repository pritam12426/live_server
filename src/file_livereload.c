/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * file_livereload.c — LiveReload script injection
 *
 * Injects a small JavaScript snippet into HTML responses before </body>
 * to enable automatic browser reload on file changes.
 */

#include "file_livereload.h"

#include <string.h>
#include "mime.h"

/* LiveReload client script - connects to /livereload SSE endpoint */
static const char LIVERELOAD_SCRIPT[] =
	"<script>\n"
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
	"</script>\n";

/**
 * Find the closing </body> tag in HTML (case-insensitive).
 * Single-pass scan for the sequence "</body>".
 * @param haystack HTML content (must be null-terminated)
 * @return pointer to '<' of "</body>", or NULL if not found
 */
static char *find_body_close(char *haystack)
{
	for (char *p = haystack; *p; p++) {
		if (*(p) == '<'
			&& (p[1] == 'b' || p[1] == 'B')
			&& (p[2] == 'o' || p[2] == 'O')
			&& (p[3] == 'd' || p[3] == 'D')
			&& (p[4] == 'y' || p[4] == 'Y')
			&& (p[5] == '>')
			&& (p[6] == '/')
			&& (p[7] == 'b' || p[7] == 'B')
			&& (p[8] == 'o' || p[8] == 'O')
			&& (p[9] == 'd' || p[9] == 'D')
			&& (p[10] == 'y' || p[10] == 'D')
			&& (p[11] == '>'))
			return p;
	}
	return NULL;
}

/**
 * Check if LiveReload injection should be applied.
 * @param mime  response MIME type
 * @param mode  server LiveReload mode
 * @return 1 if injection should occur, 0 otherwise
 */
int file_livereload_should_inject(const char *mime, LivereloadMode mode)
{
	return mode != LIVERELOAD_OFF && strstr(mime, "text/html") != NULL;
}

/**
 * Inject LiveReload script into HTML body before </body> tag.
 * If </body> not found, appends to end of body.
 * @param body        original HTML body (null-terminated)
 * @param body_len    length of body in bytes
 * @param out         output buffer (must hold body_len + script_len + 32)
 * @param out_len     size of output buffer
 * @param out_len_ptr receives length of modified body
 */
void file_livereload_inject(const char *body, size_t body_len,
                             char *out, size_t out_len,
                             size_t *out_len_ptr)
{
	size_t script_len = strlen(LIVERELOAD_SCRIPT);
	size_t total_len = body_len + script_len + 32;

	if (total_len >= out_len) {
		*out_len_ptr = 0;
		return;
	}

	/* Copy body to output buffer and null-terminate for scanning */
	memcpy(out, body, body_len);
	out[body_len] = '\0';

	size_t new_len = 0;
	char *closing = find_body_close(out);
	if (closing) {
		/* Insert script before </body> */
		memmove(closing + script_len, closing, body_len - (size_t)(closing - out) + 1);
		memcpy(closing, LIVERELOAD_SCRIPT, script_len);
		new_len = body_len + script_len;
	} else {
		/* No </body> found - append to end */
		memcpy(out + body_len, LIVERELOAD_SCRIPT, script_len);
		new_len = body_len + script_len;
	}

	*out_len_ptr = new_len;
}