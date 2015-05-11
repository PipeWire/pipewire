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

#include "server/pv-daemon.h"
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

static void
collect_capabilities (PvSource * source)
{
  PvClientSourcePrivate *priv = PV_CLIENT_SOURCE (source)->priv;
  GstCaps *res;
  GstQuery *query;

  query = gst_query_new_caps (NULL);
  gst_element_query (priv->src, query);
  gst_query_parse_caps_result (query, &res);
  g_print ("%s\n", gst_caps_to_string (res));
  gst_query_unref (query);
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
      collect_capabilities (source);
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

static GVariant *
client_get_capabilities (PvSource *source, GVariant *props)
{
  return NULL;
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

  g_print ("source socket %p\n", socket);

  if (socket == NULL) {
    if (priv->socket)
      g_signal_emit_by_name (priv->sink, "remove", priv->socket);
  } else {
    g_signal_emit_by_name (priv->sink, "add", socket);
  }
  priv->socket = socket;

  g_object_get (priv->sink, "num-handles", &num_handles, NULL);
  if (num_handles == 0) {
    gst_element_set_state (priv->pipeline, GST_STATE_READY);
  } else {
    gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
  }
}

static PvSourceOutput *
client_create_source_output (PvSource *source, GVariant *props, const gchar *prefix)
{
  PvClientSourcePrivate *priv = PV_CLIENT_SOURCE (source)->priv;
  PvSourceOutput *output;
  GVariantDict dict;
  GstCaps *caps;
  const gchar *str;
  gint32 i32;

  g_variant_dict_init (&dict, props);
  if (!g_variant_dict_lookup (&dict, "format.encoding", "&s", &str))
    goto invalid_encoding;

  caps = gst_caps_new_empty_simple (str);

  if (g_variant_dict_lookup (&dict, "format.format", "&s", &str))
    gst_caps_set_simple (caps, "format", G_TYPE_STRING, str, NULL);
  if (g_variant_dict_lookup (&dict, "format.width", "i", &i32))
    gst_caps_set_simple (caps, "width", G_TYPE_INT, (gint) i32, NULL);
  if (g_variant_dict_lookup (&dict, "format.height", "i", &i32))
    gst_caps_set_simple (caps, "height", G_TYPE_INT, (gint) i32, NULL);
  if (g_variant_dict_lookup (&dict, "format.views", "i", &i32))
    gst_caps_set_simple (caps, "views", G_TYPE_INT, (gint) i32, NULL);
  if (g_variant_dict_lookup (&dict, "format.chroma-site", "&s", &str))
    gst_caps_set_simple (caps, "chroma-site", G_TYPE_STRING, str, NULL);
  if (g_variant_dict_lookup (&dict, "format.colorimetry", "&s", &str))
    gst_caps_set_simple (caps, "colorimetry", G_TYPE_STRING, str, NULL);
  if (g_variant_dict_lookup (&dict, "format.interlace-mode", "&s", &str))
    gst_caps_set_simple (caps, "interlace-mode", G_TYPE_STRING, str, NULL);

  g_print ("caps %s\n", gst_caps_to_string (caps));

  g_object_set (priv->filter, "caps", caps, NULL);
  gst_caps_unref (caps);

  output = PV_SOURCE_CLASS (pv_client_source_parent_class)->create_source_output (source, props, prefix);

  gst_element_set_state (priv->pipeline, GST_STATE_READY);

  g_signal_connect (output, "notify::socket", (GCallback) on_socket_notify, source);

  return output;

  /* ERRORS */
invalid_encoding:
  {
    g_variant_dict_clear (&dict);
    return NULL;
  }
}

static gboolean
client_release_source_output  (PvSource *source, PvSourceOutput *output)
{
  return PV_SOURCE_CLASS (pv_client_source_parent_class)->release_source_output (source, output);
}

static void
client_source_finalize (GObject * object)
{
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

  g_object_get (gobject, "socket", &socket, NULL);
  g_print ("input socket %p\n", socket);
  g_object_set (priv->src, "socket", socket, NULL);
}

PvSourceOutput *
pv_client_source_get_source_input (PvClientSource *source, GVariant *props, const gchar *prefix)
{
  PvClientSourcePrivate *priv;

  g_return_val_if_fail (PV_IS_CLIENT_SOURCE (source), NULL);
  priv = source->priv;

  if (priv->input == NULL) {
    priv->input = PV_SOURCE_CLASS (pv_client_source_parent_class)->create_source_output (PV_SOURCE (source), props, prefix);
    g_signal_connect (priv->input, "notify::socket", (GCallback) on_input_socket_notify, source);
  }
  return priv->input;
}

static void
pv_client_source_class_init (PvClientSourceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PvSourceClass *source_class = PV_SOURCE_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PvClientSourcePrivate));

  gobject_class->finalize = client_source_finalize;

  source_class->get_capabilities = client_get_capabilities;
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
