/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <spa/log.h>

#include <pipewire/client/log.h>
#include <pipewire/client/type.h>

#define DEFAULT_LOG_LEVEL SPA_LOG_LEVEL_ERROR

enum spa_log_level pw_log_level = DEFAULT_LOG_LEVEL;

static struct spa_log *global_log = NULL;

/** Set the global log interface
 * \param log the global log to set
 * \memberof pw_log
 */
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
struct spa_log *pw_log_get(void)
{
	return global_log;
}

/** Set the global log level
 * \param level the new log level
 * \memberof pw_log
 */
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
		global_log->logv(global_log, level, file, line, func, fmt, args);
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
void
pw_log_logv(enum spa_log_level level,
	    const char *file,
	    int line,
	    const char *func,
	    const char *fmt,
	    va_list args)
{
	if (SPA_UNLIKELY(pw_log_level_enabled(level) && global_log)) {
		global_log->logv(global_log, level, file, line, func, fmt, args);
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
