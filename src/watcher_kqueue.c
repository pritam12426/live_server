/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * watcher_kqueue.c — macOS/BSD kqueue file watcher backend
 *
 * Uses kqueue(2) with EVFILT_VNODE to monitor filesystem events recursively.
 * Watches root directory and all subdirectories; adds watches dynamically
 * when new directories are created.
 */

#include "watcher_kqueue.h"

#if defined(__APPLE__)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "log.h"

/**
 * Add a single directory to the kqueue watch set.
 * Opens directory with O_EVTONLY and registers for VNODE events.
 * @param w    watcher handle (contains kq and watch array)
 * @param path absolute path of directory to watch
 * @return directory fd on success, -1 on error
 */
static int kqueue_add_dir(Watcher *w, const char *path)
{
	/* O_EVTONLY: open for event monitoring only (no read/write) */
	int fd = open(path, O_EVTONLY | O_DIRECTORY);
	if (fd < 0) return -1;

	struct kevent kev;
	EV_SET(&kev, fd, EVFILT_VNODE,
	       EV_ADD | EV_CLEAR,
	       NOTE_DELETE | NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB |
	       NOTE_LINK | NOTE_RENAME | NOTE_REVOKE,
	       0, NULL);

	if (kevent(w->kq, &kev, 1, NULL, 0, NULL) < 0) {
		close(fd);
		return -1;
	}

	/* Grow watch array if needed */
	if (w->watch_count >= w->watch_cap) {
		int new_cap = w->watch_cap ? w->watch_cap * 2 : 64;
		struct KqEntry *tmp = realloc(w->watches, (size_t)new_cap * sizeof *tmp);
		if (!tmp) { close(fd); return -1; }
		w->watches   = tmp;
		w->watch_cap = new_cap;
	}

	w->watches[w->watch_count].fd = fd;
	snprintf(w->watches[w->watch_count].path,
	         sizeof w->watches[w->watch_count].path,
	         "%s", path);
	w->watch_count++;
	return fd;
}

/**
 * Recursively add all directories under path to the kqueue watch set.
 * @param w             watcher handle
 * @param path          directory to watch
 * @param ignore_hidden skip dotfiles/directories
 */
void watcher_kqueue_watch_recursive(Watcher *w, const char *path, int ignore_hidden)
{
	kqueue_add_dir(w, path);

	DIR *d = opendir(path);
	if (!d) return;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
		if (ignore_hidden && ent->d_name[0] == '.') continue;

		char sub[4096];
		snprintf(sub, sizeof sub, "%s/%s", path, ent->d_name);

		struct stat st;
		if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode))
			watcher_kqueue_watch_recursive(w, sub, ignore_hidden);
	}
	closedir(d);
}

/**
 * Look up the directory path for a given watched directory fd.
 * @param w  watcher handle
 * @param fd directory fd to find
 * @return directory path, or NULL if not found
 */
static const char *kqueue_fd_path(Watcher *w, int fd)
{
	for (int i = 0; i < w->watch_count; i++)
		if (w->watches[i].fd == fd)
			return w->watches[i].path;
	return NULL;
}

/**
 * Remove a directory from the watch set when it's deleted.
 * @param w  watcher handle
 * @param fd directory fd to remove
 */
static void kqueue_remove_dir(Watcher *w, int fd)
{
	for (int i = 0; i < w->watch_count; i++) {
		if (w->watches[i].fd == fd) {
			struct kevent kev;
			EV_SET(&kev, fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
			kevent(w->kq, &kev, 1, NULL, 0, NULL);
			close(fd);
			/* Remove from array by swapping with last */
			w->watches[i] = w->watches[w->watch_count - 1];
			w->watch_count--;
			break;
		}
	}
}

/**
 * Initialize kqueue subsystem for a watcher.
 * @param w watcher handle
 * @return 0 on success, -1 on failure
 */
int watcher_kqueue_init(Watcher *w)
{
	w->kq = kqueue();
	if (w->kq < 0) return -1;

	watcher_kqueue_watch_recursive(w, w->root, w->ignore_hidden);
	return 0;
}

/**
 * Kqueue watcher thread: waits for events from kqueue or wake pipe.
 * Calls user callback on filesystem changes.
 * @param arg watcher handle
 * @return NULL (thread exit)
 */
void *watcher_kqueue_thread(void *arg)
{
	Watcher *w = arg;
	struct kevent events[64];

	while (atomic_load_explicit(&w->running, memory_order_relaxed)) {
		/* Wait for events with timeout to recheck running flag */
		struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
		int nev = kevent(w->kq, NULL, 0, events, 64, &ts);
		if (nev < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (!atomic_load_explicit(&w->running, memory_order_relaxed)) break;

		if (nev == 0) continue;  /* Timeout */

		char changed_path[4096] = {0};
		int  fired = 0;

		for (int i = 0; i < nev; i++) {
			const struct kevent *ev = &events[i];

			/* Check if this is our wake pipe */
			if (ev->ident == (uintptr_t)w->wake_rd) {
				fired = -1;  /* Shutdown signal */
				break;
			}

			/* Directory fd changed */
			const char *dir = kqueue_fd_path(w, (int)ev->ident);
			if (!dir) continue;

			/* Directory deleted or renamed - remove from watch set */
			if (ev->fflags & (NOTE_DELETE | NOTE_RENAME)) {
				kqueue_remove_dir(w, (int)ev->ident);
				continue;
			}

			/* Other events (write, extend, attrib, link, revoke) -
			 * kqueue doesn't tell us the specific filename, just the directory.
			 * Report the directory path. */
			snprintf(changed_path, sizeof changed_path, "%s", dir);

			if (!fired) fired = 1;
		}

		if (fired == -1) break;  /* Shutdown */
		if (fired && atomic_load_explicit(&w->running, memory_order_relaxed)) {
			LOG_INFO("kqueue change detected: %s", changed_path);
			w->cb(changed_path[0] ? changed_path : NULL, w->userdata);
		}
	}

	return NULL;
}

/**
 * Clean up kqueue resources.
 * @param w watcher handle
 */
void watcher_kqueue_cleanup(Watcher *w)
{
	if (w->kq >= 0) {
		close(w->kq);
		w->kq = -1;
	}
	for (int i = 0; i < w->watch_count; i++)
		close(w->watches[i].fd);
	free(w->watches);
	w->watches = NULL;
	w->watch_count = 0;
	w->watch_cap = 0;
}

#endif  // __APPLE__