/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * file_etag.h — ETag generation and comparison
 */

#ifndef _FILE_ETAG_H_
#define _FILE_ETAG_H_


#include <stddef.h>
#include <sys/stat.h>

/**
 * Build an ETag string from file metadata.
 * Format: "mtime-hex-size-hex" (both lowercase hex, wrapped in quotes).
 * @param st  stat result containing st_mtime and st_size
 * @param buf output buffer (must be at least 4 bytes)
 * @param len size of output buffer
 */
void build_etag(const struct stat *st, char *buf, size_t len);

/**
 * Compare an ETag with If-None-Match header value.
 * @param etag          generated ETag (quoted)
 * @param if_none_match value of If-None-Match header (may be unquoted)
 * @return 1 if they match (indicating 304 Not Modified), 0 otherwise
 */
int etag_match(const char *etag, const char *if_none_match);


#endif  // _FILE_ETAG_H_