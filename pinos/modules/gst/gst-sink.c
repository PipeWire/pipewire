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

#include <gst/net/net.h>

#include "gst-sink.h"

#define PINOS_GST_SINK_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_GST_SINK, PinosGstSinkPrivate))

struct _PinosGstSinkPrivate
{
  GstElement *pipeline;
  GstElement *src;
  GstElement *depay;
  GstElement *element;

  PinosPort *input;
  GstCaps *possible_formats;

  GstNetTimeProvider *provider;

  PinosProperties *props;
};

enum {
  PROP_0,
  PROP_ELEMENT,
  PROP_POSSIBLE_FORMATS
};

G_DEFINE_TYPE (PinosGstSink, pinos_gst_sink, PINOS_TYPE_SERVER_NODE);

static gboolean
bus_handler (GstBus     *bus,
             GstMessage *message,
             gpointer    user_data)
{
  PinosNode *node = user_data;
  PinosGstSinkPrivate *priv = PINOS_GST_SINK (node)->priv;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:
    {
      GError *error;
      gchar *debug;

      gst_message_parse_error (message, &error, &debug);
      g_warning ("got error %s (%s)\n", error->message, debug);
      g_free (debug);

      pinos_node_report_error (node, error);
      gst_element_set_state (priv->pipeline, GST_STATE_NULL);
      break;
    }
    case GST_MESSAGE_NEW_CLOCK:
    {
      GstClock *clock;
      PinosProperties *props;

      gst_message_parse_new_clock (message, &clock);
      GST_INFO ("got new clock %s", GST_OBJECT_NAME (clock));

      g_object_get (node, "properties", &props, NULL);
      pinos_properties_set (props, "gst.pipeline.clock", GST_OBJECT_NAME (clock));
      g_object_set (node, "properties", props, NULL);
      pinos_properties_free (props);
      break;
    }
    case GST_MESSAGE_CLOCK_LOST:
    {
      GstClock *clock;
      PinosProperties *props;

      gst_message_parse_new_clock (message, &clock);
      GST_INFO ("clock lost %s", GST_OBJECT_NAME (clock));

      g_object_get (node, "properties", &props, NULL);
      pinos_properties_remove (props, "gst.pipeline.clock");
      g_object_set (node, "properties", props, NULL);
      pinos_properties_free (props);

      gst_element_set_state (priv->pipeline, GST_STATE_PAUSED);
      gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
      break;
    }
    default:
      break;
  }
  return TRUE;
}

static gboolean
setup_pipeline (PinosGstSink *sink, GError **error)
{
  PinosGstSinkPrivate *priv = sink->priv;
  GstBus *bus;

  g_debug ("gst-sink %p: setup pipeline", sink);
  priv->pipeline = gst_pipeline_new (NULL);

  priv->src = gst_element_factory_make ("pinosportsrc", NULL);
  g_object_set (priv->src, "port", priv->input,
                           NULL);

  gst_bin_add (GST_BIN (priv->pipeline), priv->src);

//  priv->depay = gst_element_factory_make ("pinosdepay", NULL);
//  gst_bin_add (GST_BIN (priv->pipeline), priv->depay);
//  gst_element_link (priv->src, priv->depay);

  g_object_set (priv->element, "sync", FALSE, NULL);
  gst_bin_add (GST_BIN (priv->pipeline), priv->element);
  gst_element_link (priv->src, priv->element);

  bus = gst_pipeline_get_bus (GST_PIPELINE (priv->pipeline));
  gst_bus_add_watch (bus, bus_handler, sink);
  gst_object_unref (bus);

  return TRUE;
}

#if 0
static gboolean
start_pipeline (PinosGstSink *sink, GError **error)
{
  PinosGstSinkPrivate *priv = sink->priv;
#if 0
  GstCaps *caps;
  GstQuery *query;
  gchar *str;
#endif
  GstStateChangeReturn ret;

  g_debug ("gst-sink %p: starting pipeline", sink);

  ret = gst_element_set_state (priv->pipeline, GST_STATE_READY);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto ready_failed;

#if 0
  query = gst_query_new_caps (NULL);
  if (gst_element_query (priv->element, query)) {
    gst_query_parse_caps_result (query, &caps);
    gst_caps_replace (&priv->possible_formats, caps);
    str = gst_caps_to_string (caps);
    g_debug ("gst-sink %p: updated possible formats %s", sink, str);
    g_free (str);
  }
  gst_query_unref (query);
#endif

  return TRUE;

  /* ERRORS */
ready_failed:
  {
    GST_ERROR_OBJECT (sink, "failed state change to READY");
    gst_element_set_state (priv->pipeline, GST_STATE_NULL);
    if (error)
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to start pipeline");
    return FALSE;
  }
}
#endif

static void
stop_pipeline (PinosGstSink *sink)
{
  PinosGstSinkPrivate *priv = sink->priv;

  g_debug ("gst-sink %p: stopping pipeline", sink);
  gst_element_set_state (priv->pipeline, GST_STATE_NULL);
  g_clear_object (&priv->provider);
}

static void
destroy_pipeline (PinosGstSink *sink)
{
  PinosGstSinkPrivate *priv = sink->priv;

  g_debug ("gst-sink %p: destroy pipeline", sink);
  stop_pipeline (sink);
  g_clear_object (&priv->pipeline);
}


