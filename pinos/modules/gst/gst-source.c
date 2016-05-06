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

#include "gst-source.h"

#define PINOS_GST_SOURCE_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_GST_SOURCE, PinosGstSourcePrivate))

struct _PinosGstSourcePrivate
{
  GstElement *pipeline;
  GstElement *element;
  GstElement *filter;
  GstElement *sink;

  PinosPort *output;
  GstCaps *possible_formats;

  GstNetTimeProvider *provider;

  PinosProperties *props;

  gint n_channels;
};

enum {
  PROP_0,
  PROP_ELEMENT,
  PROP_POSSIBLE_FORMATS
};

G_DEFINE_TYPE (PinosGstSource, pinos_gst_source, PINOS_TYPE_NODE);

static gboolean
bus_handler (GstBus     *bus,
             GstMessage *message,
             gpointer    user_data)
{
  PinosNode *node = user_data;
  PinosGstSourcePrivate *priv = PINOS_GST_SOURCE (node)->priv;

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
setup_pipeline (PinosGstSource *source, GError **error)
{
  PinosGstSourcePrivate *priv = source->priv;
  GstBus *bus;

  g_debug ("gst-source %p: setup pipeline", source);
  priv->pipeline = gst_pipeline_new (NULL);
  gst_pipeline_set_latency (GST_PIPELINE_CAST (priv->pipeline), 0);

  gst_bin_add (GST_BIN (priv->pipeline), priv->element);

  priv->filter = gst_element_factory_make ("capsfilter", NULL);
  gst_bin_add (GST_BIN (priv->pipeline), priv->filter);
  gst_element_link (priv->element, priv->filter);

  priv->sink = gst_element_factory_make ("pinossocketsink", NULL);
  g_object_set (priv->sink, "sync", TRUE,
                            "enable-last-sample", FALSE,
                            "qos", FALSE,
                            NULL);

  gst_bin_add (GST_BIN (priv->pipeline), priv->sink);
  gst_element_link (priv->filter, priv->sink);

  bus = gst_pipeline_get_bus (GST_PIPELINE (priv->pipeline));
  gst_bus_add_watch (bus, bus_handler, source);
  gst_object_unref (bus);

  return TRUE;
}

static gboolean
start_pipeline (PinosGstSource *source, GError **error)
{
  PinosGstSourcePrivate *priv = source->priv;
  GstCaps *caps;
  GstQuery *query;
  GstStateChangeReturn ret;
  gchar *str;

  g_debug ("gst-source %p: starting pipeline", source);

  ret = gst_element_set_state (priv->pipeline, GST_STATE_READY);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto ready_failed;

  query = gst_query_new_caps (NULL);
  if (gst_element_query (priv->element, query)) {
    gst_query_parse_caps_result (query, &caps);
    gst_caps_replace (&priv->possible_formats, caps);
    str = gst_caps_to_string (caps);
    g_debug ("gst-source %p: updated possible formats %s", source, str);
    g_free (str);
  }
  gst_query_unref (query);

  return TRUE;

  /* ERRORS */
ready_failed:
  {
    GST_ERROR_OBJECT (source, "failed state change to READY");
    gst_element_set_state (priv->pipeline, GST_STATE_NULL);
    if (error)
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Failed to start pipeline");
    return FALSE;
  }
}

static void
stop_pipeline (PinosGstSource *source)
{
  PinosGstSourcePrivate *priv = source->priv;

  g_debug ("gst-source %p: stopping pipeline", source);
  gst_element_set_state (priv->pipeline, GST_STATE_NULL);
  g_clear_object (&priv->provider);
}

static void
destroy_pipeline (PinosGstSource *source)
{
  PinosGstSourcePrivate *priv = source->priv;

  g_debug ("gst-source %p: destroy pipeline", source);
  stop_pipeline (source);
  g_clear_object (&priv->pipeline);
}


static gboolean
set_state (PinosNode      *node,
           PinosNodeState  state)
{
  PinosGstSourcePrivate *priv = PINOS_GST_SOURCE (node)->priv;

  g_debug ("gst-source %p: set state %s", node, pinos_node_state_as_string (state));

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
    {
      GstQuery *query;
      GstClock *clock;
      gchar *address;
      gint port;
      GstClockTime base_time;
      gboolean live;
      GstClockTime min_latency, max_latency;

      gst_element_set_state (priv->pipeline, GST_STATE_PAUSED);
      gst_element_get_state (priv->pipeline, NULL, NULL, -1);
      gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
      gst_element_get_state (priv->pipeline, NULL, NULL, -1);

      clock = gst_pipeline_get_clock (GST_PIPELINE (priv->pipeline));
      base_time = gst_clock_get_time (clock);

      if (priv->provider)
        g_object_unref (priv->provider);
      priv->provider = gst_net_time_provider_new (clock, NULL, 0);

      g_object_get (priv->provider, "address", &address, "port", &port, NULL);

      pinos_properties_set (priv->props, "pinos.clock.type", "gst.net.time.provider");
      pinos_properties_set (priv->props, "pinos.clock.source", GST_OBJECT_NAME (clock));
      pinos_properties_set (priv->props, "pinos.clock.address", address);
      pinos_properties_setf (priv->props, "pinos.clock.port", "%d", port);
      pinos_properties_setf (priv->props, "pinos.clock.base-time", "%"G_GUINT64_FORMAT, base_time);

      g_free (address);
      gst_object_unref (clock);

      query = gst_query_new_latency ();
      if (gst_element_query (GST_ELEMENT_CAST (priv->pipeline), query)) {
        gst_query_parse_latency (query, &live, &min_latency, &max_latency);
      } else {
        live = FALSE;
        min_latency = 0;
        max_latency = -1;
      }
      gst_query_unref (query);

      GST_DEBUG_OBJECT (priv->pipeline,
          "got min latency %" GST_TIME_FORMAT ", max latency %"
          GST_TIME_FORMAT ", live %d", GST_TIME_ARGS (min_latency),
          GST_TIME_ARGS (max_latency), live);

      pinos_properties_setf (priv->props, "pinos.latency.is-live", "%d", live);
      pinos_properties_setf (priv->props, "pinos.latency.min", "%"G_GUINT64_FORMAT, min_latency);
      pinos_properties_setf (priv->props, "pinos.latency.max", "%"G_GUINT64_FORMAT, max_latency);
      break;
    }
    case PINOS_NODE_STATE_ERROR:
      break;
  }
  pinos_node_update_state (node, state);
  return TRUE;
}

