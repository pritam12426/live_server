/*
 * livereload.c — Server-Sent Events (SSE) live-reload system
 *
 * Maintains a registry of SSE clients, starts a file-system watcher,
 * and broadcasts "reload" or "hard-reload" events when files change.
 */

#include "livereload.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "response.h"
#include "transport.h"
#include "watcher.h"

// Maximum number of simultaneous SSE connections
#define LR_MAX_CLIENTS 256

// A single SSE client entry in the registry
typedef struct {
	Transport *transport;
} LRClient;

// Global SSE client registry (protected by lr_mutex)
static LRClient         lr_clients[LR_MAX_CLIENTS];
static pthread_mutex_t  lr_mutex  = PTHREAD_MUTEX_INITIALIZER;
static atomic_int       lr_stop      = 0;   // Shutdown flag
static Watcher         *lr_watcher   = NULL; // File-system watcher instance
static void            *lr_userdata  = NULL; // Freed on shutdown

// Register an SSE client in the first free slot
// Returns the slot index, or -1 if the table is full
static int lr_add_client(Transport *t)
{
	pthread_mutex_lock(&lr_mutex);
	for (int i = 0; i < LR_MAX_CLIENTS; i++) {
		if (lr_clients[i].transport == NULL) {
			lr_clients[i].transport = t;
			pthread_mutex_unlock(&lr_mutex);
			LOG_INFO("SSE client connected (slot=%d, fd=%d)", i, transport_fd(t));
			return i;
		}
	}
	pthread_mutex_unlock(&lr_mutex);
	LOG_WARN("SSE client table full (%d slots), rejecting fd=%d", LR_MAX_CLIENTS, transport_fd(t));
	return -1;
}

// Remove an SSE client from the registry by slot index
static void lr_remove_client(int slot)
{
	pthread_mutex_lock(&lr_mutex);
	Transport *t = lr_clients[slot].transport;
	lr_clients[slot].transport = NULL;
	pthread_mutex_unlock(&lr_mutex);
	if (t) {
		LOG_INFO("SSE client disconnected (slot=%d, fd=%d)", slot, transport_fd(t));
	}
}

// Broadcast an SSE event to all connected clients
// Removes clients whose transport write fails
void livereload_broadcast(const char *event)
{
	char frame[128];
	int  flen = snprintf(frame, sizeof frame, "data: %s\n\n", event);
	int  sent = 0;

	pthread_mutex_lock(&lr_mutex);
	for (int i = 0; i < LR_MAX_CLIENTS; i++) {
		Transport *t = lr_clients[i].transport;
		if (t == NULL) continue;
		if (transport_write(t, frame, (size_t)flen) <= 0) {
			LOG_WARN("SSE broadcast to fd=%d failed, removing client", transport_fd(t));
			lr_clients[i].transport = NULL;
		} else {
			sent++;
		}
	}
	pthread_mutex_unlock(&lr_mutex);
	LOG_DEBUG("SSE broadcast '%s' to %d client(s)", event, sent);
}

// File-change callback invoked by the watcher
// Sends the appropriate reload event based on mode
static void on_change(const char *path, void *userdata)
{
	LivereloadMode mode = *(LivereloadMode *)userdata;
	(void)path;
	const char *event = (mode == LIVERELOAD_HARD_RELOAD) ? "hard-reload" : "reload";
	LOG_DEBUG("File change detected — broadcasting '%s'", event);
	livereload_broadcast(event);
}

// Start the live-reload system: initialise client registry + file watcher
// Returns 0 on success, -1 on error
int livereload_start(const char *root, LivereloadMode mode, bool poll)
{
	// Clear client registry
	for (int i = 0; i < LR_MAX_CLIENTS; i++)
		lr_clients[i].transport = NULL;
	atomic_store_explicit(&lr_stop, 0, memory_order_relaxed);

	// Allocate a copy of the mode so the callback can use it
	lr_userdata = NULL;
	LivereloadMode *mode_copy = malloc(sizeof *mode_copy);
	if (!mode_copy) return -1;
	*mode_copy = mode;
	lr_userdata = mode_copy;

	// Create the file-system watcher
	lr_watcher = watcher_create(root,
								(int)poll,
								/*ignore_hidden=*/0,
								on_change,
								mode_copy);
	if (!lr_watcher) {
		free(mode_copy);
		return -1;
	}

	// Start the watcher thread
	if (watcher_start(lr_watcher) != 0) {
		watcher_destroy(lr_watcher);
		lr_watcher = NULL;
		free(mode_copy);
		return -1;
	}

	// Log which backend is being used
	const char *lr_backend_str;
#if defined(__linux__)
	lr_backend_str = poll ? "poll" : "inotify";
#elif defined(__APPLE__) && defined(__MACH__)
	lr_backend_str = "poll";
	(void)poll;
#endif  // __linux__

	LOG_INFO("Live-reload watching: %s (%s)",
			 root,
			 lr_backend_str);
	return 0;
}

// Stop the live-reload system: stop watcher, free resources, clear clients
void livereload_stop(void)
{
	atomic_store_explicit(&lr_stop, 1, memory_order_relaxed);

	if (lr_watcher) {
		watcher_stop(lr_watcher);
		watcher_destroy(lr_watcher);
		lr_watcher = NULL;
	}

	free(lr_userdata);
	lr_userdata = NULL;

	// Clear all client references (don't close transports — let server.c do it)
	pthread_mutex_lock(&lr_mutex);
	for (int i = 0; i < LR_MAX_CLIENTS; i++) {
		lr_clients[i].transport = NULL;
	}
	pthread_mutex_unlock(&lr_mutex);
}

// Handle an SSE connection: send headers, register client, heartbeat loop
void livereload_handle_sse(Transport *t)
{
	static const char SSE_HEADERS[] =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream\r\n"
		"Cache-Control: no-cache\r\n"
		"Connection: keep-alive\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"\r\n";

	if (transport_write(t, SSE_HEADERS, sizeof(SSE_HEADERS) - 1) <= 0)
		return;

	int slot = lr_add_client(t);
	if (slot < 0) {
		transport_write(t, ": server-full\n\n", 15);
		return;
	}

	LOG_DEBUG("SSE heartbeat loop started for fd=%d (slot=%d)", transport_fd(t), slot);

	// Send heartbeats every 15 s to keep the connection alive
	static const char HEARTBEAT[] = ": heartbeat\n\n";
	while (!atomic_load_explicit(&lr_stop, memory_order_relaxed)) {
		struct timespec ts = { 15, 0 };
		nanosleep(&ts, NULL);

		if (transport_write(t, HEARTBEAT, sizeof(HEARTBEAT) - 1) <= 0) {
			LOG_DEBUG("SSE client fd=%d disconnected", transport_fd(t));
			break;
		}
		LOG_DEBUG("SSE heartbeat sent to fd=%d", transport_fd(t));
	}

	lr_remove_client(slot);
}
