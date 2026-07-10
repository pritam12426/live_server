/*
 * log.c — Thread-safe logging implementation
 *
 * Supports:
 *   - Four log levels: ERROR, WARN, INFO, DEBUG
 *   - Optional timestamps with microsecond precision (LOG_SHOW_TIME_STAMP)
 *   - Optional source-file location (LOG_SHOW_SOURCE_LOCATION)
 *   - ANSI colour output when writing to a TTY
 *   - File output via log_init(path)
 *   - Full thread safety via pthread_rwlock
 */

#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>  // isatty(), fileno()

#ifdef LOG_SHOW_TIME_STAMP
	#include <time.h>
#endif  // LOG_SHOW_TIME_STAMP


// ANSI colour codes
#define COLOR_RESET        "\x1b[0m"
#define COLOR_BOLD_RED     "\x1b[1;31m"
#define COLOR_BOLD_GREEN   "\x1b[1;32m"
#define COLOR_BOLD_YELLOW  "\x1b[1;33m"
#define COLOR_BOLD_BLUE    "\x1b[1;34m"
#define COLOR_BOLD_MAGENTA "\x1b[1;35m"
#define COLOR_BOLD_CYAN    "\x1b[1;36m"
#define COLOR_DIM          "\x1b[2m"


// ── Logger state ─────────────────────────────────────────────────────────────
//
// Protected by g_rwlock:
//   - readers (log_record, log_get_level, log_use_color) take a READ lock —
//     many threads can log simultaneously.
//   - writers (log_init, log_set_level) take a WRITE lock —
//     exclusive access, blocks until all readers finish.

static pthread_rwlock_t g_rwlock     = PTHREAD_RWLOCK_INITIALIZER;
static Log_level_t      g_log_level  = LOG_LEVEL_INFO;
static FILE            *g_log_stream = NULL;  // NULL = not yet initialised
static bool             g_use_color  = false;


// ── Internal helpers (called with read-lock already held) ─────────────────────

// Print the log-level label without colour
static void default_log_handler(FILE *out, Log_level_t level)
{
	switch (level) {
		case LOG_LEVEL_INFO:  fprintf(out, "[INFO ] "); break;
		case LOG_LEVEL_WARN:  fprintf(out, "[WARN ] "); break;
		case LOG_LEVEL_ERROR: fprintf(out, "[ERROR] "); break;
		case LOG_LEVEL_DEBUG: fprintf(out, "[DEBUG] "); break;
		default:              fprintf(out, "[UNKWN] "); break;
	}
}

// Print the log-level label with ANSI colour
static void color_log_handler(FILE *out, Log_level_t level)
{
	switch (level) {
		case LOG_LEVEL_ERROR:
			fprintf(out, "🚨 [" COLOR_BOLD_RED    "ERROR" COLOR_RESET "] ");
			break;
		case LOG_LEVEL_WARN:
			fprintf(out, "⚠️  [" COLOR_BOLD_YELLOW "WARN " COLOR_RESET "] ");
			break;
		case LOG_LEVEL_INFO:
			fprintf(out, "ℹ️  [" COLOR_BOLD_GREEN  "INFO " COLOR_RESET "] ");
			break;
		case LOG_LEVEL_DEBUG:
			fprintf(out, "🛠️  [" COLOR_BOLD_CYAN   "DEBUG" COLOR_RESET "] ");
			break;
		default:
			fprintf(out, "[UNKWN] ");
			break;
	}
}


#ifdef LOG_SHOW_TIME_STAMP

// Print a microsecond-precision timestamp at the start of each log line
static void log_time_stamp_handler(FILE *out, bool use_color)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);

	struct tm tm_now;
	localtime_r(&ts.tv_sec, &tm_now);

	char timestamp[20];
	strftime(timestamp, sizeof(timestamp), "%H:%M:%S", &tm_now);

	int us = (int) (ts.tv_nsec / 1000);  // convert ns → microseconds

	if (use_color)
		fprintf(out, COLOR_DIM);
	fprintf(out, "[%s.%06d] ", timestamp, us);
	if (use_color)
		fprintf(out, COLOR_RESET);
}

#endif  // LOG_SHOW_TIME_STAMP


