/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * thread_buffer.c — Thread-local buffer pool
 *
 * Each thread gets its own reusable buffers to avoid malloc/free
 * on the hot path. Buffers grow to accommodate the largest request
 * but are never shrunk (amortized O(1) allocation).
 */

#include "thread_buffer.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"

// Thread-local buffer state
typedef struct {
	char *body_buf;
	size_t body_cap;
} ThreadBuffers;

static __thread ThreadBuffers *g_tls_buffers = NULL;

// Get or create thread-local buffer state
static ThreadBuffers *get_tls_buffers(void)
{
	if (!g_tls_buffers) {
		g_tls_buffers = calloc(1, sizeof(ThreadBuffers));
		if (!g_tls_buffers) return NULL;
		LOG_DEBUG("TLS buffer state allocated for thread");
	}
	return g_tls_buffers;
}

// Get a body buffer of at least min_size
char *thread_buffer_get_body(size_t min_size)
{
	ThreadBuffers *tb = get_tls_buffers();
	if (!tb) return NULL;

	if (min_size > tb->body_cap) {
		// Grow by 2x to amortize allocation cost
		size_t new_cap = tb->body_cap ? tb->body_cap * 2 : 4096;
		while (new_cap < min_size) new_cap *= 2;

		char *new_buf = realloc(tb->body_buf, new_cap);
		if (!new_buf) {
			LOG_ERROR("Failed to grow TLS body buffer to %zu bytes", new_cap);
			return NULL;
		}

		tb->body_buf = new_buf;
		tb->body_cap = new_cap;
		LOG_DEBUG("TLS body buffer grown to %zu bytes", new_cap);
	}
	return tb->body_buf;
}

// Clean up thread-local buffers (call on thread exit)
void thread_buffer_cleanup(void)
{
	if (g_tls_buffers) {
		LOG_DEBUG("Cleaning up TLS buffers (body cap: %zu)", g_tls_buffers->body_cap);
		free(g_tls_buffers->body_buf);
		free(g_tls_buffers);
		g_tls_buffers = NULL;
	}
}
