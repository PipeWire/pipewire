/* Simple Plugin API
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_LOG_H__
#define __SPA_LOG_H__

#ifdef __cplusplus
extern "C" {
#endif

#define SPA_TYPE__Log		SPA_TYPE_INTERFACE_BASE "Log"
#define SPA_TYPE_LOG_BASE	SPA_TYPE__Log ":"

#include <stdarg.h>

#include <spa/utils/defs.h>

enum spa_log_level {
	SPA_LOG_LEVEL_NONE = 0,
	SPA_LOG_LEVEL_ERROR,
	SPA_LOG_LEVEL_WARN,
	SPA_LOG_LEVEL_INFO,
	SPA_LOG_LEVEL_DEBUG,
	SPA_LOG_LEVEL_TRACE,
};

/**
 * The Log interface
 */
struct spa_log {
	/** the version of this log. This can be used to expand this
	 * structure in the future */
#define SPA_VERSION_LOG	0
	uint32_t version;
	/**
	 * Extra information about the log
	 */
	const struct spa_dict *info;

	/**
	 * Logging level, everything above this level is not logged
	 */
	enum spa_log_level level;

	/**
	 * Log a message with the given log level.
	 *
	 * \param log a spa_log
	 * \param level a spa_log_level
	 * \param file the file name
	 * \param line the line number
	 * \param func the function name
	 * \param fmt printf style format
	 * \param ... format arguments
	 */
	void (*log) (struct spa_log *log,
		     enum spa_log_level level,
		     const char *file,
		     int line,
		     const char *func,
		     const char *fmt, ...) SPA_PRINTF_FUNC(6, 7);

	/**
	 * Log a message with the given log level.
	 *
	 * \param log a spa_log
	 * \param level a spa_log_level
	 * \param file the file name
	 * \param line the line number
	 * \param func the function name
	 * \param fmt printf style format
	 * \param args format arguments
	 */
	void (*logv) (struct spa_log *log,
		      enum spa_log_level level,
		      const char *file,
		      int line,
		      const char *func,
		      const char *fmt,
		      va_list args) SPA_PRINTF_FUNC(6, 0);
};

#define spa_log_level_enabled(l,lev) ((l) && (l)->level >= (lev))

#if defined(__USE_ISOC11) || defined(__USE_ISOC99) || \
    (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L)

#define spa_log_log(l,lev,...)					\
({								\
	if (SPA_UNLIKELY (spa_log_level_enabled (l, lev)))	\
		(l)->log((l),lev,__VA_ARGS__);			\
})

#define spa_log_error(l,...)	spa_log_log(l,SPA_LOG_LEVEL_ERROR,__FILE__,__LINE__,__func__,__VA_ARGS__)
#define spa_log_warn(l,...)	spa_log_log(l,SPA_LOG_LEVEL_WARN,__FILE__,__LINE__,__func__,__VA_ARGS__)
#define spa_log_info(l,...)	spa_log_log(l,SPA_LOG_LEVEL_INFO,__FILE__,__LINE__,__func__,__VA_ARGS__)
#define spa_log_debug(l,...)	spa_log_log(l,SPA_LOG_LEVEL_DEBUG,__FILE__,__LINE__,__func__,__VA_ARGS__)
#define spa_log_trace(l,...)	spa_log_log(l,SPA_LOG_LEVEL_TRACE,__FILE__,__LINE__,__func__,__VA_ARGS__)

#else

#define SPA_LOG_FUNC(name,lev)							\
static inline void spa_log_##name (struct spa_log *l, const char *format, ...)  \
{										\
	if (SPA_UNLIKELY (spa_log_level_enabled (l, lev))) {			\
		va_list varargs;						\
		va_start (varargs, format);					\
		(l)->logv((l),lev,__FILE__,__LINE__,__func__,format,varargs);	\
		va_end (varargs);						\
	}									\
}

SPA_LOG_FUNC(error, SPA_LOG_LEVEL_ERROR)
SPA_LOG_FUNC(warn, SPA_LOG_LEVEL_WARN)
SPA_LOG_FUNC(info, SPA_LOG_LEVEL_INFO)
SPA_LOG_FUNC(debug, SPA_LOG_LEVEL_DEBUG)
SPA_LOG_FUNC(trace, SPA_LOG_LEVEL_TRACE)

#endif
#ifdef __cplusplus
}  /* extern "C" */
#endif
#endif /* __SPA_LOG_H__ */
