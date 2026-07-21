/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * log.h — Thread-safe logger interface
 *
 * Logging macros:
 *   LOG_FATAL(...)  — unrecoverable errors (program will abort)
 *   LOG_ERROR(...)  — critical errors
 *   LOG_WARN(...)   — warnings (recoverable)
 *   LOG_INFO(...)   — informational messages
 *   LOG_DEBUG(...)  — debug details
 *   LOG_TRACE(...)  — fine-grained tracing
 */

#ifndef _LOG_H_
#define _LOG_H_


#include <stdbool.h> // bool
#include <stdio.h>   // FILE

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus


// Log severity levels (lower number = higher priority)
typedef enum {
	LOG_LEVEL_OFF   = 0,
	LOG_LEVEL_FATAL = 1,
	LOG_LEVEL_ERROR = 2,
	LOG_LEVEL_WARN  = 3,
	LOG_LEVEL_INFO  = 4,
	LOG_LEVEL_DEBUG = 5,
	LOG_LEVEL_TRACE = 6,
} Log_level_t;

// Feature flags for log_init (bitmask)
typedef enum {
	LOG_FLAG_NONE           = 0,
	LOG_FLAG_SHOW_TIMESTAMP = 1 << 0,
	LOG_FLAG_SHOW_SOURCE    = 1 << 1,
} Log_flags_t;

// Logger initialisation.
// May be called at any time from any thread; it is fully thread-safe.
// Calling it more than once is valid (e.g. to rotate the log file).
//
// file_path: path to a log file to write to. If NULL, output goes to stderr.
//            Colour is auto-disabled for file output.
// level:     initial log level filter.
// flags:     bitmask of Log_flags_t features to enable.
//
// Link with -lpthread when using this logger.
void log_init(const char *file_path, Log_level_t level, Log_flags_t flags);

// Logger configuration
void        log_set_level(Log_level_t level);
Log_level_t log_get_level(void);

// Returns the current log output stream (either stderr or the file opened
// by log_init). Returns stderr if log_init() has not been called yet.
FILE *log_get_file(void);

// Returns true if the logger is currently emitting ANSI color codes.
// Color is enabled automatically when the output stream is a TTY,
// and disabled when writing to a file.
bool log_use_color(void);

// Internal implementation — do not call directly.
void log_record(Log_level_t level,
                const char *file __attribute__((unused)),
                int         line __attribute__((unused)),
                const char *func __attribute__((unused)),
                int         new_line,
                const char *fmt,
                ...);


/* --------------------------------------------------
 * Public logging macros
 * -------------------------------------------------- */

// Log with custom newline behaviour (0 = no newline, 1 = with newline)
// Used internally; prefer LOG_FATAL / LOG_ERROR / LOG_WARN / etc.
#define LOG_CUSTOM(LOG_LEVEL, NEW_LINE, ...)                                   \
	log_record(LOG_LEVEL, __FILE__, __LINE__, __func__, NEW_LINE, __VA_ARGS__)

// Log an error and append strerror(errno) (via perror)
#define LOG_PERROR(...)                                                            \
	do {                                                                           \
		log_record(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, 0, __VA_ARGS__); \
		perror(" ");                                                               \
	} while (0)

#define LOG_FATAL(...) \
	log_record(LOG_LEVEL_FATAL, __FILE__, __LINE__, __func__, 1, __VA_ARGS__)

#define LOG_ERROR(...) \
	log_record(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, 1, __VA_ARGS__)

#define LOG_WARN(...) \
	log_record(LOG_LEVEL_WARN, __FILE__, __LINE__, __func__, 1, __VA_ARGS__)

#define LOG_INFO(...) \
	log_record(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, 1, __VA_ARGS__)

#define LOG_DEBUG(...) \
	log_record(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, 1, __VA_ARGS__)

#define LOG_TRACE(...) \
	log_record(LOG_LEVEL_TRACE, __FILE__, __LINE__, __func__, 1, __VA_ARGS__)


#ifdef __cplusplus
}
#endif  // __cplusplus


#endif  // _LOG_H_
