/*
 * file_send.h — File sending implementation
 */

#ifndef _FILE_SEND_H_
#define _FILE_SEND_H_

#include "types.h"
#include "http.h"
#include <sys/stat.h>

typedef struct Transport Transport;

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