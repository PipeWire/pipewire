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

#include <gio/gio.h>
#include <glib.h>

#include <client/pinos.h>
#include <server/daemon.h>
#include <server/module.h>
#include <spa/include/spa/memory.h>

gint
main (gint argc, gchar *argv[])
{
  PinosDaemon *daemon;
  GMainLoop *loop;
  PinosProperties *props;
  GError *err = NULL;

  pinos_init (&argc, &argv);
  spa_memory_init ();

  loop = g_main_loop_new (NULL, FALSE);

  props = pinos_properties_new ("test", "test", NULL);
  daemon = pinos_daemon_new (props);

  if (pinos_module_load (daemon, "module-gst", &err) == NULL) {
    g_error ("could not load module-gst: %s", err->message);
    g_clear_error (&err);
  }

  pinos_spa_alsa_sink_new (daemon, "alsa-sink", NULL);
  pinos_spa_v4l2_source_new (daemon, "v4l2-source", NULL);
  pinos_daemon_start (daemon);

  g_main_loop_run (loop);

  pinos_properties_free (props);
  g_main_loop_unref (loop);
  g_object_unref (daemon);

  return 0;
}
