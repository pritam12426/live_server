/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * watcher_poll.c — Portable polling fallback file watcher
 *
 * Periodically scans the entire directory tree and compares mtimes
 * against a previous snapshot to detect changes. Used when inotify/kqueue
 * are unavailable or when --poll flag is specified.
 */

#include "watcher_poll.h"

#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

#define POLL_INTERVAL_MS 500
#define PMAP_SIZE 4096

/**
 * Create a new hash map for path → mtime lookups.
 * Uses open addressing with linear probing.
 * @return allocated PollMap, or NULL on OOM
 */
struct PollMap *pmap_create(void)
{
	struct PollMap *m = calloc(1, sizeof(*m));
	if (m) pmap_clear(m);
	return m;
}

/**
 * Destroy a PollMap and free its memory.
 * @param m map to destroy
 */
void pmap_destroy(struct PollMap *m)
{
	free(m);
}

/**
 * Clear all entries in the map (reset to empty).
 * @param m map to clear
 */
void pmap_clear(struct PollMap *m)
{
	memset(m->used, 0, sizeof(m->used));
}

/**
 * Simple djb2 hash function for strings.
 * @param s null-terminated string
 * @return 32-bit hash value
 */
static unsigned pmap_hash(const char *s)
{
	unsigned h = 5381;
	for (; *s; s++)
		h = h * 33 + (unsigned char)*s;
	return h;
}

/**
 * Insert or update a path → mtime mapping.
 * @param m   map to insert into
 * @param path file/directory path (used as key)
 * @param mt  modification time to store
 */
void pmap_insert(struct PollMap *m, const char *path, time_t mt)
{
	unsigned idx = pmap_hash(path) & (PMAP_SIZE - 1);
	char k[256];
	snprintf(k, sizeof k, "%s", path);
	for (int i = 0; i < PMAP_SIZE; i++) {
		unsigned slot = (idx + (unsigned)i) & (PMAP_SIZE - 1);
		if (!m->used[slot]) {
			memcpy(m->keys[slot], k, sizeof(m->keys[slot]));
			m->vals[slot] = mt;
			m->used[slot] = 1;
			return;
		}
		if (strcmp(m->keys[slot], k) == 0) {
			m->vals[slot] = mt;
			return;
		}
	}
}

/**
 * Look up a path's mtime in the map.
 * @param m       map to search
 * @param path    path to look up
 * @param out_mt  receives mtime if found
 * @return 1 if found, 0 if not found
 */
int pmap_get(struct PollMap *m, const char *path, time_t *out_mt)
{
	char k[64];
	snprintf(k, sizeof k, "%s", path);
	unsigned idx = pmap_hash(path) & (PMAP_SIZE - 1);
	for (int i = 0; i < PMAP_SIZE; i++) {
		unsigned slot = (idx + (unsigned)i) & (PMAP_SIZE - 1);
		if (!m->used[slot]) return 0;
		if (strcmp(m->keys[slot], k) == 0) {
			*out_mt = m->vals[slot];
			return 1;
		}
	}
	return 0;
}

/**
 * Recursively build a snapshot of all files and their mtimes under root.
 * Stores entries in a dynamically-growing array.
 * @param ps          PollState to populate
 * @param root        root directory to scan
 * @param ignore_hidden skip dotfiles/directories
 */
void poll_snapshot(struct PollState *ps, const char *root, int ignore_hidden)
{
	DIR *d = opendir(root);
	if (!d) return;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
		if (ignore_hidden && ent->d_name[0] == '.') continue;

		char path[4096];
		snprintf(path, sizeof path, "%s/%s", root, ent->d_name);

		struct stat st;
		if (stat(path, &st) != 0) continue;

		/* Grow entries array if needed */
		if (ps->count >= ps->cap) {
			int new_cap = ps->cap ? ps->cap * 2 : 256;
			struct PollEntry *tmp = realloc(ps->entries,
			                                (size_t)new_cap * sizeof *tmp);
			if (!tmp) { closedir(d); return; }
			ps->entries = tmp;
			ps->cap     = new_cap;
		}
		snprintf(ps->entries[ps->count].path,
		         sizeof ps->entries[ps->count].path, "%s", path);
		ps->entries[ps->count].mtime = st.st_mtime;
		ps->count++;

		/* Recurse into subdirectories */
		if (S_ISDIR(st.st_mode))
			poll_snapshot(ps, path, ignore_hidden);
	}
	closedir(d);
}