static void
on_socket_notify (GObject    *gobject,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  PinosGstSource *source = user_data;
  PinosGstSourcePrivate *priv = source->priv;
  GSocket *socket;
  guint num_handles;
  GstCaps *caps;
  GBytes *requested_format, *format = NULL;
  gchar *str;
  gpointer state = NULL;
  const gchar *key, *val;
  PinosProperties *props;

  g_object_get (gobject, "socket", &socket, NULL);
  GST_DEBUG ("got socket %p", socket);

  if (socket == NULL) {
    GSocket *prev_socket = g_object_steal_data (gobject, "last-socket");
    if (prev_socket) {
      g_signal_emit_by_name (priv->sink, "remove", prev_socket);
      g_object_unref (prev_socket);
    }
  } else {
    g_signal_emit_by_name (priv->sink, "add", socket);
    g_object_set_data_full (gobject, "last-socket", socket, g_object_unref);
  }

  g_object_get (priv->sink, "num-handles", &num_handles, NULL);
  if (num_handles == 0) {
    pinos_node_report_idle (PINOS_NODE (source));
    g_object_set (priv->filter, "caps", NULL, NULL);

    str = gst_caps_to_string (priv->possible_formats);
    format = g_bytes_new_take (str, strlen (str) + 1);
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
      /* we already have a client, format is whatever is configured already */
      g_bytes_unref (requested_format);

      g_object_get (priv->filter, "caps", &caps, NULL);
      str = gst_caps_to_string (caps);
      format = g_bytes_new_take (str, strlen (str) + 1);
      gst_caps_unref (caps);
    }
    /* this is what we use as the final format for the output */
    g_object_set (gobject, "format", format, NULL);
    pinos_node_report_busy (PINOS_NODE (source));
  }
  if (format) {
    g_object_set (priv->output, "possible-formats", format, NULL);
    g_bytes_unref (format);
  }

  g_object_get (gobject, "properties", &props, NULL);
  while ((key = pinos_properties_iterate (priv->props, &state))) {
    val = pinos_properties_get (priv->props, key);
    pinos_properties_set (props, key, val);
  }
  g_object_set (gobject, "properties", props, NULL);
  pinos_properties_free (props);
}

