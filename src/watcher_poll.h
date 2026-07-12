/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * watcher_poll.h — Polling fallback backend (internal)
 */

#ifndef _WATCHER_POLL_H_
#define _WATCHER_POLL_H_


#include "watcher_internal.h"

struct PollMap *pmap_create(void);
void pmap_destroy(struct PollMap *m);
void pmap_clear(struct PollMap *m);
void pmap_insert(struct PollMap *m, const char *path, time_t mt);
int pmap_get(struct PollMap *m, const char *path, time_t *out_mt);

void poll_snapshot(struct PollState *ps, const char *root, int ignore_hidden);
void poll_sleep_ms(int ms, int wake_fd);
void *poll_thread(void *arg);


#endif  // _WATCHER_POLL_H_
