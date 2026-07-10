/*
 * watcher.c — File-system watcher
 *
 * Two backends are compiled in:
 *   inotify (Linux)  — recursive: we watch root AND every subdirectory.
 *                       When a new directory is created we add a watch for it.
 *   poll fallback    — walks the directory tree every POLL_INTERVAL_MS ms
 *                       and compares mtimes with the previous snapshot.
 *
 * The watcher runs in its own pthread and calls the user callback whenever
 * a change is detected.
 */

#include "watcher.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

/* ------------------------------------------------------------------ */
/*  inotify backend (Linux only)                                        */
/* ------------------------------------------------------------------ */
#if defined(__linux__)
#include <sys/inotify.h>

#define INOTIFY_BUF_LEN (4096 * (sizeof(struct inotify_event) + 16 + 1))

// Map from inotify watch descriptor → directory path
typedef struct WdEntry {
	int  wd;
	char path[4096];
} WdEntry;

// inotify state: fd + dynamic array of watched directories
typedef struct {
	int       ifd;         /* inotify fd               */
	WdEntry  *watches;     /* dynamic array             */
	int       watch_count;
	int       watch_cap;
} InotifyState;

// Add a single directory to the inotify watch set
static int inotify_add_dir(InotifyState *s, const char *path)
{
	int wd = inotify_add_watch(s->ifd, path,
                               IN_CREATE | IN_DELETE | IN_MODIFY |
                               IN_MOVED_FROM | IN_MOVED_TO |
                               IN_CLOSE_WRITE);
	if (wd < 0) return -1;

	// Grow the watch array if needed (dynamic)
	if (s->watch_count >= s->watch_cap) {
		int new_cap = s->watch_cap ? s->watch_cap * 2 : 64;
		WdEntry *tmp = realloc(s->watches, (size_t)new_cap * sizeof *tmp);
		if (!tmp) return -1;
		s->watches   = tmp;
		s->watch_cap = new_cap;
	}

	s->watches[s->watch_count].wd = wd;
	snprintf(s->watches[s->watch_count].path,
			 sizeof s->watches[s->watch_count].path,
			 "%s", path);
	s->watch_count++;
	return wd;
}

// Recursively add all directories under root to the watch set
static void inotify_watch_recursive(InotifyState *s,
                                    const char   *path,
									int           ignore_hidden)
{
	inotify_add_dir(s, path);

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
			inotify_watch_recursive(s, sub, ignore_hidden);
	}
	closedir(d);
}

// Look up the path for a given inotify watch descriptor
static const char *inotify_wd_path(InotifyState *s, int wd)
{
	for (int i = 0; i < s->watch_count; i++)
		if (s->watches[i].wd == wd)
			return s->watches[i].path;
	return NULL;
}

#endif  // __linux__

/* ------------------------------------------------------------------ */
/*  Poll backend (portable fallback)                                    */
/* ------------------------------------------------------------------ */

// How often to check for changes (milliseconds)
#define POLL_INTERVAL_MS 500

// A single file/directory entry in the poll snapshot
typedef struct PollEntry {
	char   path[4096];
	time_t mtime;
} PollEntry;

// Simple open-addressing hash map: path → mtime for O(1) lookups during diff
#define PMAP_SIZE 4096
typedef struct PollMap {
	char   keys[PMAP_SIZE][64]; // short hash keys for fast comparison
	time_t vals[PMAP_SIZE];
	int    used[PMAP_SIZE];
} PollMap;

static void pmap_clear(PollMap *m)
{
	memset(m->used, 0, sizeof(m->used));
}

static unsigned pmap_hash(const char *s)
{
	unsigned h = 5381;
	for (; *s; s++)
		h = h * 33 + (unsigned char)*s;
	return h;
}

static void pmap_insert(PollMap *m, const char *path, time_t mt)
{
	unsigned idx = pmap_hash(path) & (PMAP_SIZE - 1);
	// Store first 63 chars of basename as key for comparison
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	char k[64];
	snprintf(k, sizeof k, "%s", base);
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

static int pmap_get(PollMap *m, const char *path, time_t *out_mt)
{
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	char k[64];
	snprintf(k, sizeof k, "%s", base);
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

// Poll state: dynamic array of entries
typedef struct {
	PollEntry *entries;
	int        count;
	int        cap;
} PollState;

// Recursively build a snapshot of all files and their mtimes under root
static void poll_snapshot(PollState *ps, const char *root, int ignore_hidden)
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

		// Add to snapshot (grow array if needed)
		if (ps->count >= ps->cap) {
			int new_cap = ps->cap ? ps->cap * 2 : 256;
			PollEntry *tmp = realloc(ps->entries,
									 (size_t)new_cap * sizeof *tmp);
			if (!tmp) { closedir(d); return; }
			ps->entries = tmp;
			ps->cap     = new_cap;
		}
		snprintf(ps->entries[ps->count].path,
				 sizeof ps->entries[ps->count].path, "%s", path);
		ps->entries[ps->count].mtime = st.st_mtime;
		ps->count++;

		// Recurse into subdirectories
		if (S_ISDIR(st.st_mode))
			poll_snapshot(ps, path, ignore_hidden);
	}
	closedir(d);
}