// ── Public API ────────────────────────────────────────────────────────────────

// Initialise the logger.
//   file_path: path to log file (NULL = stderr).
//              Colour is auto-disabled for file output.
// Thread-safe; can be called multiple times (e.g. for log rotation).
void log_init(const char *file_path)
{
	// Resolve the new stream and color flag BEFORE taking the lock,
	// so we hold the write-lock for the shortest possible time.
	FILE *new_stream;
	bool  new_color;

	if (file_path == NULL) {
		new_color  = isatty(fileno(stderr)) ? true : false;
		new_stream = stderr;
	} else {
		new_stream = fopen(file_path, "a");
		if (new_stream == NULL) {
			// Fall back to stderr and warn — no lock needed yet
			fprintf(stderr,
			        "[LOG] warning: could not open log file '%s', "
			        "falling back to stderr\n",
			        file_path);
			new_stream = stderr;
			new_color  = isatty(fileno(stderr)) ? true : false;
		} else {
			// Log files are never TTYs — no colour codes in files
			new_color = false;
		}
	}

	pthread_rwlock_wrlock(&g_rwlock);
	{
		// Close any previously opened log file (but never close stderr)
		if (g_log_stream != NULL && g_log_stream != stderr)
			fclose(g_log_stream);

		g_log_stream = new_stream;
		g_use_color  = new_color;
	}
	pthread_rwlock_unlock(&g_rwlock);
}


// Set the minimum log level; messages below this are suppressed
void log_set_level(Log_level_t level)
{
	pthread_rwlock_wrlock(&g_rwlock);
	g_log_level = level;
	pthread_rwlock_unlock(&g_rwlock);
}


// Get the current minimum log level
Log_level_t log_get_level(void)
{
	pthread_rwlock_rdlock(&g_rwlock);
	Log_level_t level = g_log_level;
	pthread_rwlock_unlock(&g_rwlock);
	return level;
}


// Check whether ANSI colour is enabled
bool log_use_color(void)
{
	pthread_rwlock_rdlock(&g_rwlock);
	bool color = g_use_color;
	pthread_rwlock_unlock(&g_rwlock);
	return color;
}


// Get the current log output stream (stderr if not initialised)
FILE *log_get_file(void)
{
	pthread_rwlock_rdlock(&g_rwlock);
	FILE *stream = g_log_stream ? g_log_stream : stderr;
	pthread_rwlock_unlock(&g_rwlock);
	return stream;
}


// Core logging function: formats and writes a log message.
// Called by the LOG_* macros.  Thread-safe via reader-writer lock.
void log_record(Log_level_t level,
                const char *file __attribute__((unused)),
                int         line __attribute__((unused)),
                const char *func __attribute__((unused)),
                int         new_line,
                const char *fmt,
                ...)
{
	if (fmt == NULL) return;

	if (g_log_stream == NULL) {
		fprintf(stderr, COLOR_BOLD_RED "[LOG] error: call log_init() before logging" COLOR_RESET);
		if (new_line) fputc('\n', stderr);
		return;
	}

	// Take a write-lock so only one thread writes at a time
	// (prevents interleaved log lines from concurrent requests)
	pthread_rwlock_wrlock(&g_rwlock);
	{
		// Suppress messages below the configured level
		if (level > g_log_level) {
			pthread_rwlock_unlock(&g_rwlock);
			return;
		}

#ifdef LOG_SHOW_TIME_STAMP
		log_time_stamp_handler(g_log_stream, g_use_color);
#endif  // LOG_SHOW_TIME_STAMP

		if (g_use_color)
			color_log_handler(g_log_stream, level);
		else
			default_log_handler(g_log_stream, level);

		if (file) {
			fprintf(g_log_stream,
			        "%s[%s:%d:%s]%s ",
			        g_use_color ? COLOR_DIM : "",
			        file,
			        line,
			        func,
			        g_use_color ? COLOR_RESET : "");
		}

		va_list args;
		va_start(args, fmt);
		vfprintf(g_log_stream, fmt, args);
		va_end(args);

		if (new_line) fputc('\n', g_log_stream);

		fflush(g_log_stream);
	}

	pthread_rwlock_unlock(&g_rwlock);
}
