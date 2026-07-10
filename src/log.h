/*
 * log.h — Thread-safe logger interface
 *
 * Logging macros:
 *   LOG_ERROR(...)  — critical errors
 *   LOG_WARN(...)   — warnings (recoverable)
 *   LOG_INFO(...)   — informational messages
 *   LOG_DEBUG(...)  — debug details (compile-time no-op when !DEBUG)
 *
 * Build-time flags (set in Makefile):
 *   LOG_SHOW_TIME_STAMP        — prepend [HH:MM:SS.ffffff]
 *   LOG_SHOW_SOURCE_LOCATION   — append [file:line:func]
 */

#ifndef _LOG_H_
#define _LOG_H_

#include <stdbool.h>
#include <stdio.h>

// Log severity levels (lower number = higher priority)
typedef enum {
	LOG_LEVEL_ERROR  = 0,
	LOG_LEVEL_WARN   = 1,
	LOG_LEVEL_INFO   = 2,
	LOG_LEVEL_DEBUG  = 3
} Log_level_t;

// Logger initialisation.
// May be called at any time from any thread; it is fully thread-safe.
// Calling it more than once is valid (e.g. to rotate the log file).
//
// file_path: path to a log file to write to. If NULL, output goes to stderr.
//            If file_path is not NULL, color output is disabled automatically
//            (since log files are not TTYs).
void log_init(const char *file_path);

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
void log_record(
	Log_level_t level,
	const char *file __attribute__((unused)),
	int         line __attribute__((unused)),
	const char *func __attribute__((unused)),
	int         new_line,
	const char *fmt,
	...
);

// Shutdown logger and flush all messages
void log_shutdown(void);

/* --------------------------------------------------
 * Public logging macros
 * -------------------------------------------------- */

#ifdef LOG_SHOW_SOURCE_LOCATION

// Log with custom newline behaviour (0 = no newline, 1 = with newline)
// Used internally; prefer LOG_ERROR / LOG_WARN / etc.
#define LOG_CUSTOM(LOG_LEVEL, NEW_LINE, ...) \
	log_record(LOG_LEVEL, __FILE__, __LINE__, __FUNCTION__, NEW_LINE, __VA_ARGS__)

// Log an error and append strerror(errno) (via perror)
#define LOG_PERROR(...) \
	do { \
		log_record(LOG_LEVEL_ERROR, __FILE__, __LINE__, __FUNCTION__, 0, __VA_ARGS__); \
		perror(" "); \
	} while (0)

#define LOG_ERROR(...) \
	log_record(LOG_LEVEL_ERROR, __FILE__, __LINE__, __FUNCTION__, 1, __VA_ARGS__)

#define LOG_WARN(...) \
	log_record(LOG_LEVEL_WARN, __FILE__, __LINE__, __FUNCTION__, 1, __VA_ARGS__)

#define LOG_INFO(...) \
	log_record(LOG_LEVEL_INFO, __FILE__, __LINE__, __FUNCTION__, 1, __VA_ARGS__)

#define LOG_DEBUG(...) \
	log_record(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __FUNCTION__, 1, __VA_ARGS__)

#else

#define LOG_CUSTOM(LOG_LEVEL, NEW_LINE, ...) \
	log_record(LOG_LEVEL, 0, 0, 0, NEW_LINE, __VA_ARGS__)

#define LOG_PERROR(...) \
	do { \
		log_record(LOG_LEVEL_ERROR, 0, 0, 0, 0, __VA_ARGS__); \
		perror(" "); \
	} while (0)

#define LOG_ERROR(...) \
	log_record(LOG_LEVEL_ERROR, 0, 0, 0, 1, __VA_ARGS__)

#define LOG_WARN(...) \
	log_record(LOG_LEVEL_WARN, 0, 0, 0, 1, __VA_ARGS__)

#define LOG_INFO(...) \
	log_record(LOG_LEVEL_INFO, 0, 0, 0, 1, __VA_ARGS__)

#define LOG_DEBUG(...) \
	log_record(LOG_LEVEL_DEBUG, 0, 0, 0, 1, __VA_ARGS__)

#endif  // LOG_SHOW_SOURCE_LOCATION

#endif  // _LOG_H_