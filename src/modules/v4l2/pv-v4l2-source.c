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

#include <string.h>
#include <gst/gst.h>
#include <gio/gio.h>

#include "pv-v4l2-source.h"

#define PV_V4L2_SOURCE_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PV_TYPE_V4L2_SOURCE, PvV4l2SourcePrivate))

struct _PvV4l2SourcePrivate
{
  GstElement *pipeline;
  GstElement *src;
  GstElement *filter;
  GstElement *sink;

  GstCaps *possible_formats;
};

G_DEFINE_TYPE (PvV4l2Source, pv_v4l2_source, PV_TYPE_SOURCE);

static gboolean
bus_handler (GstBus * bus, GstMessage * message, gpointer user_data)
{
  PvSource *source = user_data;
  PvV4l2SourcePrivate *priv = PV_V4L2_SOURCE (source)->priv;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:
    {
      GError *error;
      gchar *debug;

      gst_message_parse_error (message, &error, &debug);
      g_print ("got error %s (%s)\n", error->message, debug);
      g_free (debug);

      pv_source_report_error (source, error);
      gst_element_set_state (priv->pipeline, GST_STATE_NULL);
      break;
    }
    default:
      break;
  }
  return TRUE;
}

static void
setup_pipeline (PvV4l2Source *source)
{
  PvV4l2SourcePrivate *priv = source->priv;
  GstBus *bus;

  priv->pipeline = gst_parse_launch ("v4l2src name=src ! "
                                         "capsfilter name=filter ! "
                                     "pvfdpay ! "
                                     "multisocketsink "
                                         "buffers-max=2 "
                                         "buffers-soft-max=1 "
                                         "recover-policy=latest "
                                         "sync-method=latest "
                                         "name=sink "
                                         "sync=true "
                                         "enable-last-sample=false",
                                      NULL);
  priv->filter = gst_bin_get_by_name (GST_BIN (priv->pipeline), "filter");
  priv->sink = gst_bin_get_by_name (GST_BIN (priv->pipeline), "sink");
  priv->src = gst_bin_get_by_name (GST_BIN (priv->pipeline), "src");

  bus = gst_pipeline_get_bus (GST_PIPELINE (priv->pipeline));
  gst_bus_add_watch (bus, bus_handler, source);
  gst_object_unref (bus);

  gst_element_set_state (priv->pipeline, GST_STATE_READY);
}

static GstCaps *
collect_caps (PvSource * source, GstCaps *filter)
{
  PvV4l2SourcePrivate *priv = PV_V4L2_SOURCE (source)->priv;
  GstCaps *res;
  GstQuery *query;

  query = gst_query_new_caps (filter);
  gst_element_query (priv->filter, query);
  gst_query_parse_caps_result (query, &res);
  gst_caps_ref (res);
  gst_query_unref (query);

  return res;
}

static gboolean
v4l2_set_state (PvSource *source, PvSourceState state)
{
  PvV4l2SourcePrivate *priv = PV_V4L2_SOURCE (source)->priv;

  switch (state) {
    case PV_SOURCE_STATE_SUSPENDED:
      gst_element_set_state (priv->pipeline, GST_STATE_NULL);
      break;

    case PV_SOURCE_STATE_INIT:
      gst_element_set_state (priv->pipeline, GST_STATE_READY);
      break;

    case PV_SOURCE_STATE_IDLE:
      gst_element_set_state (priv->pipeline, GST_STATE_PAUSED);
      break;

    case PV_SOURCE_STATE_RUNNING:
      gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
      break;

    case PV_SOURCE_STATE_ERROR:
      break;
  }
  pv_source_update_state (source, state);
  return TRUE;
}

static GBytes *
v4l2_get_formats (PvSource *source, GBytes *filter)
{
  GstCaps *caps, *cfilter;
  gchar *str;

  cfilter = gst_caps_from_string (g_bytes_get_data (filter, NULL));
  if (cfilter == NULL)
    return NULL;

  caps = collect_caps (source, cfilter);
  if (caps == NULL)
    return NULL;

  str = gst_caps_to_string (caps);

  return g_bytes_new_take (str, strlen (str) + 1);
}

