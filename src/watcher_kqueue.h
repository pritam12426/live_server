/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * watcher_kqueue.h — macOS/BSD kqueue backend (internal)
 */

#ifndef _WATCHER_KQUEUE_H_
#define _WATCHER_KQUEUE_H_


#include "watcher_internal.h"

#if defined(__APPLE__)

int watcher_kqueue_init(Watcher *w);
void watcher_kqueue_watch_recursive(Watcher *w, const char *path, int ignore_hidden);
void *watcher_kqueue_thread(void *arg);
void watcher_kqueue_cleanup(Watcher *w);

#endif  // __APPLE__


#endif  // _WATCHER_KQUEUE_H_
