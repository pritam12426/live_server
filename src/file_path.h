/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * file_path.h — Path resolution and traversal protection
 */

#ifndef _FILE_PATH_H_
#define _FILE_PATH_H_


#include <stddef.h>

/**
 * Resolve and cache the absolute root path.
 * Called once at startup.
 * @param root  configured document root
 * @return 1 on success, 0 on failure
 */
int file_path_resolve_root(const char *root);

/**
 * Safely resolve a URL path to a filesystem path within the document root.
 * Prevents directory traversal attacks (../, symlinks outside root).
 * @param root     configured document root
 * @param url_path request path (e.g., "/images/logo.png")
 * @param out      output buffer for resolved absolute path
 * @param out_len  size of output buffer
 * @return 1 if path is safe and resolved, 0 if traversal detected or error
 */
int file_path_safe(const char *root, const char *url_path, char *out, size_t out_len);


#endif  // _FILE_PATH_H_