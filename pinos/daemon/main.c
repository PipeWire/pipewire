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

#include <pinos/client/pinos.h>
#include <pinos/server/daemon.h>
#include <pinos/server/module.h>

#include "daemon-config.h"

gint
main (gint argc, gchar *argv[])
{
  PinosDaemon *daemon;
  GMainLoop *loop;
  PinosDaemonConfig *config;
  PinosProperties *props;
  GError *err = NULL;

  pinos_init (&argc, &argv);

  loop = g_main_loop_new (NULL, FALSE);

  /* parse configuration */
  config = pinos_daemon_config_new ();
  if (!pinos_daemon_config_load (config, &err)) {
    g_error ("failed to parse config: %s", err->message);
    g_clear_error (&err);
  }

  props = pinos_properties_new ("test", "test", NULL);
  daemon = pinos_daemon_new (props);

  pinos_daemon_config_run_commands (config, daemon);

  pinos_daemon_start (daemon);

  g_main_loop_run (loop);

  pinos_properties_free (props);
  g_main_loop_unref (loop);
  g_object_unref (daemon);

  return 0;
}