/* ------------------------------------------------------------------ */
/*  Watcher struct                                                      */
/* ------------------------------------------------------------------ */

struct Watcher {
	char          root[4096];       // Root directory to watch
	int           use_poll;         // 1 = poll backend, 0 = inotify (Linux)
	int           ignore_hidden;    // Skip dotfiles
	watcher_cb_t  cb;               // User callback on change
	void         *userdata;         // Opaque pointer passed to cb

	pthread_t     thread;           // Watcher thread
	atomic_int    running;          // 1 while thread should keep running

	// Pipe used to wake the thread on shutdown
	int           wake_rd;
	int           wake_wr;

#if defined(__linux__)
	InotifyState  ino;             // inotify state (Linux only)
#endif  // __linux__
};

/* ------------------------------------------------------------------ */
/*  inotify thread                                                      */
/* ------------------------------------------------------------------ */

#if defined(__linux__)
// inotify watcher thread: waits for events from inotify or wake pipe
static void *inotify_thread(void *arg)
{
	Watcher      *w   = arg;
	InotifyState *s   = &w->ino;
	char          buf[INOTIFY_BUF_LEN] __attribute__((aligned(8)));

	while (atomic_load_explicit(&w->running, memory_order_relaxed)) {
		// Use select() to wait on both inotify fd and wake pipe
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(s->ifd,    &rfds);
		FD_SET(w->wake_rd, &rfds);
		int maxfd = (s->ifd > w->wake_rd ? s->ifd : w->wake_rd) + 1;

		struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
		int ret = select(maxfd, &rfds, NULL, NULL, &tv);
		if (ret < 0) break;
		if (!atomic_load_explicit(&w->running, memory_order_relaxed)) break;

		if (FD_ISSET(w->wake_rd, &rfds)) break;   // Shutdown signal

		if (!FD_ISSET(s->ifd, &rfds)) continue;    // Timeout, recheck

		// Read all available inotify events
		ssize_t len = read(s->ifd, buf, sizeof buf);
		if (len <= 0) continue;

		char         changed_path[4096] = {0};
		int          fired              = 0;
		const char  *p                  = buf;

		while (p < buf + len) {
			const struct inotify_event *ev =
				(const struct inotify_event *)p;
			p += sizeof(struct inotify_event) + ev->len;

			if (ev->mask & IN_IGNORED) continue;

			const char *dir = inotify_wd_path(s, ev->wd);
			if (!dir) continue;

			if (ev->len > 0) {
				snprintf(changed_path, sizeof changed_path,
						 "%s/%s", dir, ev->name);
			} else {
				snprintf(changed_path, sizeof changed_path, "%s", dir);
			}

			// If a new directory was created, add it to the watch set
			if ((ev->mask & (IN_CREATE | IN_MOVED_TO)) &&
				ev->len > 0) {
				struct stat st;
				if (stat(changed_path, &st) == 0 && S_ISDIR(st.st_mode))
					inotify_watch_recursive(s, changed_path, w->ignore_hidden);
			}

			if (!fired) {
				fired = 1;
			}
		}

		if (fired && atomic_load_explicit(&w->running, memory_order_relaxed)) {
			LOG_DEBUG("inotify change detected: %s", changed_path);
			w->cb(changed_path[0] ? changed_path : NULL, w->userdata);
		}
	}

	return NULL;
}
#endif  // __linux__

/* ------------------------------------------------------------------ */
/*  Poll thread                                                         */
/* ------------------------------------------------------------------ */

// Sleep for ms milliseconds, aborting early if wake_fd becomes readable.
// Retries on EINTR so the full sleep interval is respected.
static void poll_sleep_ms(int ms, int wake_fd)
{
	struct timeval tv;
	tv.tv_sec  = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;

	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(wake_fd, &rfds);

	// Keep retrying on EINTR; abort only when wake_fd becomes readable
	while (tv.tv_sec > 0 || tv.tv_usec > 0) {
		FD_ZERO(&rfds);
		FD_SET(wake_fd, &rfds);
		int ret = select(wake_fd + 1, &rfds, NULL, NULL, &tv);
		if (ret > 0) break;             // wake_fd readable → shutdown signal
		if (ret == 0) break;            // timeout elapsed
		// ret < 0: EINTR — tv is updated by Linux, macOS leaves it unchanged;
		// on macOS we continue with the original timeout, but since this is a
		// sleep, being woken early is harmless — just re-check running flag.
		if (errno != EINTR) break;
	}
}

