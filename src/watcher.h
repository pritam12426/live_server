/*
 * watcher.h — File-system watcher (inotify, kqueue, or poll)
 */

#ifndef _WATCHER_H_
#define _WATCHER_H_

#include "watcher_internal.h"

/*
 * Create a watcher for `root_dir`.
 *   use_poll      - If non-zero, use the poll fallback instead of inotify/kqueue
 *   ignore_hidden - Skip hidden files/dirs (dot-prefixed)
 *   cb            - Called on change
 *   userdata      - Passed through to cb
 *
 * Returns NULL on error.
 */
Watcher *watcher_create(const char   *root_dir,
                          int          use_poll,
                          int          ignore_hidden,
                          watcher_cb_t cb,
                          void         *userdata);

/* Start the watcher thread.  Returns 0 on success. */
int watcher_start(Watcher *w);

/* Signal the watcher to stop and wait for the thread to exit. */
void watcher_stop(Watcher *w);

/* Free all resources.  Call after watcher_stop(). */
void watcher_destroy(Watcher *w);

#endif  // _WATCHER_H_