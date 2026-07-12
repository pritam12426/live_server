/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * header_cache.c — Pre-computed static HTTP header components
 *
 * Static headers (Date, Server, Connection) are cached and updated
 * once per second to avoid repeated formatting and syscalls.
 */

#include "header_cache.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "project_config.h"

// Cached header components
static char hc_date[64];
static char hc_server[128];
static char hc_conn_keep_alive[32] = "Connection: keep-alive\r\n";
static char hc_conn_close[32] = "Connection: close\r\n";

// Mutex for updating cached values
static pthread_mutex_t hc_mutex = PTHREAD_MUTEX_INITIALIZER;

// Update the Date header (called once per second)
static void header_cache_update_date(void)
{
	time_t	now = time(NULL);
	struct tm tm;
	gmtime_r(&now, &tm);
	strftime(hc_date, sizeof hc_date,
	         "Date: %a, %d %b %Y %H:%M:%S GMT\r\n", &tm);
	LOG_DEBUG("Header cache Date updated: %.*s", (int)sizeof(hc_date) - 2, hc_date);
}

// Initialize static headers
void header_cache_init(void)
{
	header_cache_update_date();
	snprintf(hc_server, sizeof hc_server,
	         "Server: " MAIN_BINARY "/" PROJECT_VERSION "\r\n");
	LOG_INFO("Header cache initialized (Server: %s)", MAIN_BINARY "/" PROJECT_VERSION);
}

// Get cached Date header (updates once per second)
const char *header_cache_date(void)
{
	static time_t last_update = 0;
	time_t now = time(NULL);

	if (now != last_update) {
		pthread_mutex_lock(&hc_mutex);
		if (now != last_update) {  // double-check
			header_cache_update_date();
			last_update = now;
			LOG_DEBUG("Date header regenerated");
		}
		pthread_mutex_unlock(&hc_mutex);
	}
	return hc_date;
}

const char *header_cache_server(void)
{
	return hc_server;
}

const char *header_cache_conn(int keep_alive)
{
	return keep_alive ? hc_conn_keep_alive : hc_conn_close;
}

// Build a complete HTTP response header from pre-computed components
int header_cache_build(char *buf, size_t buf_len,
                       int status, const char *status_text,
                       const char *content_type,
                       size_t content_length,
                       const char *extra_headers,
                       int keep_alive)
{
	const char *date = header_cache_date();
	const char *server = header_cache_server();
	const char *conn = header_cache_conn(keep_alive);

	int len = snprintf(buf, buf_len,
	                   "HTTP/1.1 %d %s\r\n"
	                   "%s"
	                   "%s"
	                   "Content-Type: %s\r\n"
	                   "Content-Length: %zu\r\n"
	                   "%s"
	                   "%s"
	                   "\r\n",
	                   status, status_text,
	                   date,
	                   server,
	                   content_type ? content_type : "",
	                   content_length,
	                   extra_headers ? extra_headers : "",
	                   conn);

	if ((size_t)len >= buf_len) return -1;
	return len;
}

// Overload for responses without Content-Type (e.g., 304, 204)
int header_cache_build_no_type(char *buf, size_t buf_len,
                               int status, const char *status_text,
                               size_t content_length,
                               const char *extra_headers,
                               int keep_alive)
{
	const char *date = header_cache_date();
	const char *server = header_cache_server();
	const char *conn = header_cache_conn(keep_alive);

	int len = snprintf(buf, buf_len,
	                   "HTTP/1.1 %d %s\r\n"
	                   "%s"
	                   "%s"
	                   "Content-Length: %zu\r\n"
	                   "%s"
	                   "%s"
	                   "\r\n",
	                   status, status_text,
	                   date,
	                   server,
	                   content_length,
	                   extra_headers ? extra_headers : "",
	                   conn);

	if ((size_t)len >= buf_len) return -1;
	return len;
}
