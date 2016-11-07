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

#include <stdio.h>

#include <pinos/client/log.h>

SpaLogLevel pinos_log_level = SPA_LOG_LEVEL_DEBUG;

static void
do_logv (SpaLog        *log,
         SpaLogLevel    level,
         const char    *file,
         int            line,
         const char    *func,
         const char    *fmt,
         va_list        args)
{
  char text[16*1024], location[128];
  static const char *levels[] = {
    "-",
    "E",
    "W",
    "I",
    "D",
    "T",
  };
  vsnprintf (text, sizeof(text), fmt, args);
  if (1) {
    snprintf (location, sizeof(location), "%s:%i %s()", strrchr (file, '/')+1, line, func);
    fprintf(stderr, "[%s][%s] %s\n", levels[level], location, text);
  } else {
    fprintf(stderr, "[%s] %s\n", levels[level], text);
  }
}

static void
do_log (SpaLog        *log,
        SpaLogLevel    level,
        const char    *file,
        int            line,
        const char    *func,
        const char    *fmt, ...)
{
  va_list args;
  va_start (args, fmt);
  do_logv (log, level, file, line, func, fmt, args);
  va_end (args);
}

static SpaLog log = {
  sizeof (SpaLog),
  NULL,
  SPA_LOG_LEVEL_DEBUG,
  do_log,
  do_logv,
};

SpaLog *
pinos_log_get (void)
{
  return &log;
}


void
pinos_log_log (SpaLogLevel  level,
               const char  *file,
               int          line,
               const char  *func,
               const char  *fmt, ...)
{
  if (log.level >= level) {
    va_list args;
    va_start (args, fmt);
    do_logv (&log, level, file, line, func, fmt, args);
    va_end (args);
  }
}

void
pinos_log_logv (SpaLogLevel  level,
                const char  *file,
                int          line,
                const char  *func,
                const char  *fmt,
                va_list      args)
{
  if (log.level >= level) {
    do_logv (&log, level, file, line, func, fmt, args);
  }
}
