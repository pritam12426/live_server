/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * thread_pool.h — Fixed-size thread pool with work queue
 *
 * Replaces pthread-per-connection with a pool of reusable worker threads.
 * Tasks are submitted via thread_pool_submit() and executed by the first
 * available worker.
 */

#ifndef _THREAD_POOL_H_
#define _THREAD_POOL_H_


#include <stddef.h>

typedef void (*ThreadTaskFunc)(void *arg);

typedef struct {
	ThreadTaskFunc func;
	void          *arg;
} ThreadTask;

typedef struct ThreadPool ThreadPool;

// Create a pool with num_threads workers (clamped 1–256)
ThreadPool *thread_pool_create(int num_threads);

// Submit a task for asynchronous execution
void thread_pool_submit(ThreadPool *pool, ThreadTaskFunc func, void *arg);

// Stop all workers and free resources
void thread_pool_destroy(ThreadPool *pool);


#endif  // _THREAD_POOL_H_
