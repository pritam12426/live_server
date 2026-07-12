/*
 * watcher.c — File-system watcher (public API)
 *
 * Three backends are compiled in:
 *   inotify (Linux)   — recursive: we watch root AND every subdirectory.
 *					   When a new directory is created we add a watch for it.
 *   kqueue (macOS/BSD) — similar to inotify: we watch root AND every subdirectory
 *					   using EVFILT_VNODE. New directories are added dynamically.
 *   poll fallback	 — walks the directory tree every POLL_INTERVAL_MS ms
 *					   and compares mtimes with the previous snapshot.
 *
 * The watcher runs in its own pthread and calls the user callback whenever
 * a change is detected.
 */

#include "watcher.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"

#if defined(__linux__)
#include "watcher_inotify.h"
#elif defined(__APPLE__)
#include "watcher_kqueue.h"
#endif
#include "watcher_poll.h"

Watcher *watcher_create(const char   *root_dir,
						int		   use_poll,
						int		   ignore_hidden,
						watcher_cb_t  cb,
						void		 *userdata)
{
	Watcher *w = calloc(1, sizeof *w);
	if (!w) return NULL;

	snprintf(w->root, sizeof w->root, "%s", root_dir);
	w->ignore_hidden = ignore_hidden;
	w->cb			= cb;
	w->userdata	  = userdata;
	atomic_store_explicit(&w->running, 0, memory_order_relaxed);

	int pipefds[2];
	if (pipe(pipefds) != 0) { free(w); return NULL; }
	w->wake_rd = pipefds[0];
	w->wake_wr = pipefds[1];

#if defined(__linux__)
	if (!use_poll) {
		w->use_poll  = 0;
		w->ifd	   = inotify_init1(IN_NONBLOCK);
		if (w->ifd < 0) {
			LOG_WARN("inotify_init1 failed, falling back to poll: %m");
			LOG_INFO("File watcher: using poll backend instead of inotify");
			w->use_poll = 1;
		} else {
			watcher_inotify_watch_recursive(w, root_dir, ignore_hidden);
		}
	} else {
		w->use_poll = 1;
	}
#elif defined(__APPLE__)
	if (!use_poll) {
		w->use_poll = 0;
		w->kq = kqueue();
		if (w->kq < 0) {
			LOG_WARN("kqueue() failed, falling back to poll: %m");
			LOG_INFO("File watcher: using poll backend instead of kqueue");
			w->use_poll = 1;
		} else {
			watcher_kqueue_watch_recursive(w, root_dir, ignore_hidden);
		}
	} else {
		w->use_poll = 1;
	}
#else
	w->use_poll = 1;
	(void)use_poll;
#endif

	LOG_DEBUG("Watcher created for %s (poll=%d, ignore_hidden=%d)",
			  root_dir, w->use_poll, ignore_hidden);
	return w;
}

int watcher_start(Watcher *w)
{
	LOG_INFO("Starting file watcher for %s", w->root);
	atomic_store_explicit(&w->running, 1, memory_order_relaxed);

#if defined(__linux__)
	if (!w->use_poll)
		return pthread_create(&w->thread, NULL, watcher_inotify_thread, w);
#elif defined(__APPLE__)
	if (!w->use_poll)
		return pthread_create(&w->thread, NULL, watcher_kqueue_thread, w);
#endif

	return pthread_create(&w->thread, NULL, poll_thread, w);
}

void watcher_stop(Watcher *w)
{
	LOG_INFO("Stopping file watcher for %s", w->root);
	atomic_store_explicit(&w->running, 0, memory_order_relaxed);
	char b = 1;
	write(w->wake_wr, &b, 1);
	pthread_join(w->thread, NULL);
	LOG_DEBUG("File watcher thread joined");
}

void watcher_destroy(Watcher *w)
{
	if (!w) return;
	close(w->wake_rd);
	close(w->wake_wr);

#if defined(__linux__)
	if (!w->use_poll && w->ifd >= 0) {
		watcher_inotify_cleanup(w);
	}
#elif defined(__APPLE__)
	if (!w->use_poll) {
		watcher_kqueue_cleanup(w);
	}
#endif

	free(w);
}
