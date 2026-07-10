#ifndef _SERVER_H_
#define _SERVER_H_

#include <stdbool.h>

#include "types.h"

typedef struct {
	const char *host;
	int         port;

	const char *root_dir;
	const char *user;
	const char *pass;

	LivereloadMode livereload_mode;
	bool           print_request;
	bool           ignore_hidden;
	bool           poll;
	const char    *browser;

	int thread_pool_size;
	int keep_alive_timeout;
	int max_conns_per_ip;
} ServerConfig;

int server_run(const ServerConfig *cfg);

#endif  // _SERVER_H_
