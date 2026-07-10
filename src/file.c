#include "file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "mime.h"
#include "project_config.h"
#include "response.h"
#include "transport.h"

static const char LIVERELOAD_SCRIPT[] =
	"<script>\n"
	"(function(){\n"
	"	var es = new EventSource('/livereload');\n"
	"		es.onmessage = function(e) {\n"
	"		if (e.data === 'reload') {\n"
	"			window.location.reload();\n"
	"		} else if (e.data === 'hard-reload') {\n"
	"			var url = new URL(window.location.href);\n"
	"			url.searchParams.set('_lr', Date.now());\n"
	"			window.location.replace(url.toString());\n"
	"		}\n"
	"	};"
	"})();\n"
	"</script>\n";

static void build_etag(const struct stat *st, char *buf, size_t len)
{
	snprintf(buf, len, "\"%lx-%lx\"",
	            (unsigned long)st->st_mtime,
	            (unsigned long)st->st_size);
}

static void http_date(time_t t, char *buf, size_t len)
{
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(buf, len, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static int safe_path(const char *root, const char *url_path, char *out, size_t out_len)
{
	char real_root[4096];
	if (realpath(root, real_root) == NULL)
		return 0;

	size_t root_len = strlen(real_root);

	char tmp[4096];
	snprintf(tmp, sizeof tmp, "%s%s", root, url_path);

	char resolved[4096];
	if (realpath(tmp, resolved) != NULL) {
		if (strncmp(resolved, real_root, root_len) != 0)
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

	if (strncmp(resolved_dir, real_root, root_len) != 0)
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

static void log_request(const char        *client_ip,
                        int                client_port,
                        const HttpRequest *req,
                        int                status,
                        long long          bytes,
                        const char        *mime)
{
	// int is_remote = client_ip && strcmp(client_ip, "127.0.0.1") != 0
	//              && strcmp(client_ip, "::1") != 0
	//              && strcmp(client_ip, "localhost") != 0;

	if (bytes >= 0 && mime) {
		LOG_INFO("%s:%d \"%s %s %s\" %d - (%lld bytes, %s)",
		         client_ip,
		         client_port,
		         req->method_str,
		         req->path,
		         req->version,
		         status,
		         bytes,
		         mime);
	} else {
		LOG_INFO("%s:%d \"%s %s %s\" %d",
		         client_ip,
		         client_port,
		         req->method_str,
		         req->path,
		         req->version,
		         status);
	}
}

static int send_file(Transport          *t,
                     const char        *client_ip,
                     int                client_port,
                     const HttpRequest *req,
                     const char        *fs_path,
                     LivereloadMode     livereload_mode,
                     int                print_request,
                     int                keep_alive)
{
	struct stat st;
	if (stat(fs_path, &st) != 0) {
		response_error(t, 404, "Not Found", "File not found.");
		if (print_request)
			log_request(client_ip, client_port, req, 404, -1, NULL);
		return 404;
	}

	const char *mime    = mime_from_path(fs_path);
	int         is_html = (strstr(mime, "text/html") != NULL);

	char etag[64];
	build_etag(&st, etag, sizeof etag);

	char last_modified[64];
	http_date(st.st_mtime, last_modified, sizeof last_modified);

	if (req->if_none_match[0] && strcmp(req->if_none_match, etag) == 0) {
		LOG_INFO("%s:%d \"%s\" 304 (ETag match)", client_ip, client_port, req->path);
		char extra[256];
		snprintf(extra, sizeof extra, "ETag: %s\r\nLast-Modified: %s\r\n", etag, last_modified);
		response_send(t, 304, "Not Modified", NULL, extra, NULL, 0, keep_alive, 1);
		if (print_request)
			log_request(client_ip, client_port, req, 304, -1, NULL);
		return 304;
	}

	if (req->if_modified_since[0] && !req->if_none_match[0]) {
		struct tm tm_ims = { 0 };
		if (strptime(req->if_modified_since, "%a, %d %b %Y %H:%M:%S GMT", &tm_ims)) {
			time_t ims_t = timegm(&tm_ims);
			if (st.st_mtime <= ims_t) {
				LOG_INFO("%s:%d \"%s\" 304 (If-Modified-Since)", client_ip, client_port, req->path);
				char extra[256];
				snprintf(extra,
				         sizeof extra,
				         "ETag: %s\r\nLast-Modified: %s\r\n",
				         etag,
				         last_modified);
				response_send(t, 304, "Not Modified", NULL, extra, NULL, 0, keep_alive, 1);
				if (print_request)
					log_request(client_ip, client_port, req, 304, -1, NULL);
				return 304;
			}
		}
	}

	long file_size = (long) st.st_size;
	// int is_remote = strcmp(client_ip, "127.0.0.1") != 0
	//              && strcmp(client_ip, "::1") != 0
	//              && strcmp(client_ip, "localhost") != 0;
	LOG_INFO("%s:%d \"%s %s\" (%s, %ld bytes)",
	         client_ip, client_port, req->method_str, req->path, mime, file_size);

	int fd = open(fs_path, O_RDONLY);
	if (fd < 0) {
		LOG_WARN("Failed to open file: %s (fd=%d)", req->path, transport_fd(t));
		response_error(t, 403, "Forbidden", "Cannot open file.");
		if (print_request)
			log_request(client_ip, client_port, req, 403, -1, NULL);
		return 403;
	}

	if (is_html && livereload_mode != LIVERELOAD_OFF) {
		LOG_DEBUG("Injecting live-reload script into %s", req->path);
		size_t script_len = strlen(LIVERELOAD_SCRIPT);
		char  *body       = malloc((size_t) file_size + script_len + 32);
		if (!body) {
			close(fd);
			response_error(t, 500, "Internal Server Error", "Out of memory.");
			return 500;
		}

		ssize_t n = read(fd, body, (size_t) file_size);
		close(fd);
		size_t body_len = (n > 0) ? (size_t) n : 0;
		body[body_len]  = '\0';

		char *closing = strcasestr(body, "</body>");
		if (!closing)
			closing = strcasestr(body, "</BODY>");
		if (closing) {
			memmove(closing + script_len, closing, body_len - (size_t) (closing - body) + 1);
			memcpy(closing, LIVERELOAD_SCRIPT, script_len);
			body_len += script_len;
		} else {
			memcpy(body + body_len, LIVERELOAD_SCRIPT, script_len);
			body_len += script_len;
		}

		char extra[512];
		snprintf(extra,
		         sizeof extra,
		         "ETag: %s\r\nLast-Modified: %s\r\nCache-Control: no-store\r\n",
		         etag,
		         last_modified);
		response_send(t, 200, "OK", mime, extra, body, body_len, keep_alive,
		              req->method != HTTP_HEAD);
		if (print_request)
			log_request(client_ip, client_port, req, 200, (long long) body_len, mime);
		free(body);
		return 200;
	}

	long range_first = 0, range_last = file_size - 1;
	int  is_range = 0;

	if (req->range_start != -1) {
		if (req->range_start < 0) {
			range_first = file_size + req->range_start;
			range_last  = file_size - 1;
		} else {
			range_first = (long) req->range_start;
			range_last  = (req->range_end >= 0) ? (long) req->range_end : file_size - 1;
		}

		if (range_first < 0)
			range_first = 0;
		if (range_last >= file_size)
			range_last = file_size - 1;

		if (range_first > range_last || range_first >= file_size) {
			close(fd);
			LOG_WARN("Range not satisfiable: %ld-%ld (file size %ld)",
			         range_first,
			         range_last,
			         file_size);
			char extra[128];
			snprintf(extra, sizeof extra, "Content-Range: bytes */%ld\r\n", file_size);
			response_send(t, 416, "Range Not Satisfiable", NULL, extra, NULL, 0, keep_alive, 1);
			if (print_request)
				log_request(client_ip, client_port, req, 416, -1, NULL);
			return 416;
		}
		LOG_INFO("Range request: bytes=%ld-%ld of %ld", range_first, range_last, file_size);
		is_range = 1;
	}

	long send_len = range_last - range_first + 1;
	if (range_first > 0)
		lseek(fd, (off_t) range_first, SEEK_SET);

	char *body = malloc((size_t) send_len);
	if (!body) {
		close(fd);
		response_error(t, 500, "Internal Server Error", "Out of memory.");
		return 500;
	}

	ssize_t nread = read(fd, body, (size_t) send_len);
	close(fd);

	if (nread <= 0) {
		free(body);
		response_error(t, 500, "Internal Server Error", "Read error.");
		return 500;
	}
	size_t body_len = (size_t) nread;
	char extra[512];
	if (is_range) {
		snprintf(extra,
		         sizeof extra,
		         "ETag: %s\r\nLast-Modified: %s\r\n"
		         "Accept-Ranges: bytes\r\n"
		         "Content-Range: bytes %ld-%ld/%ld\r\n",
		         etag,
		         last_modified,
		         range_first,
		         range_last,
		         file_size);
	} else {
		snprintf(extra,
		         sizeof extra,
		         "ETag: %s\r\nLast-Modified: %s\r\nAccept-Ranges: bytes\r\n",
		         etag,
		         last_modified);
	}

	int status = is_range ? 206 : 200;
	response_send(t,
	              status,
	              is_range ? "Partial Content" : "OK",
	              mime,
	              extra,
	              body,
	              body_len,
	              keep_alive,
	              req->method != HTTP_HEAD);
	if (print_request)
		log_request(client_ip, client_port, req, status, (long long) body_len, mime);
	free(body);
	return status;
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
		if (print_request)
			log_request(client_ip, client_port, req, 405, -1, NULL);
		return 405;
	}

	char fs_path[4096];
	if (!safe_path(root, req->path, fs_path, sizeof fs_path)) {
		response_error(t, 403, "Forbidden", "Path traversal detected.");
		if (print_request)
			log_request(client_ip, client_port, req, 403, -1, NULL);
		return 403;
	}

	struct stat st;
	if (stat(fs_path, &st) != 0) {
		LOG_WARN("File not found: %s (resolved to %s)", req->path, fs_path);
		response_error(t, 404, "Not Found", "Not found.");
		if (print_request)
			log_request(client_ip, client_port, req, 404, -1, NULL);
		return 404;
	}

	if (S_ISDIR(st.st_mode)) {
		size_t plen = strlen(req->path);
		if (plen == 0 || req->path[plen - 1] != '/') {
			char redir[4096];
			snprintf(redir, sizeof redir, "%s/", req->path);
			LOG_DEBUG("Redirecting to trailing-slash: %s → %s", req->path, redir);
			response_redirect(t, redir);
			if (print_request)
				log_request(client_ip, client_port, req, 301, -1, NULL);
			return 301;
		}

		char index_path[4096];
		snprintf(index_path, sizeof index_path, "%s/index.html", fs_path);
		struct stat idx_st;
		if (stat(index_path, &idx_st) == 0 && S_ISREG(idx_st.st_mode)) {
			LOG_DEBUG("Serving index.html: %s", index_path);
			return send_file(t, client_ip, client_port, req, index_path,
			                 livereload_mode, print_request, keep_alive);
		}

		LOG_WARN("No index.html in %s", req->path);
		response_error(t, 404, "Not Found", "index.html not found.");
		if (print_request)
			log_request(client_ip, client_port, req, 404, -1, NULL);
		return 404;
	}

	if (!S_ISREG(st.st_mode)) {
		LOG_DEBUG("Not a regular file: %s", req->path);
		response_error(t, 403, "Forbidden", "Not a regular file.");
		if (print_request)
			log_request(client_ip, client_port, req, 403, -1, NULL);
		return 403;
	}

	LOG_DEBUG("Serving file: %s → fd=%d", req->path, transport_fd(t));
	return send_file(t, client_ip, client_port, req, fs_path, livereload_mode, print_request, keep_alive);
}
