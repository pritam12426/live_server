/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * file.c — File serving interface
 */

#include "file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"
#include "mime.h"
#include "response.h"
#include "transport.h"
#include "thread_buffer.h"

#include "file_send.h"

static char g_real_root[4096];

static void file_serve_set_root(const char *root)
{
	if (realpath(root, g_real_root) == NULL)
		g_real_root[0] = '\0';
}

static int safe_path(const char *root, const char *url_path, char *out, size_t out_len)
{
	if (g_real_root[0] == '\0') {
		if (realpath(root, g_real_root) == NULL)
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

int file_serve(const char        *root,
               const HttpRequest *req,
               Transport         *t,
               const char        *client_ip,
               int                client_port,
               LivereloadMode     livereload_mode,
               int                print_request,
               int                keep_alive)
{
	if (req->method != HTTP_GET && req->method != HTTP_HEAD) {
		response_error(t, 405, "Method Not Allowed", "Only GET and HEAD are supported.");
		if (print_request) {
			LOG_INFO("%s:%d \"%s %s %s\" %d",
			         client_ip, client_port,
			         req->method_str, req->path, req->version, 405);
		}
		return 405;
	}

	if (g_real_root[0] == '\0')
		file_serve_set_root(root);

	char fs_path[4096];
	if (!safe_path(root, req->path, fs_path, sizeof fs_path)) {
		response_error(t, 403, "Forbidden", "Path traversal detected.");
		if (print_request) {
			LOG_INFO("%s:%d \"%s %s %s\" %d",
			         client_ip, client_port,
			         req->method_str, req->path, req->version, 403);
		}
		return 403;
	}

	struct stat st;
	if (stat(fs_path, &st) != 0) {
		LOG_WARN("File not found: %s (resolved to %s)", req->path, fs_path);
		response_error(t, 404, "Not Found", "Not found.");
		if (print_request) {
			LOG_INFO("%s:%d \"%s %s %s\" %d",
			         client_ip, client_port,
			         req->method_str, req->path, req->version, 404);
		}
		return 404;
	}

	if (S_ISDIR(st.st_mode)) {
		size_t plen = strlen(req->path);
		if (plen == 0 || req->path[plen - 1] != '/') {
			char redir[4096];
			snprintf(redir, sizeof redir, "%s/", req->path);
			LOG_DEBUG("Redirecting to trailing-slash: %s -> %s", req->path, redir);
			response_redirect(t, redir);
			if (print_request) {
				LOG_INFO("%s:%d \"%s %s %s\" %d",
				         client_ip, client_port,
				         req->method_str, req->path, req->version, 301);
			}
			return 301;
		}

		char index_path[4096];
		snprintf(index_path, sizeof index_path, "%s/index.html", fs_path);
		struct stat idx_st;
		if (stat(index_path, &idx_st) == 0 && S_ISREG(idx_st.st_mode)) {
			LOG_DEBUG("Serving index.html: %s", index_path);
			return file_send_file(t, client_ip, client_port, req, index_path,
			                      &idx_st, livereload_mode, print_request, keep_alive);
		}

		LOG_WARN("No index.html in %s", req->path);
		response_error(t, 404, "Not Found", "index.html not found.");
		if (print_request) {
			LOG_INFO("%s:%d \"%s %s %s\" %d",
			         client_ip, client_port,
			         req->method_str, req->path, req->version, 404);
		}
		return 404;
	}

	if (!S_ISREG(st.st_mode)) {
		LOG_DEBUG("Not a regular file: %s", req->path);
		response_error(t, 403, "Forbidden", "Not a regular file.");
		if (print_request) {
			LOG_INFO("%s:%d \"%s %s %s\" %d",
			         client_ip, client_port,
			         req->method_str, req->path, req->version, 403);
		}
		return 403;
	}

	LOG_DEBUG("Serving file: %s -> fd=%d", req->path, transport_fd(t));
	return file_send_file(t, client_ip, client_port, req, fs_path, &st, livereload_mode, print_request, keep_alive);
}
