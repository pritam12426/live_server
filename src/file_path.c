/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * file_path.c — Path resolution and traversal protection
 *
 * Resolves request paths to absolute filesystem paths while preventing
 * directory traversal attacks (../, symlinks outside root, etc.)
 */

#include "file_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Cached absolute root path (resolved once at startup) */
static char g_real_root[4096];

/**
 * Resolve and cache the absolute root path.
 * Called once at startup.
 * @param root  configured document root
 * @return 1 on success, 0 on failure
 */
int file_path_resolve_root(const char *root)
{
	if (realpath(root, g_real_root) == NULL) {
		g_real_root[0] = '\0';
		return 0;
	}
	return 1;
}

/**
 * Safely resolve a URL path to a filesystem path within the document root.
 * Prevents directory traversal attacks (../, symlinks outside root).
 * @param root     configured document root
 * @param url_path request path (e.g., "/images/logo.png")
 * @param out      output buffer for resolved absolute path
 * @param out_len  size of output buffer
 * @return 1 if path is safe and resolved, 0 if traversal detected or error
 */
int file_path_safe(const char *root, const char *url_path, char *out, size_t out_len)
{
	/* Resolve root on first use */
	if (g_real_root[0] == '\0') {
		if (!file_path_resolve_root(root))
			return 0;
	}

	size_t root_len = strlen(g_real_root);

	/* Try direct realpath resolution first */
	char tmp[4096];
	snprintf(tmp, sizeof tmp, "%s%s", root, url_path);

	char resolved[4096];
	if (realpath(tmp, resolved) != NULL) {
		/* Verify resolved path is within root */
		if (strncmp(resolved, g_real_root, root_len) != 0)
			return 0;
		if (resolved[root_len] != '/' && resolved[root_len] != '\0')
			return 0;
		snprintf(out, out_len, "%s", resolved);
		return 1;
	}

	/* Fallback: resolve directory component, validate filename separately */
	char path_copy[4096];
	snprintf(path_copy, sizeof path_copy, "%s", tmp);

	char *last_slash = strrchr(path_copy, '/');
	if (!last_slash) return 0;

	char *filename = last_slash + 1;
	*last_slash = '\0';

	char *dir_part = path_copy;
	if (*dir_part == '\0')
		dir_part = "/";

	char resolved_dir[4096];
	if (realpath(dir_part, resolved_dir) == NULL)
		return 0;

	/* Verify directory is within root */
	if (strncmp(resolved_dir, g_real_root, root_len) != 0)
		return 0;
	if (resolved_dir[root_len] != '/' && resolved_dir[root_len] != '\0')
		return 0;

	/* Check filename for .. traversal attempts */
	const char *p = filename;
	while ((p = strstr(p, "..")) != NULL) {
		if ((p == filename || *(p - 1) == '/') &&
		    (*(p + 2) == '/' || *(p + 2) == '\0'))
			return 0;
		p += 2;
	}

	snprintf(out, out_len, "%s/%s", resolved_dir, filename);
	return 1;
}
