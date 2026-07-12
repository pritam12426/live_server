/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * file_etag.c — ETag generation and comparison
 */

#include "file_etag.h"

#include <string.h>

static const char *hex_digits = "0123456789abcdef";

void build_etag(const struct stat *st, char *buf, size_t len)
{
	if (len < 4) { if (len) *buf = '\0'; return; }
	unsigned long mtime = (unsigned long)st->st_mtime;
	unsigned long size  = (unsigned long)st->st_size;
	size_t pos = 0;

	buf[pos++] = '"';

	char mtime_buf[16];
	int mtime_len = 0;
	if (mtime == 0) {
		mtime_buf[mtime_len++] = '0';
	} else {
		char tmp[16];
		int idx = 0;
		while (mtime > 0) {
			tmp[idx++] = hex_digits[mtime & 0xF];
			mtime >>= 4;
		}
		while (idx > 0) {
			mtime_buf[mtime_len++] = tmp[--idx];
		}
	}
	if (pos + (size_t)mtime_len >= len) { buf[0] = '\0'; return; }
	memcpy(buf + pos, mtime_buf, (size_t)mtime_len);
	pos += (size_t)mtime_len;

	if (pos >= len - 3) { buf[0] = '\0'; return; }
	buf[pos++] = '-';

	if (size == 0) {
		buf[pos++] = '0';
	} else {
		char tmp[16];
		int idx = 0;
		while (size > 0) {
			tmp[idx++] = hex_digits[size & 0xF];
			size >>= 4;
		}
		if (pos + (size_t)idx >= len) { buf[0] = '\0'; return; }
		while (idx > 0) {
			buf[pos++] = tmp[--idx];
		}
	}

	if (pos >= len - 1) { buf[0] = '\0'; return; }
	buf[pos++] = '"';
	buf[pos] = '\0';
}

int etag_match(const char *etag, const char *if_none_match)
{
	return etag && if_none_match[0] && strcmp(if_none_match, etag) == 0;
}
