/*
 * file.h — File serving interface
 */

#ifndef _FILE_H_
#define _FILE_H_


#include "http.h"
#include "types.h"

typedef struct Transport Transport;

// Serve a file from the filesystem based on the HTTP request
// Handles conditional requests, range requests, directory index,
// live-reload injection, and keep-alive.
int file_serve(const char        *root,
               const HttpRequest *req,
               Transport         *t,
               const char        *client_ip,
               int                client_port,
               LivereloadMode     livereload_mode,
               int                print_request,
               int                keep_alive);


#endif // _FILE_H_
