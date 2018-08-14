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

#ifndef __PIPEWIRE_LOG_H__
#define __PIPEWIRE_LOG_H__

#include <spa/support/log.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \class pw_log
 *
 * Logging functions of PipeWire
 *
 * Logging is performed to stdout and stderr. Trace logging is performed
 * in a lockfree ringbuffer and written out from the main thread as to not
 * block the realtime threads.
 */

/** The global log level */
extern enum spa_log_level pw_log_level;

void pw_log_set(struct spa_log *log);
struct spa_log *pw_log_get(void);

void
pw_log_set_level(enum spa_log_level level);


void
pw_log_log(enum spa_log_level level,
	   const char *file,
	   int line, const char *func,
	   const char *fmt, ...) SPA_PRINTF_FUNC(5, 6);

void
pw_log_logv(enum spa_log_level level,
	    const char *file,
	    int line, const char *func,
	    const char *fmt, va_list args) SPA_PRINTF_FUNC(5, 0);


/** Check if a loglevel is enabled \memberof pw_log */
#define pw_log_level_enabled(lev) (pw_log_level >= (lev))

#define pw_log(lev,...)							\
({									\
	if (SPA_UNLIKELY(pw_log_level_enabled (lev)))			\
		pw_log_log(lev,__FILE__,__LINE__,__func__,__VA_ARGS__);	\
})

#define pw_log_error(...)   pw_log(SPA_LOG_LEVEL_ERROR,__VA_ARGS__)
#define pw_log_warn(...)    pw_log(SPA_LOG_LEVEL_WARN,__VA_ARGS__)
#define pw_log_info(...)    pw_log(SPA_LOG_LEVEL_INFO,__VA_ARGS__)
#define pw_log_debug(...)   pw_log(SPA_LOG_LEVEL_DEBUG,__VA_ARGS__)
#define pw_log_trace(...)   pw_log(SPA_LOG_LEVEL_TRACE,__VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif /* __PIPEWIRE_LOG_H__ */