static void
on_socket_notify (GObject    *gobject,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  PvV4l2Source *source = user_data;
  PvV4l2SourcePrivate *priv = source->priv;
  GSocket *socket;
  guint num_handles;
  GstCaps *caps;
  GBytes *requested_format, *format;

  g_object_get (gobject, "socket", &socket, NULL);

  g_print ("source socket %p\n", socket);

  if (socket == NULL) {
    GSocket *prev_socket = g_object_get_data (gobject, "last-socket");
    if (prev_socket) {
      g_signal_emit_by_name (priv->sink, "remove", prev_socket);
    }
  } else {
    g_signal_emit_by_name (priv->sink, "add", socket);
  }
  g_object_set_data (gobject, "last-socket", socket);

  g_object_get (priv->sink, "num-handles", &num_handles, NULL);
  g_print ("num handles %d\n", num_handles);
  if (num_handles == 0) {
    gst_element_set_state (priv->pipeline, GST_STATE_READY);
    g_object_set (priv->filter, "caps", NULL, NULL);
  } else if (socket) {
    /* what client requested */
    g_object_get (gobject, "requested-format", &requested_format, NULL);
    g_assert (requested_format != NULL);

    if (num_handles == 1) {
      /* first client, we set the requested format as the format */
      format = requested_format;

      /* set on the filter */
      caps = gst_caps_from_string (g_bytes_get_data (format, NULL));
      g_assert (caps != NULL);
      g_object_set (priv->filter, "caps", caps, NULL);
      gst_caps_unref (caps);
    } else {
      gchar *str;

      /* we already have a client, format is whatever is configured already */
      g_bytes_unref (requested_format);

      g_object_get (priv->filter, "caps", &caps, NULL);
      str = gst_caps_to_string (caps);
      format = g_bytes_new (str, strlen (str) + 1);
      gst_caps_unref (caps);
    }
    /* this is what we use as the final format for the output */
    g_print ("final format %s\n", (gchar *) g_bytes_get_data (format, NULL));
    g_object_set (gobject, "format", format, NULL);
    g_bytes_unref (format);

    gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
  }
}

static PvSourceOutput *
v4l2_create_source_output (PvSource    *source,
                           const gchar *client_path,
                           GBytes      *format_filter,
                           const gchar *prefix,
                           GError      **error)
{
  PvSourceOutput *output;
  GstCaps *caps, *filtered;
  gchar *str;

  str = (gchar *) g_bytes_get_data (format_filter, NULL);
  g_print ("input filter %s\n", str);
  caps = gst_caps_from_string (str);
  if (caps == NULL)
    goto invalid_caps;

  filtered = collect_caps (source, caps);
  if (filtered == NULL || gst_caps_is_empty (filtered))
    goto no_format;

  str = gst_caps_to_string (filtered);
  g_print ("output filter %s\n", str);
  format_filter = g_bytes_new_take (str, strlen (str) + 1);

  output = PV_SOURCE_CLASS (pv_v4l2_source_parent_class)
                ->create_source_output (source,
                                        client_path,
                                        format_filter,
                                        prefix,
                                        error);
  if (error == NULL)
    return NULL;

  g_signal_connect (output, "notify::socket", (GCallback) on_socket_notify, source);

  return output;

  /* ERRORS */
invalid_caps:
  {
    if (error)
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "Input filter data invalid");
    return NULL;
  }
no_format:
  {
    if (error)
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "No format available that matches input filter");
    return NULL;
  }
}

static gboolean
v4l2_release_source_output  (PvSource *source, PvSourceOutput *output)
{
  return PV_SOURCE_CLASS (pv_v4l2_source_parent_class)->release_source_output (source, output);
}

static void
v4l2_source_finalize (GObject * object)
{
  G_OBJECT_CLASS (pv_v4l2_source_parent_class)->finalize (object);
}

static void
pv_v4l2_source_class_init (PvV4l2SourceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PvSourceClass *source_class = PV_SOURCE_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PvV4l2SourcePrivate));

  gobject_class->finalize = v4l2_source_finalize;

  source_class->get_formats = v4l2_get_formats;
  source_class->set_state = v4l2_set_state;
  source_class->create_source_output = v4l2_create_source_output;
  source_class->release_source_output = v4l2_release_source_output;
}

static void
pv_v4l2_source_init (PvV4l2Source * source)
{
  source->priv = PV_V4L2_SOURCE_GET_PRIVATE (source);

  setup_pipeline (source);
}

PvSource *
pv_v4l2_source_new (PvDaemon *daemon)
{
  return g_object_new (PV_TYPE_V4L2_SOURCE, "daemon", daemon, "name", "v4l2", NULL);
}
