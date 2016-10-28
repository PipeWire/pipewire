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

typedef struct _SpaLog SpaLog;

#define SPA_LOG_URI             "http://spaplug.in/ns/log"
#define SPA_LOG_PREFIX          SPA_LOG_URI "#"

#include <stdarg.h>

#include <spa/defs.h>
#include <spa/plugin.h>

typedef enum
{
  SPA_LOG_LEVEL_NONE   = 0,
  SPA_LOG_LEVEL_ERROR,
  SPA_LOG_LEVEL_WARN,
  SPA_LOG_LEVEL_INFO,
  SPA_LOG_LEVEL_DEBUG,
  SPA_LOG_LEVEL_TRACE,
} SpaLogLevel;

/**
 * SpaLog:
 *
 * The Log interface
 */
struct _SpaLog {
  /* the total size of this log. This can be used to expand this
   * structure in the future */
  size_t size;
  /**
   * SpaLog::info
   *
   * Extra information about the log
   */
  const SpaDict *info;

  /**
   * SpaLog::level
   *
   * Logging level, everything above this level is not logged
   */
  SpaLogLevel level;

  /**
   * SpaLog::log
   * @log: a #SpaLog
   * @level: a #SpaLogLevel
   * @file: the file name
   * @line: the line number
   * @func: the function name
   * @fmt: printf style format
   * @...: format arguments
   *
   * Log a message with the given log level.
   */
  void   (*log)           (SpaLog        *log,
                           SpaLogLevel    level,
                           const char    *file,
                           int            line,
                           const char    *func,
                           const char    *fmt, ...) SPA_PRINTF_FUNC(6, 7);

  /**
   * SpaLog::logv
   * @log: a #SpaLog
   * @level: a #SpaLogLevel
   * @file: the file name
   * @line: the line number
   * @func: the function name
   * @fmt: printf style format
   * @args: format arguments
   *
   * Log a message with the given log level.
   */
  void   (*logv)          (SpaLog        *log,
                           SpaLogLevel    level,
                           const char    *file,
                           int            line,
                           const char    *func,
                           const char    *fmt,
                           va_list        args) SPA_PRINTF_FUNC(6, 0);
};

#define spa_log_level_enabled(l,lev) ((l) && (l)->level >= (lev))

#if __STDC_VERSION__ >= 199901L

#define spa_log_log(l,lev,...)                          \
  if (SPA_UNLIKELY (spa_log_level_enabled (l, lev)))    \
    (l)->log((l),lev,__VA_ARGS__)

#define spa_log_error(l,...)           spa_log_log(l,SPA_LOG_LEVEL_ERROR,__FILE__,__LINE__,__func__,__VA_ARGS__)
#define spa_log_warn(l,...)            spa_log_log(l,SPA_LOG_LEVEL_WARN,__FILE__,__LINE__,__func__,__VA_ARGS__)
#define spa_log_info(l,...)            spa_log_log(l,SPA_LOG_LEVEL_INFO,__FILE__,__LINE__,__func__,__VA_ARGS__)
#define spa_log_debug(l,...)           spa_log_log(l,SPA_LOG_LEVEL_DEBUG,__FILE__,__LINE__,__func__,__VA_ARGS__)
#define spa_log_trace(l,...)           spa_log_log(l,SPA_LOG_LEVEL_TRACE,__FILE__,__LINE__,__func__,__VA_ARGS__)

#else

#define SPA_LOG_FUNC(name,lev)                                                  \
static inline void spa_log_##name (SpaLog *l, const char *format, ...)          \
{                                                                               \
  if (SPA_UNLIKELY (spa_log_level_enabled (l, lev))) {                          \
    va_list varargs;                                                            \
    va_start (varargs, format);                                                 \
    (l)->logv((l),lev,__FILE__,__LINE__,__func__,format,varargs);               \
    va_end (varargs);                                                           \
  }                                                                             \
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
