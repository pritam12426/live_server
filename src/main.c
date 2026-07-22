/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

#include <argp.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "project_config.h"
#include "server.h"
#include "types.h"

/* ── argp global strings ──────────────────────────────────────────────────── */
// These are used by --version and --help automatically
// project_config.h defines VERSION, HOMEPAGE_URL, and AUTH_MESSAGE
const char *argp_program_version     = MAIN_BINARY " " PROJECT_VERSION;
const char *argp_program_bug_address = PROJECT_HOMEPAGE_URL "/issues" "\n" AUTH_MESSAGE;
static char doc[]                    = MAIN_BINARY " - " PROJECT_DESCRIPTION;

/* ── CLI option table ─────────────────────────────────────────────────────── */
static struct argp_option options[] = {
	{ 0, 0, 0, 0, "Logging:", 1 },
	{ "log-level",      'L', "LEVEL",  0,  "Set log level: [off|fatal|error|warn|info|debug|trace] (default: info)", 1 },
	{ "log-file",       'F', "FILE",   0,  "Set logging file"                                                      , 1 },
	{ "print-request",  'R', 0,        0,  "Log each client request and its headers",                 1 },

	{ 0, 0, 0, 0, "Live Reload:", 2 },
	{ "live-reload",       'U',  0,  0,  "Inject SSE script and reload page after file changes",      2 },
	{ "live-hard-reload",  'W',  0,  0,  "Like --live-reload but force a full cache-busting reload",  2 },

	{ 0, 0, 0, 0, "Connection:", 3 },
	{ "port",       'P', "PORT",  0,  "TCP port to listen on (default: 8080)",                       3 },
	{ "host",       'H', "HOST",  0,  "Listener host / IP (default: localhost)",                     3 },
	{ "threads",    'T', "NUM",   0,  "Thread pool size (default: 2)",                               3 },
	{ "keep-alive", 'K', "SECS",  0,  "Keep-alive timeout in seconds (default: 3, 0 = disable)",     3 },
	{ "max-conns",  'M', "NUM",   0,  "Max concurrent connections per IP (default: 0 = unlimited)",  3 },

	{ 0, 0, 0, 0, "Authentication:", 4 },
	{ "user", 'u', "USER", 0,  "Enable Basic-Auth with this username (default when omitted: admin)",  4 },
	{ "pass", 'p', "PASS", 0,  "Enable Basic-Auth with this password (default when omitted: admin)",  4 },

	{ 0, 0, 0, 0, "Serving:", 5 },
	{ "dir",     'I', "DIR",     0,  "Directory to serve (default: .)",                                    5 },
	{ "ignore",  'i', 0,         0,  "Hide hidden files and .gitignored entries",                          5 },
	{ "browser", 'B', "BROWSER", 0,  "Open page in BROWSER on startup (e.g. firefox)",                     5 },
	{ "poll",    'O', 0,         0,  "Use poll-based watcher instead of inotify (useful over NFS/sshfs)",  5 },

	{ 0 }
};

/* ── Arguments struct (mirrors ServerConfig) ──────────────────────────────── */
// Stored as globals so parse_opt() can fill them; later copied into ServerConfig
typedef struct {
	// Logging
	Log_level_t    log_level;      // -L: verbosity threshold
	const char    *log_file;       // -F: Logging file
	bool           print_request;  // -R: log every request

	// Live Reload
	LivereloadMode livereload_mode;  // -U / -W: live-reload mode

	// Connection
	int            port;        // -P: listen port (default: 8080)
	const char    *host;        // -H: bind address (default: localhost)
	int            threads;     // -t: thread pool size (default: 2)
	int            keep_alive;  // -k: keep-alive timeout (default: 3)
	int            max_conns;   // -M: max conns per IP (default: 0 = unlimited)

	// Authentication
	const char    *pass;  // -p: Basic-Auth password (NULL = disabled)
	const char    *user;  // -u: Basic-Auth username (NULL = disabled)

	// Serving
	const char    *dir;      // -I: document root (default: ".")
	bool           ignore;   // -i: skip hidden files
	const char    *browser;  // -B: browser to open on start
	bool           poll;     // -o: use poll watcher (macOS always uses poll)
} Arguments;

static Arguments G_Args = {
	// Logging
	.log_level       = LOG_LEVEL_INFO,
	.print_request   = false,

	// Live Reload
	.livereload_mode = LIVERELOAD_OFF,

	// Connection
	.port            = 8080,
	.host            = "localhost",
	.threads         = 2,
	.keep_alive      = 3,
	.max_conns       = 0,

	// Authentication
	.user            = NULL,  // NULL = auth disabled by default
	.pass            = NULL,

	// Serving
	.dir             = ".",
	.ignore          = false,
	.browser         = NULL,
	.poll            = false,
};

