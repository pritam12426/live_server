/*
 * thread_pool.c — Fixed-size thread pool with work queue
 *
 * Each worker pulls tasks from a circular buffer protected by a mutex.
 * Two condition variables signal "not empty" (workers wake to grab work)
 * and "not full" (producers wait when the queue is full).
 *
 * On destroy, the stop flag is set and all workers are joined.
 */

#include "thread_pool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "log.h"

#define QUEUE_CAPACITY 4096

struct ThreadPool {
	pthread_t      *threads;
	int             num_threads;
	ThreadTask      queue[QUEUE_CAPACITY];
	int             head;
	int             tail;
	int             count;
	pthread_mutex_t lock;
	pthread_cond_t  not_empty;
	pthread_cond_t  not_full;
	atomic_int      stop;
};

static void *worker_loop(void *arg)
{
	ThreadPool *pool = arg;

	while (1) {
		pthread_mutex_lock(&pool->lock);

		while (pool->count == 0 && !atomic_load_explicit(&pool->stop, memory_order_relaxed))
			pthread_cond_wait(&pool->not_empty, &pool->lock);

		if (pool->count == 0 && atomic_load_explicit(&pool->stop, memory_order_relaxed)) {
			pthread_mutex_unlock(&pool->lock);
			break;
		}

		ThreadTask task = pool->queue[pool->head];
		pool->head = (pool->head + 1) % QUEUE_CAPACITY;
		pool->count--;
		int depth = pool->count;

		pthread_cond_signal(&pool->not_full);
		pthread_mutex_unlock(&pool->lock);

		LOG_DEBUG("Worker picked up task (queue depth: %d)", depth);
		task.func(task.arg);
	}

	return NULL;
}

ThreadPool *thread_pool_create(int num_threads)
{
	if (num_threads < 1) num_threads = 1;
	if (num_threads > 256) num_threads = 256;

	ThreadPool *pool = calloc(1, sizeof(*pool));
	if (!pool) return NULL;

	pool->num_threads = num_threads;
	pthread_mutex_init(&pool->lock, NULL);
	pthread_cond_init(&pool->not_empty, NULL);
	pthread_cond_init(&pool->not_full, NULL);
	atomic_store_explicit(&pool->stop, 0, memory_order_relaxed);

	pool->threads = calloc((size_t)num_threads, sizeof(pthread_t));
	if (!pool->threads) {
		free(pool);
		return NULL;
	}

	for (int i = 0; i < num_threads; i++) {
		if (pthread_create(&pool->threads[i], NULL, worker_loop, pool) != 0) {
			LOG_PERROR("pthread_create in thread_pool");
			atomic_store_explicit(&pool->stop, 1, memory_order_relaxed);
			for (int j = 0; j < i; j++)
				pthread_join(pool->threads[j], NULL);
			free(pool->threads);
			free(pool);
			return NULL;
		}
	}

	LOG_DEBUG("Thread pool created with %d workers", num_threads);
	return pool;
}

void thread_pool_submit(ThreadPool *pool, ThreadTaskFunc func, void *arg)
{
	if (!pool) return;

	pthread_mutex_lock(&pool->lock);

	while (pool->count == QUEUE_CAPACITY && !atomic_load_explicit(&pool->stop, memory_order_relaxed))
		pthread_cond_wait(&pool->not_full, &pool->lock);

	if (atomic_load_explicit(&pool->stop, memory_order_relaxed)) {
		pthread_mutex_unlock(&pool->lock);
		return;
	}

	pool->queue[pool->tail] = (ThreadTask){ .func = func, .arg = arg };
	pool->tail = (pool->tail + 1) % QUEUE_CAPACITY;
	pool->count++;

	LOG_DEBUG("Task submitted to pool (queue depth: %d)", pool->count);

	pthread_cond_signal(&pool->not_empty);
	pthread_mutex_unlock(&pool->lock);
}

void thread_pool_destroy(ThreadPool *pool)
{
	if (!pool) return;

	atomic_store_explicit(&pool->stop, 1, memory_order_relaxed);

	pthread_cond_broadcast(&pool->not_empty);
	pthread_cond_broadcast(&pool->not_full);

	for (int i = 0; i < pool->num_threads; i++)
		pthread_join(pool->threads[i], NULL);

	pthread_mutex_destroy(&pool->lock);
	pthread_cond_destroy(&pool->not_empty);
	pthread_cond_destroy(&pool->not_full);

	free(pool->threads);
	free(pool);

	LOG_DEBUG("Thread pool destroyed");
}
