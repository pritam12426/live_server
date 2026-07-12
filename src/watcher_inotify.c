/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * watcher_inotify.c — Linux inotify file watcher backend
 *
 * Uses inotify(7) to monitor filesystem events recursively.
 * Watches root directory and all subdirectories; adds watches dynamically
 * when new directories are created.
 */

#include "watcher_inotify.h"

#if defined(__linux__)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"

#define INOTIFY_BUF_LEN (4096 * (sizeof(struct inotify_event) + 16 + 1))

/**
 * Add a single directory to the inotify watch set.
 * @param w     watcher handle (contains ifd and watch array)
 * @param path  absolute path of directory to watch
 * @return watch descriptor on success, -1 on error
 */
static int inotify_add_dir(Watcher *w, const char *path)
{
	int wd = inotify_add_watch(w->ifd, path,
	                           IN_CREATE | IN_DELETE | IN_MODIFY |
	                           IN_MOVED_FROM | IN_MOVED_TO |
	                           IN_CLOSE_WRITE);
	if (wd < 0) return -1;

	/* Grow watch array if needed */
	if (w->watch_count >= w->watch_cap) {
		int new_cap = w->watch_cap ? w->watch_cap * 2 : 64;
		struct WdEntry *tmp = realloc(w->watches, (size_t)new_cap * sizeof *tmp);
		if (!tmp) return -1;
		w->watches   = tmp;
		w->watch_cap = new_cap;
	}

	w->watches[w->watch_count].wd = wd;
	snprintf(w->watches[w->watch_count].path,
	         sizeof w->watches[w->watch_count].path,
	         "%s", path);
	w->watch_count++;
	return wd;
}

/**
 * Recursively add all directories under path to the inotify watch set.
 * @param w             watcher handle
 * @param path          directory to watch
 * @param ignore_hidden skip dotfiles/directories
 */
void watcher_inotify_watch_recursive(Watcher *w, const char *path, int ignore_hidden)
{
	inotify_add_dir(w, path);

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
			watcher_inotify_watch_recursive(w, sub, ignore_hidden);
	}
	closedir(d);
}

/**
 * Look up the directory path for a given inotify watch descriptor.
 * @param w  watcher handle
 * @param wd watch descriptor to find
 * @return directory path, or NULL if not found
 */
static const char *inotify_wd_path(Watcher *w, int wd)
{
	for (int i = 0; i < w->watch_count; i++)
		if (w->watches[i].wd == wd)
			return w->watches[i].path;
	return NULL;
}

/**
 * Initialize inotify subsystem for a watcher.
 * @param w watcher handle
 * @return 0 on success, -1 on failure
 */
int watcher_inotify_init(Watcher *w)
{
	w->ifd = inotify_init1(IN_NONBLOCK);
	if (w->ifd < 0) return -1;

	watcher_inotify_watch_recursive(w, w->root, w->ignore_hidden);
	return 0;
}

/**
 * Inotify watcher thread: waits for events from inotify or wake pipe.
 * Calls user callback on filesystem changes.
 * @param arg watcher handle
 * @return NULL (thread exit)
 */
void *watcher_inotify_thread(void *arg)
{
	Watcher *w = arg;
	char buf[INOTIFY_BUF_LEN] __attribute__((aligned(8)));

	while (atomic_load_explicit(&w->running, memory_order_relaxed)) {
		/* Wait on inotify fd and wake pipe simultaneously */
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(w->ifd,    &rfds);
		FD_SET(w->wake_rd, &rfds);
		int maxfd = (w->ifd > w->wake_rd ? w->ifd : w->wake_rd) + 1;

		struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
		int ret = select(maxfd, &rfds, NULL, NULL, &tv);
		if (ret < 0) break;
		if (!atomic_load_explicit(&w->running, memory_order_relaxed)) break;

		/* Wake pipe signaled - shutdown requested */
		if (FD_ISSET(w->wake_rd, &rfds)) break;

		/* Timeout - recheck running flag */
		if (!FD_ISSET(w->ifd, &rfds)) continue;

		/* Read all available inotify events */
		ssize_t len = read(w->ifd, buf, sizeof buf);
		if (len <= 0) continue;

		char         changed_path[4096] = {0};
		int          fired              = 0;
		const char  *p                  = buf;

		while (p < buf + len) {
			const struct inotify_event *ev =
				(const struct inotify_event *)p;
			p += sizeof(struct inotify_event) + ev->len;

			if (ev->mask & IN_IGNORED) continue;

			const char *dir = inotify_wd_path(w, ev->wd);
			if (!dir) continue;

			/* Build full path of changed file/directory */
			if (ev->len > 0) {
				(void)snprintf(changed_path, sizeof changed_path,
				               "%s/%s", dir, ev->name);
			} else {
				(void)snprintf(changed_path, sizeof changed_path, "%s", dir);
			}

			/* If a new directory was created, add it to watch set */
			if ((ev->mask & (IN_CREATE | IN_MOVED_TO)) &&
			    ev->len > 0) {
				struct stat st;
				if (stat(changed_path, &st) == 0 && S_ISDIR(st.st_mode))
					watcher_inotify_watch_recursive(w, changed_path, w->ignore_hidden);
			}

			if (!fired) {
				fired = 1;
			}
		}

		if (fired && atomic_load_explicit(&w->running, memory_order_relaxed)) {
			LOG_INFO("inotify change detected: %s", changed_path);
			w->cb(changed_path[0] ? changed_path : NULL, w->userdata);
		}
	}

	return NULL;
}

/**
 * Clean up inotify resources.
 * @param w watcher handle
 */
void watcher_inotify_cleanup(Watcher *w)
{
	if (w->ifd >= 0) {
		close(w->ifd);
		w->ifd = -1;
	}
	free(w->watches);
	w->watches = NULL;
	w->watch_count = 0;
	w->watch_cap = 0;
}

#endif  // __linux__