/* ── Option parser ────────────────────────────────────────────────────────── */
// Called by argp for each CLI flag; key is the short-option character
static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	switch (key) {
	case 'P': {
		// Parse port number, validate range 1–65535
		char *end;
		long  port = strtol(arg, &end, 10);
		if (*arg == '\0' || *end != '\0')
			argp_error(state, "Invalid port: '%s'.", arg);
		if (port < 1 || port > 65535)
			argp_error(state, "Port out of range: %ld.", port);
		G_Args.port = (int)port;
		break;
	}
	case 'L': {
		if      (strcmp(arg, "off")   == 0) G_Args.log_level = LOG_LEVEL_OFF;
		else if (strcmp(arg, "fatal") == 0) G_Args.log_level = LOG_LEVEL_FATAL;
		else if (strcmp(arg, "error") == 0) G_Args.log_level = LOG_LEVEL_ERROR;
		else if (strcmp(arg, "warn")  == 0) G_Args.log_level = LOG_LEVEL_WARN;
		else if (strcmp(arg, "info")  == 0) G_Args.log_level = LOG_LEVEL_INFO;
		else if (strcmp(arg, "debug") == 0) G_Args.log_level = LOG_LEVEL_DEBUG;
		else if (strcmp(arg, "trace") == 0) G_Args.log_level = LOG_LEVEL_TRACE;
		else     argp_error(state, "Invalid log level: '%s'. Use: off, fatal, error, warn, info, debug, trace.", arg);
		break;
	}
	case 'F': G_Args.log_file      = arg;  break;
	case 'H': G_Args.host          = arg;  break;
	case 'R': G_Args.print_request = true; break;
	case 'T': {
		char *end;
		long n = strtol(arg, &end, 10);
		if (*arg == '\0' || *end != '\0' || n < 1 || n > 256)
			argp_error(state, "Invalid thread count: '%s'. Must be 1–256.", arg);
		G_Args.threads = (int)n;
		break;
	}
	case 'K': {
		char *end;
		long n = strtol(arg, &end, 10);
		if (*arg == '\0' || *end != '\0' || n < 0 || n > 3600)
			argp_error(state, "Invalid keep-alive timeout: '%s'. Must be 0–3600.", arg);
		G_Args.keep_alive = (int)n;
		break;
	}
	case 'M': {
		char *end;
		long n = strtol(arg, &end, 10);
		if (*arg == '\0' || *end != '\0' || n < 0 || n > 1000)
			argp_error(state, "Invalid max-conns: '%s'. Must be 0–1000.", arg);
		G_Args.max_conns = (int)n;
		break;
	}
	case 'i': G_Args.ignore        = true; break;
	case 'O': G_Args.poll          = true; break;
	case 'p': {
		// --pass without --user defaults user to "admin"
		if (arg == NULL) {
			G_Args.user = "admin";
			G_Args.pass = "admin";
		} else {
			if (G_Args.user == NULL)
				G_Args.user = "admin";
			G_Args.pass = arg;
		}
		break;
	}
	case 'u': G_Args.user = arg;  break;
	case 'U': {
		// --live-reload and --live-hard-reload are mutually exclusive
		if (G_Args.livereload_mode == LIVERELOAD_HARD_RELOAD)
			argp_error(state, "Cannot combine --live-reload and --live-hard-reload.");
		G_Args.livereload_mode = LIVERELOAD_SOFT_RELOAD;
		break;
	}
	case 'W': {
		if (G_Args.livereload_mode == LIVERELOAD_SOFT_RELOAD)
			argp_error(state, "Cannot combine --live-reload and --live-hard-reload.");
		G_Args.livereload_mode = LIVERELOAD_HARD_RELOAD;
		break;
	}
	case 'B': G_Args.browser = arg; break;
	case 'I': G_Args.dir     = arg; break;
	case ARGP_KEY_END: {
		// Validate argument combinations after all flags are parsed
		if (G_Args.user != NULL && G_Args.pass == NULL)
			argp_error(state, "A password must be provided when a username is specified.");

		// Verify --dir exists (skip check for "." since it always exists)
		if (strcmp(G_Args.dir, ".") != 0) {
			if (access(G_Args.dir, F_OK) != 0)
				argp_error(state, "Invalid path '%s': no such directory.", G_Args.dir);
		}
		break;
	}
	default: return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = {
	.options = options,
	.parser  = parse_opt,
	.doc     = doc,
};

/* ── main ─────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
	// Parse CLI args; argp calls parse_opt() for each flag
	argp_parse(&argp, argc, argv, 0, 0, 0);
	log_init(G_Args.log_file, G_Args.log_level);

	// Dump parsed CLI args when in debug mode — useful for troubleshooting
	if (LOG_LEVEL_IS_ENABLED(LOG_LEVEL_DEBUG)) {
		LOG_CUSTOM(LOG_LEVEL_DEBUG, false, "Command-line args: [");
		for (int i = 0; i < argc; i++) {
			fprintf(stderr, "\"%s\"", argv[i]);
			if (i != argc - 1) fputs(", ", stderr);
		}
		fputs("]\n", stderr);
	}

	// Build server config from parsed arguments
	ServerConfig cfg = {
		.host               = G_Args.host,
		.port               = G_Args.port,
		.root_dir           = G_Args.dir,
		.user               = G_Args.user,
		.pass               = G_Args.pass,
		.livereload_mode    = G_Args.livereload_mode,
		.print_request      = G_Args.print_request,
		.ignore_hidden      = G_Args.ignore,
		.poll               = G_Args.poll,
		.browser            = G_Args.browser,
		.thread_pool_size   = G_Args.threads,
		.keep_alive_timeout = G_Args.keep_alive,
		.max_conns_per_ip   = G_Args.max_conns,
	};

	// Enter the main server loop — this blocks until shutdown
	return server_run(&cfg);
}
