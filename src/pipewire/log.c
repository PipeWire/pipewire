/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <pipewire/log.h>

#define DEFAULT_LOG_LEVEL SPA_LOG_LEVEL_ERROR

SPA_EXPORT
enum spa_log_level pw_log_level = DEFAULT_LOG_LEVEL;

static struct spa_log *global_log = NULL;

/** Set the global log interface
 * \param log the global log to set
 * \memberof pw_log
 */
SPA_EXPORT
void pw_log_set(struct spa_log *log)
{
	global_log = log;
	if (global_log)
		global_log->level = pw_log_level;
}

/** Get the global log interface
 * \return the global log
 * \memberof pw_log
 */
SPA_EXPORT
struct spa_log *pw_log_get(void)
{
	return global_log;
}

/** Set the global log level
 * \param level the new log level
 * \memberof pw_log
 */
SPA_EXPORT
void pw_log_set_level(enum spa_log_level level)
{
	pw_log_level = level;
	if (global_log)
		global_log->level = level;
}

/** Log a message
 * \param level the log level
 * \param file the file this message originated from
 * \param line the line number
 * \param func the function
 * \param fmt the printf style format
 * \param ... printf style arguments to log
 *
 * \memberof pw_log
 */
SPA_EXPORT
void
pw_log_log(enum spa_log_level level,
	   const char *file,
	   int line,
	   const char *func,
	   const char *fmt, ...)
{
	if (SPA_UNLIKELY(pw_log_level_enabled(level) && global_log)) {
		va_list args;
		va_start(args, fmt);
		spa_interface_call(&global_log->iface,
			struct spa_log_methods, logv, 0, level, file, line,
			func, fmt, args);
		va_end(args);
	}
}

/** Log a message with va_list
 * \param level the log level
 * \param file the file this message originated from
 * \param line the line number
 * \param func the function
 * \param fmt the printf style format
 * \param args a va_list of arguments
 *
 * \memberof pw_log
 */
SPA_EXPORT
void
pw_log_logv(enum spa_log_level level,
	    const char *file,
	    int line,
	    const char *func,
	    const char *fmt,
	    va_list args)
{
	if (SPA_UNLIKELY(pw_log_level_enabled(level) && global_log)) {
		spa_interface_call(&global_log->iface,
			struct spa_log_methods, logv, 0, level, file, line,
			func, fmt, args);
	}
}

/** \fn void pw_log_error (const char *format, ...)
 * Log an error message
 * \param format a printf style format
 * \param ... printf style arguments
 * \memberof pw_log
 */
/** \fn void pw_log_warn (const char *format, ...)
 * Log a warning message
 * \param format a printf style format
 * \param ... printf style arguments
 * \memberof pw_log
 */
/** \fn void pw_log_info (const char *format, ...)
 * Log an info message
 * \param format a printf style format
 * \param ... printf style arguments
 * \memberof pw_log
 */
/** \fn void pw_log_debug (const char *format, ...)
 * Log a debug message
 * \param format a printf style format
 * \param ... printf style arguments
 * \memberof pw_log
 */
/** \fn void pw_log_trace (const char *format, ...)
 * Log a trace message. Trace messages may be generated from
 * \param format a printf style format
 * \param ... printf style arguments
 * realtime threads
 * \memberof pw_log
 */
