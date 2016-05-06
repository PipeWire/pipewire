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

#include <string.h>
#include <gst/gst.h>
#include <gio/gio.h>

#include <client/pinos.h>

#define ANY_CAPS "ANY"

static GMainLoop *loop;

static void
on_socket_notify (GObject    *gobject,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  GSocket *socket;
  GstElement *pipeline, *src, *filter;
  GBytes *format;
  GstCaps *caps;
  GError *error = NULL;

  pipeline = gst_parse_launch ("socketsrc name=src ! pinosdepay ! capsfilter name=filter ! videoconvert ! xvimagesink", &error);
  if (error != NULL) {
    g_warning ("error creating pipeline: %s", error->message);
    g_clear_error (&error);
    g_assert (pipeline != NULL);
  }

  /* configure socket in the socketsrc */
  g_object_get (gobject, "socket", &socket, NULL);
  g_print ("got socket %p\n", socket);
  src = gst_bin_get_by_name (GST_BIN (pipeline), "src");
  g_object_set (src, "socket", socket, NULL);

  /* configure format as capsfilter */
  g_object_get (gobject, "format", &format, NULL);
  caps = gst_caps_from_string (g_bytes_get_data (format, NULL));
  g_bytes_unref (format);
  filter = gst_bin_get_by_name (GST_BIN (pipeline), "filter");
  g_object_set (filter, "caps", caps, NULL);
  gst_caps_unref (caps);

  /* and set to playing */
  gst_element_set_state (pipeline, GST_STATE_PLAYING);
}

static void
on_stream_notify (GObject    *gobject,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  PinosStreamState state;
  PinosStream *s = user_data;

  g_object_get (gobject, "state", &state, NULL);
  g_print ("got stream state %d\n", state);

  switch (state) {
    case PINOS_STREAM_STATE_ERROR:
      g_main_loop_quit (loop);
      break;

    case PINOS_STREAM_STATE_READY:
    {
      GBytes *possible, *format;
      GstCaps *caps;
      GstStructure *structure;
      gchar *str;

      g_object_get (s, "possible-formats", &possible, NULL);

      caps = gst_caps_from_string (g_bytes_get_data (possible, NULL));

      structure = gst_caps_get_structure (caps, 0);

      /* set some reasonable defaults */
      if (gst_structure_has_field (structure, "width"))
        gst_structure_fixate_field_nearest_int (structure, "width", 320);
      if (gst_structure_has_field (structure, "height"))
        gst_structure_fixate_field_nearest_int (structure, "height", 240);
      if (gst_structure_has_field (structure, "framerate"))
        gst_structure_fixate_field_nearest_fraction (structure, "framerate", 30, 1);

      /* use random fixation otherwise */
      caps = gst_caps_fixate (caps);
      str = gst_caps_to_string (caps);
      gst_caps_unref (caps);

      format = g_bytes_new_static (str, strlen (str) + 1);
      pinos_stream_start (s, format, PINOS_STREAM_MODE_SOCKET);
      break;
    }

    case PINOS_STREAM_STATE_STREAMING:
      break;

    default:
      break;
  }
}

static void
on_state_notify (GObject    *gobject,
                 GParamSpec *pspec,
                 gpointer    user_data)
{
  PinosContextState state;
  PinosContext *c = user_data;

  g_object_get (gobject, "state", &state, NULL);
  g_print ("got context state %d\n", state);

  switch (state) {
    case PINOS_CONTEXT_STATE_ERROR:
      g_main_loop_quit (loop);
      break;
    case PINOS_CONTEXT_STATE_READY:
    {
      PinosStream *stream;
      GBytes *format;

      stream = pinos_stream_new (c, "test", NULL);
      g_signal_connect (stream, "notify::state", (GCallback) on_stream_notify, stream);
      g_signal_connect (stream, "notify::socket", (GCallback) on_socket_notify, stream);

      format = g_bytes_new_static (ANY_CAPS, strlen (ANY_CAPS) + 1);
      pinos_stream_connect (stream, PINOS_DIRECTION_OUTPUT, NULL, 0, format);
      g_bytes_unref (format);
      break;
    }
    default:
      break;
  }
}

gint
main (gint argc, gchar *argv[])
{
  PinosContext *c;

  pinos_init (&argc, &argv);

  loop = g_main_loop_new (NULL, FALSE);

  c = pinos_context_new (NULL, "test-client", NULL);
  g_signal_connect (c, "notify::state", (GCallback) on_state_notify, c);
  pinos_context_connect(c, PINOS_CONTEXT_FLAGS_NONE);

  g_main_loop_run (loop);

  g_object_unref (c);

  return 0;
}
