/*
 * log.c — Thread-safe logging implementation
 *
 * Features:
 *   - Six log levels: FATAL, ERROR, WARN, INFO, DEBUG, TRACE
 *   - Compile-time timestamps (-DLOG_SHOW_TIME_STAMP)
 *   - Compile-time source location (-DLOG_SHOW_SOURCE_LOCATION)
 *   - ANSI colour output (auto-disabled for file output)
 *   - Thread safety via pthread_mutex
 */

#include "log.h"

#include <pthread.h>  // pthread_mutex_t, pthread_mutex_lock(), pthread_mutex_unlock()
#include <stdarg.h>   // va_list, va_start(), va_end()
#include <stdio.h>    // fprintf(), fopen(), fclose(), fflush(), vfprintf(), stderr
#include <unistd.h>   // isatty(), fileno()

#ifdef LOG_SHOW_TIME_STAMP
#include <time.h>     // clock_gettime(), localtime_r(), strftime()
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
// Protected by g_log_mutex.
// Since log_record() always takes a write-lock (to prevent interleaved output),
// a plain mutex is simpler and slightly faster than a rwlock.

static pthread_mutex_t g_log_mutex  = PTHREAD_MUTEX_INITIALIZER;
static Log_level_t     g_log_level  = LOG_LEVEL_INFO;
static FILE           *g_log_stream = NULL;  // NULL = not yet initialised
static bool            g_use_color  = false;


// ── Internal helpers (called with read-lock already held) ─────────────────────

// Print the log-level label without colour
static void default_log_handler(FILE *out, Log_level_t level)
{
	switch (level) {
		case LOG_LEVEL_FATAL: fprintf(out, "[FATAL] "); break;
		case LOG_LEVEL_ERROR: fprintf(out, "[ERROR] "); break;
		case LOG_LEVEL_WARN:  fprintf(out, "[WARN ] "); break;
		case LOG_LEVEL_INFO:  fprintf(out, "[INFO ] "); break;
		case LOG_LEVEL_DEBUG: fprintf(out, "[DEBUG] "); break;
		case LOG_LEVEL_TRACE: fprintf(out, "[TRACE] "); break;
		default:              fprintf(out, "[UNKWN] "); break;
	}
}

// Print the log-level label with ANSI colour
static void color_log_handler(FILE *out, Log_level_t level)
{
	switch (level) {
		case LOG_LEVEL_FATAL:
			fprintf(out, "💀 [" COLOR_BOLD_BLUE "FATAL" COLOR_RESET "] ");
			break;
		case LOG_LEVEL_ERROR:
			fprintf(out, "🚨 [" COLOR_BOLD_RED "ERROR" COLOR_RESET "] ");
			break;
		case LOG_LEVEL_WARN:
			fprintf(out, "⚠️  [" COLOR_BOLD_YELLOW "WARN " COLOR_RESET "] ");
			break;
		case LOG_LEVEL_INFO:
			fprintf(out, "ℹ️  [" COLOR_BOLD_GREEN "INFO " COLOR_RESET "] ");
			break;
		case LOG_LEVEL_DEBUG:
			fprintf(out, "🛠️  [" COLOR_BOLD_CYAN "DEBUG" COLOR_RESET "] ");
			break;
		case LOG_LEVEL_TRACE:
			fprintf(out, "🔬 [" COLOR_BOLD_MAGENTA "TRACE" COLOR_RESET "] ");
			break;
		default:
			fprintf(out, "[" COLOR_BOLD_BLUE "UNKWN" COLOR_RESET "] ");
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

// Initialise the logger. Thread-safe; may be called multiple times.
//   file_path: log file path, or NULL for stderr (colour auto-disabled for files).
//   level:     minimum severity to emit (e.g. LOG_LEVEL_INFO).
void log_init(const char *file_path, Log_level_t level)
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

	pthread_mutex_lock(&g_log_mutex);
	{
		// Close any previously opened log file (but never close stderr)
		if (g_log_stream != NULL && g_log_stream != stderr)
			fclose(g_log_stream);

		g_log_stream = new_stream;
		g_use_color  = new_color;
		g_log_level  = level;
	}
	pthread_mutex_unlock(&g_log_mutex);
}


// Set the minimum log level; messages below this are suppressed
void log_set_level(Log_level_t level)
{
	pthread_mutex_lock(&g_log_mutex);
	g_log_level = level;
	pthread_mutex_unlock(&g_log_mutex);
}


// Get the current minimum log level
Log_level_t log_get_level(void)
{
	pthread_mutex_lock(&g_log_mutex);
	Log_level_t level = g_log_level;
	pthread_mutex_unlock(&g_log_mutex);
	return level;
}


// Check whether ANSI colour is enabled
bool log_use_color(void)
{
	pthread_mutex_lock(&g_log_mutex);
	bool color = g_use_color;
	pthread_mutex_unlock(&g_log_mutex);
	return color;
}


// Get the current log output stream (stderr if not initialised)
FILE *log_get_file(void)
{
	pthread_mutex_lock(&g_log_mutex);
	FILE *stream = g_log_stream ? g_log_stream : stderr;
	pthread_mutex_unlock(&g_log_mutex);
	return stream;
}


// Core logging function: formats and writes a log message.
// Called by the LOG_* macros.  Thread-safe via mutex.
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
		fprintf(stderr, COLOR_BOLD_RED "[LOG] error: log_init() not called — dropping message" COLOR_RESET);
		if (new_line) fputc('\n', stderr);
		return;
	}

	// Take a mutex so only one thread writes at a time
	// (prevents interleaved log lines from concurrent requests)
	pthread_mutex_lock(&g_log_mutex);
	{
		// Suppress messages below the configured level
		if (level > g_log_level) {
			pthread_mutex_unlock(&g_log_mutex);
			return;
		}

#ifdef LOG_SHOW_TIME_STAMP
		log_time_stamp_handler(g_log_stream, g_use_color);
#endif  // LOG_SHOW_TIME_STAMP

		if (g_use_color)
			color_log_handler(g_log_stream, level);
		else
			default_log_handler(g_log_stream, level);

#ifdef LOG_SHOW_SOURCE_LOCATION
		fprintf(g_log_stream,
		        "%s[%s:%d:%s]%s ",
		        g_use_color ? COLOR_DIM : "",
		        file,
		        line,
		        func,
		        g_use_color ? COLOR_RESET : "");
#endif  // LOG_SHOW_SOURCE_LOCATION

		va_list args;
		va_start(args, fmt);
		vfprintf(g_log_stream, fmt, args);
		va_end(args);

		if (new_line) fputc('\n', g_log_stream);

		fflush(g_log_stream);
	}

	pthread_mutex_unlock(&g_log_mutex);
}
