/* Pinos
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

#ifndef __PINOS_LOG_H__
#define __PINOS_LOG_H__

#include <spa/log.h>

#ifdef __cplusplus
extern "C" {
#endif

SpaLog *      pinos_log_get    (void);


void          pinos_log_log    (SpaLogLevel  level,
                                const char  *file,
                                int          line,
                                const char  *func,
                                const char  *fmt, ...) SPA_PRINTF_FUNC(5, 6);
void          pinos_log_logv   (SpaLogLevel  level,
                                const char  *file,
                                int          line,
                                const char  *func,
                                const char  *fmt,
                                va_list      args) SPA_PRINTF_FUNC(5, 0);

#if __STDC_VERSION__ >= 199901L

#define pinos_log_error(...)           pinos_log_log(SPA_LOG_LEVEL_ERROR,__FILE__,__LINE__,__func__,__VA_ARGS__)
#define pinos_log_warn(...)            pinos_log_log(SPA_LOG_LEVEL_WARN,__FILE__,__LINE__,__func__,__VA_ARGS__)
#define pinos_log_info(...)            pinos_log_log(SPA_LOG_LEVEL_INFO,__FILE__,__LINE__,__func__,__VA_ARGS__)
#define pinos_log_debug(...)           pinos_log_log(SPA_LOG_LEVEL_DEBUG,__FILE__,__LINE__,__func__,__VA_ARGS__)
#define pinos_log_trace(...)           pinos_log_log(SPA_LOG_LEVEL_TRACE,__FILE__,__LINE__,__func__,__VA_ARGS__)

#else

#define PINOS_LOG_FUNC(name,lev)                                                \
static inline void pinos_log_##name (const char *format, ...)                   \
{                                                                               \
  va_list varargs;                                                              \
  va_start (varargs, format);                                                   \
  pinos_log_logv (lev,__FILE__,__LINE__,__func__,format,varargs);               \
  va_end (varargs);                                                             \
}
PINOS_LOG_FUNC(error, SPA_LOG_LEVEL_ERROR)
PINOS_LOG_FUNC(warn, SPA_LOG_LEVEL_WARN)
PINOS_LOG_FUNC(info, SPA_LOG_LEVEL_INFO)
PINOS_LOG_FUNC(debug, SPA_LOG_LEVEL_DEBUG)
PINOS_LOG_FUNC(trace, SPA_LOG_LEVEL_TRACE)

#endif

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_LOG_H__ */
