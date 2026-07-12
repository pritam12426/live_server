/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * transport.c — Abstract I/O layer over plain sockets
 *
 * Transport wraps a raw fd so the rest of the server can read/write
 * without caring about the underlying socket details.
 */

#include "transport.h"

#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "log.h"

// Opaque Transport struct
struct Transport {
	int  fd;        // Underlying socket fd
};

// Allocate a new transport for a connected socket.
Transport *transport_new(int fd)
{
	Transport *t = calloc(1, sizeof(*t));
	if (!t) {
		LOG_ERROR("calloc failed for transport_new (fd=%d)", fd);
		return NULL;
	}
	t->fd = fd;

	// Disable Nagle's algorithm for lower latency on small responses
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

	LOG_DEBUG("Transport created (fd=%d)", fd);
	return t;
}

// No-op for plain sockets.  Returns 0.
int transport_accept(Transport *t)
{
	if (!t) return -1;
	return 0;
}

// Read up to len bytes from the transport.
// Returns the number of bytes read, 0 on EOF, or -1 on error.
ssize_t transport_read(Transport *t, void *buf, size_t len)
{
	if (!t) return -1;
	return read(t->fd, buf, len);
}

// Write len bytes to the transport (handles partial writes internally).
// Returns the number of bytes written, or -1 on error.
ssize_t transport_write(Transport *t, const void *buf, size_t len)
{
	if (!t) return -1;

	// SIGPIPE is ignored in server.c
	size_t written = 0;
	while (written < len) {
		ssize_t n = write(t->fd, (const char *)buf + written, len - written);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) continue;
			break;
		}
		written += (size_t)n;
	}
	return (ssize_t)written;
}

// Scatter-gather write using writev() - writes multiple buffers in one syscall
// Returns total bytes written, or -1 on error
ssize_t transport_writev(Transport *t, const struct iovec *iov, int iovcnt)
{
	if (!t) return -1;

	// SIGPIPE is ignored in server.c
	return writev(t->fd, iov, iovcnt);
}

// Close the transport and free resources.
void transport_close(Transport *t)
{
	if (!t) return;

	if (t->fd >= 0) {
		LOG_DEBUG("Closing socket (fd=%d)", t->fd);
		close(t->fd);
		t->fd = -1;
	}
}

// Close and free the transport, nullifying the caller's pointer.
void transport_destroy(Transport **t)
{
	if (!t || !*t)
		return;
	transport_close(*t);
	free(*t);
	*t = NULL;
}

// Set a receive timeout on the underlying socket.
int transport_set_timeout(Transport *t, int seconds)
{
	if (!t || t->fd < 0) return -1;
	struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
	int rc = setsockopt(t->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	LOG_DEBUG("Set SO_RCVTIMEO=%ds on fd=%d → %d", seconds, t->fd, rc);
	return rc;
}

// Return the underlying socket fd (for logging / debugging).
int transport_fd(const Transport *t)
{
	return t ? t->fd : -1;
}

// Always returns false (TLS was removed from this build).
bool transport_is_tls(const Transport *t)
{
	(void)t;
	return false;
}
