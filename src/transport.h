/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * transport.h — Abstract I/O layer over plain sockets
 *
 * Transport* is an opaque handle.  The rest of the server never
 * touches raw fd directly.
 */

#ifndef _TRANSPORT_H_
#define _TRANSPORT_H_


#include <stdbool.h>
#include <stddef.h>
#include <sys/uio.h>
#include <sys/types.h>

typedef struct Transport Transport;

// Allocate a new transport for a connected socket fd.
Transport *transport_new(int fd);

// No-op for plain sockets.  Returns 0.
int transport_accept(Transport *t);

// Read / write (handle partial writes internally, return bytes transferred)
ssize_t transport_read(Transport *t, void *buf, size_t len);
ssize_t transport_write(Transport *t, const void *buf, size_t len);

// Scatter-gather write (writev) - writes multiple buffers in one syscall
ssize_t transport_writev(Transport *t, const struct iovec *iov, int iovcnt);

// Close the transport and free resources.
void transport_close(Transport *t);

// Close and free the transport, nullifying the caller's pointer.
void transport_destroy(Transport **t);

// Set a receive timeout (seconds) on the underlying socket.
int transport_set_timeout(Transport *t, int seconds);

// Accessors
int  transport_fd(const Transport *t);
bool transport_is_tls(const Transport *t);


#endif  // _TRANSPORT_H_
