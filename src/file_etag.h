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

void build_etag(const struct stat *st, char *buf, size_t len);
int etag_match(const char *etag, const char *if_none_match);


#endif  // _FILE_ETAG_H_
