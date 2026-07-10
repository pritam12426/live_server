/*
 * ratelimit.h — Per-IP connection rate limiting
 *
 * Tracks concurrent connections per client IP and rejects new ones
 * when the configured limit is exceeded (HTTP 429).
 */

#ifndef _RATELIMIT_H_
#define _RATELIMIT_H_

typedef struct RateLimit RateLimit;

// Create a limiter that allows max_conns_per_ip simultaneous connections.
// Returns NULL if max_conns_per_ip <= 0 (no limit).
RateLimit *ratelimit_create(int max_conns_per_ip);

// Register a connection from ip.  Returns 0 if allowed, -1 if denied.
int ratelimit_accept(RateLimit *rl, const char *ip);

// Unregister a connection from ip (call when the connection closes).
void ratelimit_leave(RateLimit *rl, const char *ip);

// Free all resources.
void ratelimit_destroy(RateLimit *rl);

#endif  // _RATELIMIT_H_
