/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * header_cache.h — Pre-computed static HTTP header components
 */

#ifndef _HEADER_CACHE_H_
#define _HEADER_CACHE_H_


#include <stddef.h>

// Initialize the header cache (call once at startup)
void header_cache_init(void);

// Build a complete HTTP response header using cached components
// Returns header length, or -1 if buffer too small
int header_cache_build(char *buf, size_t buf_len,
                       int status, const char *status_text,
                       const char *content_type,
                       size_t content_length,
                       const char *extra_headers,
                       int keep_alive);

// Build header without Content-Type (for 304, 204, etc.)
int header_cache_build_no_type(char *buf, size_t buf_len,
                               int status, const char *status_text,
                               size_t content_length,
                               const char *extra_headers,
                               int keep_alive);

// Accessor functions for individual cached components
const char *header_cache_date(void);
const char *header_cache_server(void);
const char *header_cache_conn(int keep_alive);


#endif  // _HEADER_CACHE_H_
