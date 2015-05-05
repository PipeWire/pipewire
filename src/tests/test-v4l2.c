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

#include <gst/gst.h>
#include <gio/gio.h>

#include <client/pulsevideo.h>

#include <modules/v4l2/pv-v4l2-source.h>

static GMainLoop *loop;

static void
on_state_notify (GObject    *gobject,
                 GParamSpec *pspec,
                 gpointer    user_data)
{
  PvContextState state;
  //PvContext *c = user_data;

  g_object_get (gobject, "state", &state, NULL);
  g_print ("got context state %d\n", state);

  switch (state) {
    case PV_CONTEXT_STATE_ERROR:
      g_main_loop_quit (loop);
      break;
    case PV_CONTEXT_STATE_READY:
    {
      //PvSource *source;

      //source = pv_v4l2_source_new (NULL);
      //pv_context_register_source (c, source);
      break;
    }
    default:
      break;
  }
}

gint
main (gint argc, gchar *argv[])
{
  PvContext *c;

  pv_init (&argc, &argv);

  loop = g_main_loop_new (NULL, FALSE);

  c = pv_context_new (NULL, "test-client", NULL);
  g_signal_connect (c, "notify::state", (GCallback) on_state_notify, c);
  pv_context_connect(c, PV_CONTEXT_FLAGS_NONE);

  g_main_loop_run (loop);

  g_object_unref (c);

  return 0;
}
