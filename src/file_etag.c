/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * file_etag.c — ETag generation and comparison
 *
 * ETag format: "mtime-hex-size-hex" (both lowercase hex, wrapped in double quotes)
 * Example: "5f3c2a1b-1000"
 * - mtime: file modification time as hex
 * - size:  file size as hex
 * This format is compact, fast to compute, and changes when either mtime or size changes.
 */

#include "file_etag.h"

#include <string.h>

static const char *hex_digits = "0123456789abcdef";

/**
 * Build an ETag string from file metadata.
 * Format: "mtime-hex-size-hex" (both lowercase hex, wrapped in quotes).
 * @param st  stat result containing st_mtime and st_size
 * @param buf output buffer (must be at least 4 bytes)
 * @param len size of output buffer
 */
void build_etag(const struct stat *st, char *buf, size_t len)
{
	if (len < 4) { if (len) *buf = '\0'; return; }
	unsigned long mtime = (unsigned long)st->st_mtime;
	unsigned long size  = (unsigned long)st->st_size;
	size_t pos = 0;

	buf[pos++] = '"';

	/* Write mtime as hex (8 bytes for 32-bit, up to 16 for 64-bit) */
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

	/* Write size as hex */
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

/**
 * Compare an ETag with If-None-Match header value.
 * @param etag          generated ETag (quoted)
 * @param if_none_match value of If-None-Match header (may be unquoted)
 * @return 1 if they match (indicating 304 Not Modified), 0 otherwise
 */
int etag_match(const char *etag, const char *if_none_match)
{
	return etag && if_none_match[0] && strcmp(if_none_match, etag) == 0;
}