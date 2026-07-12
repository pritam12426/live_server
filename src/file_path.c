/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * file_path.c — Path resolution and safety
 */

#include "file_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_real_root[4096];

int file_path_resolve_root(const char *root)
{
	if (realpath(root, g_real_root) == NULL) {
		g_real_root[0] = '\0';
		return 0;
	}
	return 1;
}

int file_path_safe(const char *root, const char *url_path, char *out, size_t out_len)
{
	if (g_real_root[0] == '\0') {
		if (!file_path_resolve_root(root))
			return 0;
	}

	size_t root_len = strlen(g_real_root);

	char tmp[4096];
	snprintf(tmp, sizeof tmp, "%s%s", root, url_path);

	char resolved[4096];
	if (realpath(tmp, resolved) != NULL) {
		if (strncmp(resolved, g_real_root, root_len) != 0)
			return 0;
		if (resolved[root_len] != '/' && resolved[root_len] != '\0')
			return 0;
		snprintf(out, out_len, "%s", resolved);
		return 1;
	}

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

	if (strncmp(resolved_dir, g_real_root, root_len) != 0)
		return 0;
	if (resolved_dir[root_len] != '/' && resolved_dir[root_len] != '\0')
		return 0;

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
