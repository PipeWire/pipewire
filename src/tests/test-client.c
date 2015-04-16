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

#include <client/pv-context.h>
#include <client/pv-stream.h>

#define CAPS "video/x-raw, format=(string)YUY2, width=(int)320, height=(int)240, pixel-aspect-ratio=(fraction)1/1, interlace-mode=(string)progressive, framerate=(fraction)30/1"

static GMainLoop *loop;

static void
on_socket_notify (GObject    *gobject,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  GSocket *socket;
  GstElement *pipeline, *src;

  g_object_get (gobject, "socket", &socket, NULL);
  g_print ("got socket %p\n", socket);

  pipeline = gst_parse_launch ("socketsrc name=src ! pvfddepay ! "CAPS" ! videoconvert ! xvimagesink", NULL);
  src = gst_bin_get_by_name (GST_BIN (pipeline), "src");

  g_object_set (src, "socket", socket, NULL);

  gst_element_set_state (pipeline, GST_STATE_PLAYING);
}

static void
on_stream_notify (GObject    *gobject,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  PvStreamState state;
  PvStream *s = user_data;

  g_object_get (gobject, "state", &state, NULL);
  g_print ("got stream state %d\n", state);

  switch (state) {
    case PV_STREAM_STATE_ERROR:
      g_main_loop_quit (loop);
      break;
    case PV_STREAM_STATE_READY:
      pv_stream_start (s, PV_STREAM_MODE_SOCKET);
      break;
    case PV_STREAM_STATE_STREAMING:
    {
      PvBufferInfo info;

      pv_stream_capture_buffer (s, &info);
      break;
    }
    default:
      break;
  }
}


static void
on_state_notify (GObject    *gobject,
                 GParamSpec *pspec,
                 gpointer    user_data)
{
  PvContextState state;
  PvContext *c = user_data;

  g_object_get (gobject, "state", &state, NULL);
  g_print ("got context state %d\n", state);

  switch (state) {
    case PV_CONTEXT_STATE_ERROR:
      g_main_loop_quit (loop);
      break;
    case PV_CONTEXT_STATE_READY:
    {
      PvStream *stream;

      stream = pv_stream_new (c, "test");
      g_signal_connect (stream, "notify::state", (GCallback) on_stream_notify, stream);
      g_signal_connect (stream, "notify::socket", (GCallback) on_socket_notify, stream);
      pv_stream_connect_capture (stream, NULL, 0);
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

  gst_init (&argc, &argv);

  loop = g_main_loop_new (NULL, FALSE);

  c = pv_context_new ("test-client", NULL);
  g_signal_connect (c, "notify::state", (GCallback) on_state_notify, c);
  pv_context_connect(c, PV_CONTEXT_FLAGS_NONE);

  g_main_loop_run (loop);

  g_object_unref (c);

  return 0;
}
