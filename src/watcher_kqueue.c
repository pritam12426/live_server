/*
 * watcher_kqueue.c — macOS/BSD kqueue backend
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

static int kqueue_add_dir(Watcher *w, const char *path)
{
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

static const char *kqueue_fd_path(Watcher *w, int fd)
{
	for (int i = 0; i < w->watch_count; i++)
		if (w->watches[i].fd == fd)
			return w->watches[i].path;
	return NULL;
}

static void kqueue_remove_dir(Watcher *w, int fd)
{
	for (int i = 0; i < w->watch_count; i++) {
		if (w->watches[i].fd == fd) {
			struct kevent kev;
			EV_SET(&kev, fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
			kevent(w->kq, &kev, 1, NULL, 0, NULL);
			close(fd);
			w->watches[i] = w->watches[w->watch_count - 1];
			w->watch_count--;
			break;
		}
	}
}

int watcher_kqueue_init(Watcher *w)
{
	w->kq = kqueue();
	if (w->kq < 0) return -1;

	watcher_kqueue_watch_recursive(w, w->root, w->ignore_hidden);
	return 0;
}

void *watcher_kqueue_thread(void *arg)
{
	Watcher *w = arg;
	struct kevent events[64];

	while (atomic_load_explicit(&w->running, memory_order_relaxed)) {
		struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
		int nev = kevent(w->kq, NULL, 0, events, 64, &ts);
		if (nev < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (!atomic_load_explicit(&w->running, memory_order_relaxed)) break;

		if (nev == 0) continue;

		char changed_path[4096] = {0};
		int  fired = 0;

		for (int i = 0; i < nev; i++) {
			const struct kevent *ev = &events[i];

			if (ev->ident == (uintptr_t)w->wake_rd) {
				fired = -1;
				break;
			}

			const char *dir = kqueue_fd_path(w, (int)ev->ident);
			if (!dir) continue;

			if (ev->fflags & (NOTE_DELETE | NOTE_RENAME)) {
				kqueue_remove_dir(w, (int)ev->ident);
				continue;
			}

			snprintf(changed_path, sizeof changed_path, "%s", dir);

			if (!fired) fired = 1;
		}

		if (fired == -1) break;
		if (fired && atomic_load_explicit(&w->running, memory_order_relaxed)) {
			LOG_INFO("kqueue change detected: %s", changed_path);
			w->cb(changed_path[0] ? changed_path : NULL, w->userdata);
		}
	}

	return NULL;
}

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