static gboolean
set_state (PinosNode      *node,
           PinosNodeState  state)
{
  PinosGstSinkPrivate *priv = PINOS_GST_SINK (node)->priv;

  g_debug ("gst-sink %p: set state %s", node, pinos_node_state_as_string (state));

  switch (state) {
    case PINOS_NODE_STATE_SUSPENDED:
      gst_element_set_state (priv->pipeline, GST_STATE_NULL);
      break;

    case PINOS_NODE_STATE_INITIALIZING:
      gst_element_set_state (priv->pipeline, GST_STATE_READY);
      break;

    case PINOS_NODE_STATE_IDLE:
      gst_element_set_state (priv->pipeline, GST_STATE_PAUSED);
      break;

    case PINOS_NODE_STATE_RUNNING:
      gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
      break;

    case PINOS_NODE_STATE_ERROR:
      break;
  }
  pinos_node_update_state (node, state);
  return TRUE;
}

static void
get_property (GObject    *object,
              guint       prop_id,
              GValue     *value,
              GParamSpec *pspec)
{
  PinosGstSink *sink = PINOS_GST_SINK (object);
  PinosGstSinkPrivate *priv = sink->priv;

  switch (prop_id) {
    case PROP_ELEMENT:
      g_value_set_object (value, priv->element);
      break;

    case PROP_POSSIBLE_FORMATS:
      g_value_set_boxed (value, priv->possible_formats);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
set_property (GObject      *object,
              guint         prop_id,
              const GValue *value,
              GParamSpec   *pspec)
{
  PinosGstSink *sink = PINOS_GST_SINK (object);
  PinosGstSinkPrivate *priv = sink->priv;

  switch (prop_id) {
    case PROP_ELEMENT:
      priv->element = g_value_dup_object (value);
      break;

    case PROP_POSSIBLE_FORMATS:
      priv->possible_formats = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
on_linked (PinosPort *port, PinosPort *peer, gpointer user_data)
{
  PinosNode *node = user_data;
  guint n_peers;

  g_debug ("port %p: linked", port);

  pinos_port_get_links (port, &n_peers);
  if (n_peers == 1)
    pinos_node_report_busy (node);
}

static void
on_unlinked (PinosPort *port, PinosPort *peer, gpointer user_data)
{
  PinosNode *node = user_data;
  guint n_peers;

  g_debug ("port %p: unlinked", port);
  pinos_port_get_links (port, &n_peers);
  if (n_peers == 0)
    pinos_node_report_idle (node);
}

static void
on_input_port_created (GObject      *source_object,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  PinosNode *node = PINOS_NODE (source_object);
  PinosGstSink *sink = PINOS_GST_SINK (node);
  PinosGstSinkPrivate *priv = sink->priv;

  priv->input = pinos_node_create_port_finish (node, res, NULL);

  g_signal_connect (priv->input, "linked", (GCallback) on_linked, node);
  g_signal_connect (priv->input, "unlinked", (GCallback) on_unlinked, node);

  setup_pipeline (sink, NULL);
}

static void
sink_constructed (GObject * object)
{
  PinosServerNode *node = PINOS_SERVER_NODE (object);
  PinosGstSink *sink = PINOS_GST_SINK (object);
  PinosGstSinkPrivate *priv = sink->priv;
  gchar *str;
  GBytes *possible_formats;

  G_OBJECT_CLASS (pinos_gst_sink_parent_class)->constructed (object);

  str = gst_caps_to_string (priv->possible_formats);
  possible_formats = g_bytes_new_take (str, strlen (str) + 1);

  pinos_node_create_port (PINOS_NODE (node),
                          PINOS_DIRECTION_INPUT,
                          "input",
                          possible_formats,
                          NULL,
			  NULL,
                          on_input_port_created,
                          node);
  g_bytes_unref (possible_formats);
}

static void
sink_finalize (GObject * object)
{
  PinosServerNode *node = PINOS_SERVER_NODE (object);
  PinosGstSink *sink = PINOS_GST_SINK (object);
  PinosGstSinkPrivate *priv = sink->priv;

  pinos_node_remove_port (PINOS_NODE (node), priv->input);
  destroy_pipeline (sink);
  g_clear_pointer (&priv->possible_formats, gst_caps_unref);
  pinos_properties_free (priv->props);

  G_OBJECT_CLASS (pinos_gst_sink_parent_class)->finalize (object);
}

static void
pinos_gst_sink_class_init (PinosGstSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PinosNodeClass *node_class = PINOS_NODE_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosGstSinkPrivate));

  gobject_class->constructed = sink_constructed;
  gobject_class->finalize = sink_finalize;
  gobject_class->get_property = get_property;
  gobject_class->set_property = set_property;

  g_object_class_install_property (gobject_class,
                                   PROP_ELEMENT,
                                   g_param_spec_object ("element",
                                                        "Element",
                                                        "The element",
                                                        GST_TYPE_ELEMENT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
                                   PROP_POSSIBLE_FORMATS,
                                   g_param_spec_boxed ("possible-formats",
                                                       "Possible Formats",
                                                       "The possible formats",
                                                       GST_TYPE_CAPS,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_STATIC_STRINGS));

  node_class->set_state = set_state;
}

static void
pinos_gst_sink_init (PinosGstSink * sink)
{
  PinosGstSinkPrivate *priv;

  priv = sink->priv = PINOS_GST_SINK_GET_PRIVATE (sink);
  priv->props = pinos_properties_new (NULL, NULL);
}

PinosServerNode *
pinos_gst_sink_new (PinosDaemon *daemon,
                    const gchar *name,
                    PinosProperties *properties,
                    GstElement  *element,
                    GstCaps     *caps)
{
  PinosServerNode *node;

  node = g_object_new (PINOS_TYPE_GST_SINK,
                       "daemon", daemon,
                       "name", name,
                       "properties", properties,
                       "element", element,
                       "possible-formats", caps,
                       NULL);

  return node;
}
