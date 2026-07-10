/*
 * ratelimit.c — Per-IP connection rate limiting
 *
 * Uses an open-addressing hash table (1024 slots, djb2 hash) to store
 * a reference count per client IP.  Ratelimit_accept() increments the
 * count, ratelimit_leave() decrements it.  When the count exceeds the
 * configured maximum the connection is denied.
 *
 * Thread safety is provided by a single mutex covering the table.
 */

#include "ratelimit.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

#define RL_TABLE_SIZE 1024

typedef struct {
	char  ip[64];
	int   count;
} RLEntry;

struct RateLimit {
	int             max_per_ip;
	RLEntry         table[RL_TABLE_SIZE];
	pthread_mutex_t lock;
};

// djb2 hash — simple, fast, good enough for IP strings
static unsigned long hash_ip(const char *ip)
{
	unsigned long h = 5381;
	int c;
	while ((c = *ip++))
		h = ((h << 5) + h) + (unsigned long)c;
	return h;
}

RateLimit *ratelimit_create(int max_conns_per_ip)
{
	if (max_conns_per_ip <= 0) return NULL;

	RateLimit *rl = calloc(1, sizeof(*rl));
	if (!rl) return NULL;

	rl->max_per_ip = max_conns_per_ip;
	pthread_mutex_init(&rl->lock, NULL);

	LOG_DEBUG("Rate limit created: max %d conns/IP", max_conns_per_ip);
	return rl;
}

int ratelimit_accept(RateLimit *rl, const char *ip)
{
	if (!rl) return 0;

	pthread_mutex_lock(&rl->lock);

	unsigned long idx = hash_ip(ip) % RL_TABLE_SIZE;

	// Linear probe: find either an empty slot or this IP's slot
	for (int i = 0; i < RL_TABLE_SIZE; i++) {
		unsigned long slot = (idx + (unsigned long)i) % RL_TABLE_SIZE;
		if (rl->table[slot].count == 0) {
			snprintf(rl->table[slot].ip, sizeof(rl->table[slot].ip), "%s", ip);
			rl->table[slot].count = 1;
			pthread_mutex_unlock(&rl->lock);
			return 0;
		}
		if (strcmp(rl->table[slot].ip, ip) == 0) {
			if (rl->table[slot].count >= rl->max_per_ip) {
				LOG_WARN("Rate limit exceeded for %s (%d)", ip, rl->table[slot].count);
				pthread_mutex_unlock(&rl->lock);
				return -1;
			}
			rl->table[slot].count++;
			LOG_DEBUG("Rate limit accept: %s (count=%d/%d)",
			          ip, rl->table[slot].count, rl->max_per_ip);
			pthread_mutex_unlock(&rl->lock);
			return 0;
		}
	}

	LOG_WARN("Rate limit table full, allowing %s", ip);
	pthread_mutex_unlock(&rl->lock);
	return 0;
}

void ratelimit_leave(RateLimit *rl, const char *ip)
{
	if (!rl) return;

	pthread_mutex_lock(&rl->lock);

	unsigned long idx = hash_ip(ip) % RL_TABLE_SIZE;

	for (int i = 0; i < RL_TABLE_SIZE; i++) {
		unsigned long slot = (idx + (unsigned long)i) % RL_TABLE_SIZE;
		if (rl->table[slot].count > 0 && strcmp(rl->table[slot].ip, ip) == 0) {
			rl->table[slot].count--;
			LOG_DEBUG("Rate limit leave: %s (count=%d)", ip, rl->table[slot].count);
			if (rl->table[slot].count == 0)
				rl->table[slot].ip[0] = '\0';
			break;
		}
	}

	pthread_mutex_unlock(&rl->lock);
}

void ratelimit_destroy(RateLimit *rl)
{
	if (!rl) return;
	pthread_mutex_destroy(&rl->lock);
	free(rl);
	LOG_DEBUG("Rate limit destroyed");
}
