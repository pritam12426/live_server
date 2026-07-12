/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * file_path.h — Path resolution and safety
 */

#ifndef _FILE_PATH_H_
#define _FILE_PATH_H_


#include <stddef.h>

int file_path_resolve_root(const char *root);
int file_path_safe(const char *root, const char *url_path, char *out, size_t out_len);


#endif  // _FILE_PATH_H_
