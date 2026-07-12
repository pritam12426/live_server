/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * thread_buffer.h — Thread-local buffer pool
 */

#ifndef _THREAD_BUFFER_H_
#define _THREAD_BUFFER_H_


#include <stddef.h>

// Get a body buffer of at least the requested size
// Returns NULL on allocation failure
char *thread_buffer_get_body(size_t min_size);

// Release all thread-local buffers (for cleanup)
void thread_buffer_cleanup(void);


#endif  // _THREAD_BUFFER_H_
