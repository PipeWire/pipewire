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

#ifndef __SPA_LOG_IMPL_H__
#define __SPA_LOG_IMPL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

#include <spa/support/log.h>

static inline void spa_log_impl_logv(struct spa_log *log,
				     enum spa_log_level level,
				     const char *file,
				     int line,
				     const char *func,
				     const char *fmt,
				     va_list args)
{
        char text[512], location[1024];
        static const char *levels[] = { "-", "E", "W", "I", "D", "T" };

        vsnprintf(text, sizeof(text), fmt, args);
        snprintf(location, sizeof(location), "[%s][%s:%i %s()] %s\n",
                levels[level], strrchr(file, '/') + 1, line, func, text);
        fputs(location, stderr);
}
static inline void spa_log_impl_log(struct spa_log *log,
				    enum spa_log_level level,
				    const char *file,
				    int line,
				    const char *func,
				    const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	spa_log_impl_logv(log, level, file, line, func, fmt, args);
	va_end(args);
}

#define SPA_LOG_IMPL_DEFINE(name)		\
struct {					\
	struct spa_log log;			\
} name

#define SPA_LOG_IMPL_INIT			\
        { { SPA_VERSION_LOG,			\
            NULL,				\
	    SPA_LOG_LEVEL_INFO,			\
	    spa_log_impl_log,			\
	    spa_log_impl_logv,} }

#define SPA_LOG_IMPL(name)			\
        SPA_LOG_IMPL_DEFINE(name) = SPA_LOG_IMPL_INIT

#ifdef __cplusplus
}  /* extern "C" */
#endif
#endif /* __SPA_LOG_IMPL_H__ */