/**
 * Sleep for specified milliseconds, aborting early if wake_fd becomes readable.
 * Used to implement interruptible polling interval.
 * @param ms       milliseconds to sleep
 * @param wake_fd  file descriptor to monitor for wake signal
 */
void poll_sleep_ms(int ms, int wake_fd)
{
	struct timeval tv;
	tv.tv_sec  = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;

	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(wake_fd, &rfds);

	while (tv.tv_sec > 0 || tv.tv_usec > 0) {
		FD_ZERO(&rfds);
		FD_SET(wake_fd, &rfds);
		int ret = select(wake_fd + 1, &rfds, NULL, NULL, &tv);
		if (ret > 0) break;       /* wake_fd readable - shutdown signal */
		if (ret == 0) break;      /* timeout elapsed */
		if (errno != EINTR) break; /* error */
		/* EINTR - continue with updated timeout */
	}
}

/**
 * Poll watcher thread: periodically snapshots directory tree and diffs mtimes.
 * Calls user callback when any file changes, is added, or removed.
 * @param arg watcher handle
 * @return NULL (thread exit)
 */
void *poll_thread(void *arg)
{
	Watcher   *w = arg;
	struct PollState  prev = {0}, curr = {0};
	struct PollMap   *pmap = pmap_create();
	if (!pmap) return NULL;

	/* Take initial snapshot */
	poll_snapshot(&prev, w->root, w->ignore_hidden);
	LOG_DEBUG("Poll watcher initial snapshot: %d entries", prev.count);

	/* Build initial hash map for O(1) lookups */
	for (int i = 0; i < prev.count; i++)
		pmap_insert(pmap, prev.entries[i].path, prev.entries[i].mtime);

	while (atomic_load_explicit(&w->running, memory_order_relaxed)) {
		poll_sleep_ms(POLL_INTERVAL_MS, w->wake_rd);
		if (!atomic_load_explicit(&w->running, memory_order_relaxed)) break;

		/* Take new snapshot */
		curr.count = 0;
		poll_snapshot(&curr, w->root, w->ignore_hidden);

		/* Compare: check if count differs, or any mtime changed, or new file */
		int changed = (curr.count != prev.count);
		if (!changed) {
			for (int i = 0; i < curr.count && !changed; i++) {
				time_t prev_mt;
				if (!pmap_get(pmap, curr.entries[i].path, &prev_mt)) {
					changed = 1;  /* new file not in previous snapshot */
				} else if (curr.entries[i].mtime != prev_mt) {
					changed = 1;  /* mtime changed */
				}
			}
		}

		if (changed && atomic_load_explicit(&w->running, memory_order_relaxed)) {
			LOG_DEBUG("Poll watcher detected file change (curr=%d prev=%d)",
			          curr.count, prev.count);
			w->cb(NULL, w->userdata);  /* NULL path = poll mode */
		}

		/* Swap prev and curr (reuse memory) */
		struct PollEntry *tmp = prev.entries;
		prev.entries   = curr.entries;
		prev.count     = curr.count;
		prev.cap       = curr.cap;
		curr.entries   = tmp;
		curr.count     = 0;
		curr.cap       = prev.cap;
		if (curr.entries == NULL) curr.cap = 0;

		/* Rebuild hash map for next comparison */
		pmap_clear(pmap);
		for (int i = 0; i < prev.count; i++)
			pmap_insert(pmap, prev.entries[i].path, prev.entries[i].mtime);
	}

	/* Cleanup */
	free(prev.entries);
	free(curr.entries);
	pmap_destroy(pmap);
	return NULL;
}