static void
on_channel_added (PinosPort *port, PinosChannel *channel, PinosGstSource *source)
{
  PinosGstSourcePrivate *priv = source->priv;
  GError *error = NULL;

  if (priv->n_channels == 0) {
    if (!start_pipeline (source, &error))
      return;
  }

  g_signal_connect (channel, "notify::socket", (GCallback) on_socket_notify, source);
  priv->n_channels++;
}

static void
on_channel_removed (PinosPort *port, PinosChannel *channel, PinosGstSource *source)
{
  PinosGstSourcePrivate *priv = source->priv;

  priv->n_channels--;
  if (priv->n_channels == 0)
    stop_pipeline (source);
}

static void
get_property (GObject    *object,
              guint       prop_id,
              GValue     *value,
              GParamSpec *pspec)
{
  PinosGstSource *source = PINOS_GST_SOURCE (object);
  PinosGstSourcePrivate *priv = source->priv;

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
  PinosGstSource *source = PINOS_GST_SOURCE (object);
  PinosGstSourcePrivate *priv = source->priv;

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
source_constructed (GObject * object)
{
  PinosGstSource *source = PINOS_GST_SOURCE (object);
  PinosGstSourcePrivate *priv = source->priv;
  gchar *str;
  GBytes *format;

  G_OBJECT_CLASS (pinos_gst_source_parent_class)->constructed (object);

  str = gst_caps_to_string (priv->possible_formats);
  format = g_bytes_new_take (str, strlen (str) + 1);

  priv->output = pinos_port_new (PINOS_NODE (source),
                                 PINOS_DIRECTION_OUTPUT,
                                 "output",
                                 format,
                                 NULL);
  g_bytes_unref (format);

  g_signal_connect (priv->output, "channel-added", (GCallback) on_channel_added, source);
  g_signal_connect (priv->output, "channel-removed", (GCallback) on_channel_removed, source);

  setup_pipeline (source, NULL);
}

static void
source_finalize (GObject * object)
{
  PinosGstSource *source = PINOS_GST_SOURCE (object);
  PinosGstSourcePrivate *priv = source->priv;

  destroy_pipeline (source);
  g_clear_pointer (&priv->possible_formats, gst_caps_unref);
  pinos_properties_free (priv->props);

  G_OBJECT_CLASS (pinos_gst_source_parent_class)->finalize (object);
}

static void
pinos_gst_source_class_init (PinosGstSourceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PinosNodeClass *node_class = PINOS_NODE_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosGstSourcePrivate));

  gobject_class->constructed = source_constructed;
  gobject_class->finalize = source_finalize;
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
pinos_gst_source_init (PinosGstSource * source)
{
  PinosGstSourcePrivate *priv;

  priv = source->priv = PINOS_GST_SOURCE_GET_PRIVATE (source);
  priv->props = pinos_properties_new (NULL, NULL);
}

PinosNode *
pinos_gst_source_new (PinosDaemon *daemon,
                      const gchar *name,
                      PinosProperties *properties,
                      GstElement  *element,
                      GstCaps     *caps)
{
  PinosNode *node;

  node = g_object_new (PINOS_TYPE_GST_SOURCE,
                       "daemon", daemon,
                       "name", name,
                       "properties", properties,
                       "element", element,
                       "possible-formats", caps,
                       NULL);

  return node;
}
