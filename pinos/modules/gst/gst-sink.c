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

typedef struct {
  PinosGstSink *sink;

  PinosPort *port;

  GstElement *src;
  GstElement *convert;

  GstPad *srcpad;
  GstPad *peerpad;
} SinkPortData;

struct _PinosGstSinkPrivate
{
  gchar *convert_name;

  GstElement *pipeline;
  GstElement *mixer;
  GstElement *element;

  GList *ports;
  GstCaps *possible_formats;

  GstNetTimeProvider *provider;
};

enum {
  PROP_0,
  PROP_ELEMENT,
  PROP_POSSIBLE_FORMATS,
  PROP_MIXER,
  PROP_CONVERT_NAME
};

G_DEFINE_TYPE (PinosGstSink, pinos_gst_sink, PINOS_TYPE_NODE);

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

  g_object_set (priv->element, "sync", FALSE, NULL);
  gst_bin_add (GST_BIN (priv->pipeline), priv->element);

  if (priv->mixer) {
    gst_bin_add (GST_BIN (priv->pipeline), priv->mixer);
    gst_element_link (priv->mixer, priv->element);
  }

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

    case PROP_MIXER:
      g_value_set_object (value, priv->mixer);
      break;

    case PROP_CONVERT_NAME:
      g_value_set_string (value, priv->convert_name);
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

    case PROP_MIXER:
      priv->mixer = g_value_dup_object (value);
      break;

    case PROP_CONVERT_NAME:
      priv->convert_name = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
on_linked (PinosPort *port, PinosPort *peer, gpointer user_data)
{
  SinkPortData *data = user_data;
  PinosGstSink *sink = data->sink;
  PinosGstSinkPrivate *priv = sink->priv;

  g_debug ("port %p: linked", port);

  if (priv->mixer) {
    data->peerpad = gst_element_get_request_pad (priv->mixer, "sink_%u");
  } else {
    data->peerpad = gst_element_get_static_pad (priv->element, "sink");
  }
  if (gst_pad_link (data->srcpad, data->peerpad) != GST_PAD_LINK_OK) {
    g_clear_object (&data->peerpad);
    return FALSE;
  }

  pinos_node_report_busy (PINOS_NODE (sink));

  if (data->convert) {
    gst_element_set_state (data->convert, GST_STATE_PLAYING);
  }
  gst_element_set_state (data->src, GST_STATE_PLAYING);

  return TRUE;
}

static void
on_unlinked (PinosPort *port, PinosPort *peer, gpointer user_data)
{
  SinkPortData *data = user_data;
  PinosGstSink *sink = data->sink;
  PinosGstSinkPrivate *priv = sink->priv;

  g_debug ("port %p: unlinked", port);

  if (data->convert) {
    gst_element_set_state (data->convert, GST_STATE_NULL);
  }
  gst_element_set_state (data->src, GST_STATE_NULL);

  gst_pad_unlink (data->srcpad, data->peerpad);
  if (priv->mixer)
    gst_element_release_request_pad (priv->mixer, data->peerpad);
  g_clear_object (&data->peerpad);
}

static void
free_sink_port_data (SinkPortData *data)
{
  PinosGstSink *sink = data->sink;
  PinosGstSinkPrivate *priv = sink->priv;

  gst_element_set_state (data->src, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (priv->pipeline), data->src);

  if (data->convert) {
    gst_element_set_state (data->convert, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (priv->pipeline), data->convert);
  }
  if (data->peerpad)
    gst_element_release_request_pad (priv->mixer, data->peerpad);

  g_clear_object (&data->srcpad);
  g_clear_object (&data->peerpad);

  g_slice_free (SinkPortData, data);
}

static PinosPort *
add_port (PinosNode       *node,
          PinosDirection   direction,
          GError         **error)
{
  PinosGstSink *sink = PINOS_GST_SINK (node);
  PinosGstSinkPrivate *priv = sink->priv;
  SinkPortData *data;

  data = g_slice_new0 (SinkPortData);
  data->sink = sink;

  data->port = PINOS_NODE_CLASS (pinos_gst_sink_parent_class)
                ->add_port (node, direction, error);

  g_debug ("connecting signals");
  g_signal_connect (data->port, "linked", (GCallback) on_linked, data);
  g_signal_connect (data->port, "unlinked", (GCallback) on_unlinked, data);

  data->src = gst_element_factory_make ("pinosportsrc", NULL);
  g_object_set (data->src, "port", data->port, NULL);
  gst_bin_add (GST_BIN (priv->pipeline), data->src);

  if (priv->convert_name) {
    data->convert = gst_element_factory_make (priv->convert_name, NULL);
    gst_bin_add (GST_BIN (priv->pipeline), data->convert);
    gst_element_link (data->src, data->convert);
    data->srcpad = gst_element_get_static_pad (data->convert, "src");
  } else {
    data->srcpad = gst_element_get_static_pad (data->src, "src");
  }

  priv->ports = g_list_append (priv->ports, data);

  return data->port;
}

static void
remove_port (PinosNode       *node,
             PinosPort       *port)
{
  PinosGstSink *sink = PINOS_GST_SINK (node);
  PinosGstSinkPrivate *priv = sink->priv;
  GList *walk;

  for (walk = priv->ports; walk; walk = g_list_next (walk)) {
    SinkPortData *data = walk->data;

    if (data->port == PINOS_PORT_CAST (port)) {
      free_sink_port_data (data);
      priv->ports = g_list_delete_link (priv->ports, walk);
      break;
    }
  }
  if (priv->ports == NULL)
    pinos_node_report_idle (node);
}

static void
sink_constructed (GObject * object)
{
  PinosGstSink *sink = PINOS_GST_SINK (object);

  G_OBJECT_CLASS (pinos_gst_sink_parent_class)->constructed (object);

  setup_pipeline (sink, NULL);
}

static void
sink_finalize (GObject * object)
{
  PinosGstSink *sink = PINOS_GST_SINK (object);
  PinosGstSinkPrivate *priv = sink->priv;

  destroy_pipeline (sink);
  g_clear_pointer (&priv->possible_formats, gst_caps_unref);

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
  g_object_class_install_property (gobject_class,
                                   PROP_MIXER,
                                   g_param_spec_object ("mixer",
                                                        "Mixer",
                                                        "The mixer element",
                                                        GST_TYPE_ELEMENT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
                                   PROP_CONVERT_NAME,
                                   g_param_spec_string ("convert-name",
                                                        "Convert name",
                                                        "The converter element name",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
  node_class->set_state = set_state;
  node_class->add_port = add_port;
  node_class->remove_port = remove_port;
}

static void
pinos_gst_sink_init (PinosGstSink * sink)
{
  sink->priv = PINOS_GST_SINK_GET_PRIVATE (sink);
}

PinosNode *
pinos_gst_sink_new (PinosDaemon *daemon,
                    const gchar *name,
                    PinosProperties *properties,
                    GstElement  *element,
                    GstCaps     *caps,
                    GstElement  *mixer,
                    const gchar *convert_name)
{
  PinosNode *node;

  node = g_object_new (PINOS_TYPE_GST_SINK,
                       "daemon", daemon,
                       "name", name,
                       "properties", properties,
                       "element", element,
                       "possible-formats", caps,
                       "mixer", mixer,
                       "convert-name", convert_name,
                       NULL);

  return node;
}
