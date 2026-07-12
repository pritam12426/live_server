/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * file_send.h — File sending with range requests and sendfile
 */

#ifndef _FILE_SEND_H_
#define _FILE_SEND_H_


#include "types.h"
#include "http.h"
#include "transport.h"
#include <sys/stat.h>

/**
 * Send a file over the transport with full HTTP semantics.
 * Handles: ETag/If-None-Match, If-Modified-Since, Range requests,
 * sendfile (Linux), LiveReload injection, keep-alive.
 * @param t                 transport handle
 * @param client_ip         client IP for logging
 * @param client_port       client port for logging
 * @param req               parsed HTTP request
 * @param fs_path           absolute filesystem path to serve
 * @param pre_stat          optional pre-stat() result (for index.html)
 * @param livereload_mode   live-reload configuration
 * @param print_request     whether to log the request
 * @param keep_alive        whether to send Connection: keep-alive
 * @return HTTP status code (200, 206, 304, 404, 416, etc.)
 */
int file_send_file(Transport *t,
                   const char *client_ip,
                   int client_port,
                   const HttpRequest *req,
                   const char *fs_path,
                   const struct stat *pre_stat,
                   LivereloadMode livereload_mode,
                   int print_request,
                   int keep_alive);


#endif  // _FILE_SEND_H_