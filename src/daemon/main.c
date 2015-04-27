/* Pulsevideo
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
#include <gst/gst.h>

#include <client/pulsevideo.h>
#include <server/pv-daemon.h>
#include <modules/v4l2/pv-v4l2-source.h>

gint
main (gint argc, gchar *argv[])
{
  PvDaemon *daemon;
  GMainLoop *loop;
  PvSource *source;

  pv_init (&argc, &argv);

  loop = g_main_loop_new (NULL, FALSE);

  daemon = pv_daemon_new ();

  source = pv_v4l2_source_new();
  //pv_daemon_add_source (daemon, source);
  pv_daemon_start (daemon);

  g_main_loop_run (loop);

  g_main_loop_unref (loop);
  g_object_unref (daemon);

  return 0;
}
