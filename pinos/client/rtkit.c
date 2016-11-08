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
#include <string.h>
#include <errno.h>

#include "rtkit.h"

#include <gio/gio.h>

struct _PinosRTKitBus {
  GDBusConnection *bus;
};

static pid_t _gettid(void) {
  return (pid_t) syscall(SYS_gettid);
}

static int
translate_error (GError *error)
{
  const gchar *name = g_dbus_error_get_remote_error (error);

  if (strcmp (name, "org.freedesktop.DBus.Error.NoMemory") == 0)
    return -ENOMEM;
  if (strcmp (name, "org.freedesktop.DBus.Error.ServiceUnknown") == 0 ||
      strcmp (name, "org.freedesktop.DBus.Error.NameHasNoOwner") == 0)
    return -ENOENT;
  if (strcmp (name, "org.freedesktop.DBus.Error.AccessDenied") == 0 ||
      strcmp (name, "org.freedesktop.DBus.Error.AuthFailed") == 0)
    return -EACCES;

  return -EIO;
}

PinosRTKitBus *
pinos_rtkit_bus_get_system   (void)
{
  PinosRTKitBus *bus;

  bus = g_slice_new (PinosRTKitBus);
  bus->bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);

  return bus;
}

void
pinos_rtkit_bus_free (PinosRTKitBus *system_bus)
{
  g_object_unref (system_bus->bus);
  g_slice_free (PinosRTKitBus, system_bus);
}

int
pinos_rtkit_make_realtime (PinosRTKitBus       *system_bus,
                           pid_t                thread,
                           int                  priority)
{
  GVariant *v;
  GError *error = NULL;

  if (thread == 0)
    thread = _gettid();

  v = g_dbus_connection_call_sync (system_bus->bus,
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
                                   &error);
  if (v == NULL)
    return translate_error (error);

  g_variant_unref (v);

  return 0;
}

int
pinos_rtkit_make_high_priority (PinosRTKitBus       *system_bus,
                                pid_t                thread,
                                int                  nice_level)
{
  GVariant *v;
  GError *error = NULL;

  if (thread == 0)
    thread = _gettid();

  v = g_dbus_connection_call_sync (system_bus->bus,
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
                                   &error);
  if (v == NULL)
    return translate_error (error);
  g_variant_unref (v);

  return 0;
}

int pinos_rtkit_get_max_realtime_priority (PinosRTKitBus *system_bus)
{
  return 0;
}

int pinos_rtkit_get_min_nice_level (PinosRTKitBus *system_bus, int* min_nice_level)
{
  return 0;
}

/* Return the maximum value of RLIMIT_RTTIME to set before attempting a
 * realtime request. A negative value is an errno style error code.
 */
long long pinos_rtkit_get_rttime_usec_max (PinosRTKitBus *system_bus)
{
  return 0;
}
