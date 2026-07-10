/*
 * livereload.h — Live-reload via Server-Sent Events
 */

#ifndef _LIVERELOAD_H_
#define _LIVERELOAD_H_


#include <stdbool.h>

#include "types.h"

typedef struct Transport Transport;

// Start the file-watcher and SSE server
// Returns 0 on success, -1 on failure
int livereload_start(const char *root, LivereloadMode mode, bool poll);

// Handle an incoming SSE connection (sends heartbeat loop)
void livereload_handle_sse(Transport *t);

// Send a reload event to all connected SSE clients
void livereload_broadcast(const char *event);

// Stop the watcher and clean up
void livereload_stop(void);


#endif  // _LIVERELOAD_H_