// Poll watcher thread: periodically snapshots the tree and diffs mtimes
static void *poll_thread(void *arg)
{
	Watcher   *w = arg;
	PollState  prev = {0}, curr = {0};
	PollMap    pmap;

	// Take the initial snapshot
	poll_snapshot(&prev, w->root, w->ignore_hidden);
	LOG_DEBUG("Poll watcher initial snapshot: %d entries", prev.count);

	// Build initial hash map
	pmap_clear(&pmap);
	for (int i = 0; i < prev.count; i++)
		pmap_insert(&pmap, prev.entries[i].path, prev.entries[i].mtime);

	while (atomic_load_explicit(&w->running, memory_order_relaxed)) {
		poll_sleep_ms(POLL_INTERVAL_MS, w->wake_rd);
		if (!atomic_load_explicit(&w->running, memory_order_relaxed)) break;

		// Take a new snapshot
		curr.count = 0;
		poll_snapshot(&curr, w->root, w->ignore_hidden);

		// O(N) comparison using hash map: check new entries against prev map
		int changed = (curr.count != prev.count);
		if (!changed) {
			for (int i = 0; i < curr.count && !changed; i++) {
				time_t prev_mt;
				if (!pmap_get(&pmap, curr.entries[i].path, &prev_mt)) {
					changed = 1; // new file not in previous snapshot
				} else if (curr.entries[i].mtime != prev_mt) {
					changed = 1; // mtime changed
				}
			}
		}

		if (changed && atomic_load_explicit(&w->running, memory_order_relaxed)) {
			LOG_DEBUG("Poll watcher detected file change (curr=%d prev=%d)",
			          curr.count, prev.count);
			w->cb(NULL, w->userdata);
		}

		// Swap prev and curr (reuse memory)
		PollEntry *tmp = prev.entries;
		prev.entries   = curr.entries;
		prev.count     = curr.count;
		prev.cap       = curr.cap;
		curr.entries   = tmp;
		curr.count     = 0;
		curr.cap       = prev.cap;
		if (curr.entries == NULL) curr.cap = 0;

		// Rebuild hash map for next comparison
		pmap_clear(&pmap);
		for (int i = 0; i < prev.count; i++)
			pmap_insert(&pmap, prev.entries[i].path, prev.entries[i].mtime);
	}

	free(prev.entries);
	free(curr.entries);
	return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

// Create a new file-system watcher.
//   root_dir     - directory to watch
//   use_poll     - 1 = force poll backend (macOS always uses this)
//   ignore_hidden - skip dotfiles
//   cb           - callback invoked on file change
//   userdata     - opaque pointer passed through to cb
// Returns a Watcher* on success, NULL on error.
Watcher *watcher_create(const char   *root_dir,
                         int           use_poll,
                         int           ignore_hidden,
                         watcher_cb_t  cb,
                         void         *userdata)
{
	Watcher *w = calloc(1, sizeof *w);
	if (!w) return NULL;

	snprintf(w->root, sizeof w->root, "%s", root_dir);
	w->ignore_hidden = ignore_hidden;
	w->cb            = cb;
	w->userdata      = userdata;
	atomic_store_explicit(&w->running, 0, memory_order_relaxed);

	// Create wake pipe for clean shutdown
	int pipefds[2];
	if (pipe(pipefds) != 0) { free(w); return NULL; }
	w->wake_rd = pipefds[0];
	w->wake_wr = pipefds[1];

#if defined(__linux__)
	if (!use_poll) {
		// Try inotify; fall back to poll on failure
		w->use_poll  = 0;
		w->ino.ifd   = inotify_init1(IN_NONBLOCK);
		if (w->ino.ifd < 0) {
			LOG_WARN("inotify_init1 failed, falling back to poll: %m");
			LOG_INFO("File watcher: using poll backend instead of inotify");
			w->use_poll = 1;
		} else {
			inotify_watch_recursive(&w->ino, root_dir, ignore_hidden);
		}
	} else {
		w->use_poll = 1;
	}
#elif defined(__APPLE__) && defined(__MACH__)
	// macOS / other: always use poll
	w->use_poll = 1;
	(void)use_poll;
#endif  // __linux__

	LOG_DEBUG("Watcher created for %s (poll=%d, ignore_hidden=%d)",
	          root_dir, w->use_poll, ignore_hidden);
	return w;
}

// Start the watcher thread. Returns 0 on success.
int watcher_start(Watcher *w)
{
	LOG_INFO("Starting file watcher for %s", w->root);
	atomic_store_explicit(&w->running, 1, memory_order_relaxed);

#if defined(__linux__)
	if (!w->use_poll)
		return pthread_create(&w->thread, NULL, inotify_thread, w);
#endif  // __linux__

	return pthread_create(&w->thread, NULL, poll_thread, w);
}

// Signal the watcher to stop and wait for the thread to finish.
void watcher_stop(Watcher *w)
{
	LOG_INFO("Stopping file watcher for %s", w->root);
	atomic_store_explicit(&w->running, 0, memory_order_relaxed);
	// Wake the thread from select()/poll_sleep_ms()
	char b = 1;
	write(w->wake_wr, &b, 1);
	pthread_join(w->thread, NULL);
	LOG_DEBUG("File watcher thread joined");
}

// Free all watcher resources. Call after watcher_stop().
void watcher_destroy(Watcher *w)
{
	if (!w) return;
	close(w->wake_rd);
	close(w->wake_wr);

#if defined(__linux__)
	if (!w->use_poll && w->ino.ifd >= 0) {
		close(w->ino.ifd);
		free(w->ino.watches);
	}
#endif  // __linux__

	free(w);
}
