/*
 * watcher_internal.h — Internal watcher definitions (shared by backends)
 */

#ifndef _WATCHER_INTERNAL_H_
#define _WATCHER_INTERNAL_H_

#include <stdatomic.h>
#include <time.h>

/* Callback type (forward declare) */
typedef struct Watcher Watcher;
typedef void (*watcher_cb_t)(const char *path, void *userdata);

#if defined(__linux__)
#include <sys/inotify.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#endif

struct WdEntry {
    int  wd;
    char path[4096];
};

struct KqEntry {
    int  fd;
    char path[4096];
};

struct PollEntry {
    char   path[4096];
    time_t mtime;
};

struct PollState {
    struct PollEntry *entries;
    int count;
    int cap;
};

struct PollMap {
    char   keys[4096][64];
    time_t vals[4096];
    int    used[4096];
};

struct Watcher {
    char          root[4096];
    int           use_poll;
    int           ignore_hidden;
    watcher_cb_t  cb;
    void         *userdata;

    pthread_t     thread;
    atomic_int    running;

    int           wake_rd;
    int           wake_wr;

#if defined(__linux__)
    int           ifd;
    struct WdEntry *watches;
    int           watch_count;
    int           watch_cap;
#elif defined(__APPLE__)
    int           kq;
    struct KqEntry *watches;
    int           watch_count;
    int           watch_cap;
#endif
};

#endif  // _WATCHER_INTERNAL_H_