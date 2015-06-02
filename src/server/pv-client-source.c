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

#include <server/pv-daemon.h>
#include "pv-client-source.h"

#define PV_CLIENT_SOURCE_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PV_TYPE_CLIENT_SOURCE, PvClientSourcePrivate))

struct _PvClientSourcePrivate
{
  GstElement *pipeline;
  GstElement *src;
  GstElement *filter;
  GstElement *sink;

  GSocket *socket;

  PvSourceOutput *input;
};

G_DEFINE_TYPE (PvClientSource, pv_client_source, PV_TYPE_SOURCE);

static gboolean
bus_handler (GstBus * bus, GstMessage * message, gpointer user_data)
{
  PvSource *source = user_data;
  PvClientSourcePrivate *priv = PV_CLIENT_SOURCE (source)->priv;

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
setup_pipeline (PvClientSource *source)
{
  PvClientSourcePrivate *priv = source->priv;
  GstBus *bus;

  priv->pipeline = gst_parse_launch ("socketsrc name=src ! "
                                         "capsfilter name=filter ! "
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
}

static GstCaps *
collect_caps (PvSource * source, GstCaps *filter)
{
  PvClientSourcePrivate *priv = PV_CLIENT_SOURCE (source)->priv;
  GstCaps *res;
  GstQuery *query;

  query = gst_query_new_caps (NULL);
  gst_element_query (priv->filter, query);
  gst_query_parse_caps_result (query, &res);
  gst_caps_ref (res);
  gst_query_unref (query);

  return res;
}

static gboolean
client_set_state (PvSource *source, PvSourceState state)
{
  PvClientSourcePrivate *priv = PV_CLIENT_SOURCE (source)->priv;

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
client_get_formats (PvSource *source, GBytes *filter)
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
  PvClientSource *source = user_data;
  PvClientSourcePrivate *priv = source->priv;
  GSocket *socket;
  guint num_handles;

  g_object_get (gobject, "socket", &socket, NULL);

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
  if (num_handles == 0) {
    gst_element_set_state (priv->pipeline, GST_STATE_READY);
  } else if (socket) {
    GBytes *format;

    /* suggest what we provide */
    g_object_get (priv->input, "format", &format, NULL);
    g_object_set (gobject, "format", format, NULL);
    g_bytes_unref (format);

    gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
  }
}

static PvSourceOutput *
client_create_source_output (PvSource    *source,
                             const gchar *client_path,
                             GBytes      *format_filter,
                             const gchar *prefix,
                             GError      **error)
{
  PvClientSourcePrivate *priv = PV_CLIENT_SOURCE (source)->priv;
  PvSourceOutput *output;

  /* propose format of input */
  g_object_get (priv->input, "format", &format_filter, NULL);

  output = PV_SOURCE_CLASS (pv_client_source_parent_class)
                ->create_source_output (source,
                                        client_path,
                                        format_filter,
                                        prefix,
                                        error);

  if (output == NULL)
    return NULL;

  gst_element_set_state (priv->pipeline, GST_STATE_READY);

  g_signal_connect (output, "notify::socket", (GCallback) on_socket_notify, source);

  return output;
}

static gboolean
client_release_source_output  (PvSource *source, PvSourceOutput *output)
{
  return PV_SOURCE_CLASS (pv_client_source_parent_class)->release_source_output (source, output);
}

static void
client_source_dispose (GObject * object)
{
  PvClientSourcePrivate *priv = PV_CLIENT_SOURCE (object)->priv;

  gst_element_set_state (priv->pipeline, GST_STATE_NULL);

  G_OBJECT_CLASS (pv_client_source_parent_class)->dispose (object);
}

static void
client_source_finalize (GObject * object)
{
  PvClientSourcePrivate *priv = PV_CLIENT_SOURCE (object)->priv;

  g_clear_object (&priv->input);
  g_clear_object (&priv->filter);
  g_clear_object (&priv->sink);
  g_clear_object (&priv->src);
  g_clear_object (&priv->pipeline);

  G_OBJECT_CLASS (pv_client_source_parent_class)->finalize (object);
}


static void
on_input_socket_notify (GObject    *gobject,
                        GParamSpec *pspec,
                        gpointer    user_data)
{
  PvClientSource *source = user_data;
  PvClientSourcePrivate *priv = source->priv;
  GSocket *socket;
  GBytes *requested_format;
  GstCaps *caps;

  g_object_get (gobject, "socket", &socket, NULL);

  if (socket) {
    /* requested format is final format */
    g_object_get (gobject, "requested-format", &requested_format, NULL);
    g_assert (requested_format != NULL);
    g_object_set (gobject, "format", requested_format, NULL);

    /* and set as caps on the filter */
    caps = gst_caps_from_string (g_bytes_get_data (requested_format, NULL));
    g_assert (caps != NULL);
    g_object_set (priv->filter, "caps", caps, NULL);
    gst_caps_unref (caps);
    g_bytes_unref (requested_format);
  } else {
    g_object_set (priv->filter, "caps", NULL, NULL);
  }
  g_object_set (priv->src, "socket", socket, NULL);

  if (socket)
    gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
  else
    gst_element_set_state (priv->pipeline, GST_STATE_READY);
}

PvSourceOutput *
pv_client_source_get_source_input (PvClientSource *source,
                                   const gchar    *client_path,
                                   GBytes         *format_filter,
                                   const gchar    *prefix,
                                   GError         **error)
{
  PvClientSourcePrivate *priv;

  g_return_val_if_fail (PV_IS_CLIENT_SOURCE (source), NULL);
  priv = source->priv;

  if (priv->input == NULL) {
    priv->input = PV_SOURCE_CLASS (pv_client_source_parent_class)
                        ->create_source_output (PV_SOURCE (source),
                                                client_path,
                                                format_filter,
                                                prefix,
                                                error);
    if (priv->input == NULL)
      return NULL;

    g_signal_connect (priv->input, "notify::socket", (GCallback) on_input_socket_notify, source);
  }
  return g_object_ref (priv->input);
}

static void
pv_client_source_class_init (PvClientSourceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PvSourceClass *source_class = PV_SOURCE_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PvClientSourcePrivate));

  gobject_class->dispose = client_source_dispose;
  gobject_class->finalize = client_source_finalize;

  source_class->get_formats = client_get_formats;
  source_class->set_state = client_set_state;
  source_class->create_source_output = client_create_source_output;
  source_class->release_source_output = client_release_source_output;
}

static void
pv_client_source_init (PvClientSource * source)
{
  source->priv = PV_CLIENT_SOURCE_GET_PRIVATE (source);

  setup_pipeline (source);
}

PvSource *
pv_client_source_new (PvDaemon *daemon)
{
  return g_object_new (PV_TYPE_CLIENT_SOURCE, "daemon", daemon, "name", "client-source", NULL);
}
