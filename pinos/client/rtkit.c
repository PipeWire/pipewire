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

#include <sys/syscall.h>

#include "rtkit.h"

static pid_t _gettid(void) {
  return (pid_t) syscall(SYS_gettid);
}

gboolean
pinos_rtkit_make_realtime (GDBusConnection     *system_bus,
                           pid_t                thread,
                           gint                 priority,
                           GError             **error)
{
  GVariant *v;

  if (thread == 0)
    thread = _gettid();

  v = g_dbus_connection_call_sync (system_bus,
                                   RTKIT_SERVICE_NAME,
                                   RTKIT_OBJECT_PATH,
                                   "org.freedesktop.RealtimeKit1",
                                   "MakeThreadRealtime",
                                   g_variant_new ("(tu)",
                                     (guint64) thread,
                                     (guint32) priority),
                                   NULL,
                                   G_DBUS_CALL_FLAGS_NONE,
                                   -1,
                                   NULL,
                                   error);
  if (v)
    g_variant_unref (v);

  return v != NULL;
}

gboolean
pinos_rtkit_make_high_priority (GDBusConnection     *system_bus,
                                pid_t                thread,
                                gint                 nice_level,
                                GError             **error)
{
  GVariant *v;

  if (thread == 0)
    thread = _gettid();

  v = g_dbus_connection_call_sync (system_bus,
                                   RTKIT_SERVICE_NAME,
                                   RTKIT_OBJECT_PATH,
                                   "org.freedesktop.RealtimeKit1",
                                   "MakeThreadHighPriority",
                                   g_variant_new ("(tu)",
                                     (guint64) thread,
                                     (guint32) nice_level),
                                   NULL,
                                   G_DBUS_CALL_FLAGS_NONE,
                                   -1,
                                   NULL,
                                   error);
  if (v)
    g_variant_unref (v);

  return v != NULL;
}

int pinos_rtkit_get_max_realtime_priority (GDBusConnection *system_bus)
{
  return 0;
}

int pinos_rtkit_get_min_nice_level (GDBusConnection *system_bus, int* min_nice_level)
{
  return 0;
}

/* Return the maximum value of RLIMIT_RTTIME to set before attempting a
 * realtime request. A negative value is an errno style error code.
 */
long long rtkit_get_rttime_usec_max (GDBusConnection *system_bus)
{
  return 0;
}
