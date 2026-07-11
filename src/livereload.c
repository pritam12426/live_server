/*
 * livereload.c — Server-Sent Events (SSE) live-reload system
 *
 * Maintains a registry of SSE clients using a hash map for O(1) operations,
 * starts a file-system watcher, and broadcasts "reload" or "hard-reload"
 * events when files change.
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

// Hash map configuration
#define LR_MAP_SIZE 1024  // Power of 2 for bitwise modulo
#define LR_MAP_MASK (LR_MAP_SIZE - 1)

// SSE client entry in hash map
typedef struct LRClient {
	Transport       *transport;
	int              fd;    // Cached fd for logging
	struct LRClient *next;  // Collision chain
} LRClient;

// Hash map buckets (each bucket is head of collision chain)
static LRClient *lr_buckets[LR_MAP_SIZE];
static pthread_mutex_t lr_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int lr_stop = 0;
static Watcher *lr_watcher = NULL;
static void *lr_userdata = NULL;
static atomic_int lr_client_count = 0;

// Hash function for file descriptor
static inline size_t lr_hash_fd(int fd)
{
	return (size_t)fd & LR_MAP_MASK;
}

// Register an SSE client in the hash map
// Returns 0 on success, -1 if map is full (shouldn't happen with LR_MAP_SIZE=1024)
static int lr_add_client(Transport *t)
{
	int fd = transport_fd(t);

	pthread_mutex_lock(&lr_mutex);

	// Check if we're at capacity (very unlikely with 1024 slots)
	int current_count = atomic_load_explicit(&lr_client_count, memory_order_relaxed);
	if (current_count >= LR_MAP_SIZE) {
		pthread_mutex_unlock(&lr_mutex);
		LOG_WARN("SSE client map full (%d slots), rejecting fd=%d", LR_MAP_SIZE, fd);
		return -1;
	}

	LRClient *client = malloc(sizeof(LRClient));
	if (!client) {
		pthread_mutex_unlock(&lr_mutex);
		return -1;
	}

	client->transport = t;
	client->fd = fd;

	size_t bucket = lr_hash_fd(fd);
	client->next = lr_buckets[bucket];
	lr_buckets[bucket] = client;

	atomic_fetch_add_explicit(&lr_client_count, 1, memory_order_relaxed);

	pthread_mutex_unlock(&lr_mutex);

	LOG_INFO("SSE client connected (fd=%d, total=%d)", fd, current_count + 1);
	return 0;
}

// Remove an SSE client from the hash map by fd
static void lr_remove_client(int fd)
{
	pthread_mutex_lock(&lr_mutex);

	size_t bucket = lr_hash_fd(fd);
	LRClient **pp = &lr_buckets[bucket];

	while (*pp) {
		if ((*pp)->fd == fd) {
			LRClient *to_free = *pp;
			*pp = to_free->next;
			free(to_free);
			atomic_fetch_sub_explicit(&lr_client_count, 1, memory_order_relaxed);
			LOG_INFO("SSE client disconnected (fd=%d, remaining=%d)",
			         fd, atomic_load_explicit(&lr_client_count, memory_order_relaxed) - 1);
			break;
		}
		pp = &(*pp)->next;
	}

	pthread_mutex_unlock(&lr_mutex);
}

// Broadcast an SSE event to all connected clients
// Removes clients whose transport write fails
void livereload_broadcast(const char *event)
{
	char frame[128];
	int flen = snprintf(frame, sizeof frame, "data: %s\n\n", event);
	int sent = 0;

	pthread_mutex_lock(&lr_mutex);

	for (size_t i = 0; i < LR_MAP_SIZE; i++) {
		LRClient *client = lr_buckets[i];
		LRClient **pp = &lr_buckets[i];

		while (client) {
			if (transport_write(client->transport, frame, (size_t)flen) <= 0) {
				LOG_WARN("SSE broadcast to fd=%d failed, removing client", client->fd);
				LRClient *to_free = client;
				*pp = client->next;
				client = client->next;
				free(to_free);
				atomic_fetch_sub_explicit(&lr_client_count, 1, memory_order_relaxed);
			} else {
				sent++;
				pp = &client->next;
				client = client->next;
			}
		}
	}

	pthread_mutex_unlock(&lr_mutex);
	LOG_DEBUG("SSE broadcast '%s' to %d client(s)", event, sent);
}

// File-change callback invoked by the watcher
static void on_change(const char *path, void *userdata)
{
	LivereloadMode mode = *(LivereloadMode *)userdata;
	(void)path;
	const char *event = (mode == LIVERELOAD_HARD_RELOAD) ? "hard-reload" : "reload";
	LOG_DEBUG("File change detected — broadcasting '%s'", event);
	livereload_broadcast(event);
}

// Start the live-reload system: initialise client registry + file watcher
int livereload_start(const char *root, LivereloadMode mode, bool poll)
{
	// Clear hash map
	for (size_t i = 0; i < LR_MAP_SIZE; i++)
		lr_buckets[i] = NULL;
	atomic_store_explicit(&lr_client_count, 0, memory_order_relaxed);
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
#elif defined(__APPLE__)
	lr_backend_str = "poll";
	(void)poll;
#endif

	LOG_INFO("Live-reload watching: %s (%s)", root, lr_backend_str);
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

	// Clear all client references and free memory
	pthread_mutex_lock(&lr_mutex);
	for (size_t i = 0; i < LR_MAP_SIZE; i++) {
		LRClient *client = lr_buckets[i];
		while (client) {
			LRClient *next = client->next;
			free(client);
			client = next;
		}
		lr_buckets[i] = NULL;
	}
	atomic_store_explicit(&lr_client_count, 0, memory_order_relaxed);
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

	if (lr_add_client(t) < 0) {
		transport_write(t, ": server-full\n\n", 15);
		return;
	}

	int fd = transport_fd(t);
	LOG_DEBUG("SSE heartbeat loop started for fd=%d", fd);

	// Send heartbeats every 15 s to keep the connection alive
	static const char HEARTBEAT[] = ": heartbeat\n\n";
	while (!atomic_load_explicit(&lr_stop, memory_order_relaxed)) {
		struct timespec ts = { 15, 0 };
		nanosleep(&ts, NULL);

		if (transport_write(t, HEARTBEAT, sizeof(HEARTBEAT) - 1) <= 0) {
			LOG_DEBUG("SSE client fd=%d disconnected", fd);
			break;
		}
		LOG_DEBUG("SSE heartbeat sent to fd=%d", fd);
	}

	lr_remove_client(fd);
}
