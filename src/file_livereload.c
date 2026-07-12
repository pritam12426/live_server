/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * file_livereload.c — LiveReload script injection
 */

#include "file_livereload.h"

#include <string.h>
#include "mime.h"

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
			&& (p[10] == 'y' || p[10] == 'Y')
			&& (p[11] == '>'))
			return p;
	}
	return NULL;
}

int file_livereload_should_inject(const char *mime, LivereloadMode mode)
{
	return mode != LIVERELOAD_OFF && strstr(mime, "text/html") != NULL;
}

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

	memcpy(out, body, body_len);
	out[body_len] = '\0';

	char *closing = find_body_close(out);
	if (closing) {
		memmove(closing + script_len, closing, body_len - (size_t)(closing - out) + 1);
		memcpy(closing, LIVERELOAD_SCRIPT, script_len);
		*out_len_ptr = body_len + script_len;
	} else {
		memcpy(out + body_len, LIVERELOAD_SCRIPT, script_len);
		*out_len_ptr = body_len + script_len;
	}
}
