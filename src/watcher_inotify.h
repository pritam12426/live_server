/*
 * watcher_inotify.h — Linux inotify backend (internal)
 */

#ifndef _WATCHER_INOTIFY_H_
#define _WATCHER_INOTIFY_H_

#include "watcher_internal.h"

#if defined(__linux__)

void watcher_inotify_watch_recursive(Watcher *w, const char *path);
void *watcher_inotify_thread(void *arg);
void watcher_inotify_cleanup(Watcher *w);
int watcher_inotify_init(Watcher *w);

#endif  // __linux__

#endif  // _WATCHER_INOTIFY_H_