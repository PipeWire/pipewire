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

G_DEFINE_TYPE (PinosGstSink, pinos_gst_sink, PINOS_TYPE_SINK);

static gboolean
bus_handler (GstBus     *bus,
             GstMessage *message,
             gpointer    user_data)
{
  PinosSink *sink = user_data;
  PinosGstSinkPrivate *priv = PINOS_GST_SINK (sink)->priv;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:
    {
      GError *error;
      gchar *debug;

      gst_message_parse_error (message, &error, &debug);
      g_warning ("got error %s (%s)\n", error->message, debug);
      g_free (debug);

      pinos_sink_report_error (sink, error);
      gst_element_set_state (priv->pipeline, GST_STATE_NULL);
      break;
    }
    case GST_MESSAGE_NEW_CLOCK:
    {
      GstClock *clock;
      PinosProperties *props;

      gst_message_parse_new_clock (message, &clock);
      GST_INFO ("got new clock %s", GST_OBJECT_NAME (clock));

      g_object_get (sink, "properties", &props, NULL);
      pinos_properties_set (props, "gst.pipeline.clock", GST_OBJECT_NAME (clock));
      g_object_set (sink, "properties", props, NULL);
      pinos_properties_free (props);
      break;
    }
    case GST_MESSAGE_CLOCK_LOST:
    {
      GstClock *clock;
      PinosProperties *props;

      gst_message_parse_new_clock (message, &clock);
      GST_INFO ("clock lost %s", GST_OBJECT_NAME (clock));

      g_object_get (sink, "properties", &props, NULL);
      pinos_properties_remove (props, "gst.pipeline.clock");
      g_object_set (sink, "properties", props, NULL);
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
  GstCaps *caps;

  g_debug ("gst-sink %p: setup pipeline", sink);
  priv->pipeline = gst_pipeline_new (NULL);

  priv->src = gst_element_factory_make ("socketsrc", NULL);
  caps = gst_caps_new_empty_simple ("application/x-pinos");
  g_object_set (priv->src, "send-messages", TRUE,
                           "caps", caps, NULL);
  gst_caps_unref (caps);
  gst_bin_add (GST_BIN (priv->pipeline), priv->src);

  priv->depay = gst_element_factory_make ("pinosdepay", NULL);
  gst_bin_add (GST_BIN (priv->pipeline), priv->depay);
  gst_element_link (priv->src, priv->depay);

  gst_bin_add (GST_BIN (priv->pipeline), priv->element);
  gst_element_link (priv->depay, priv->element);

  bus = gst_pipeline_get_bus (GST_PIPELINE (priv->pipeline));
  gst_bus_add_watch (bus, bus_handler, sink);
  gst_object_unref (bus);

  return TRUE;
}

static gboolean
start_pipeline (PinosGstSink *sink, GError **error)
{
  PinosGstSinkPrivate *priv = sink->priv;
  GstCaps *caps;
  GstQuery *query;
  GstStateChangeReturn ret;
  gchar *str;

  g_debug ("gst-sink %p: starting pipeline", sink);

  ret = gst_element_set_state (priv->pipeline, GST_STATE_READY);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto ready_failed;

  query = gst_query_new_caps (NULL);
  if (gst_element_query (priv->element, query)) {
    gst_query_parse_caps_result (query, &caps);
    gst_caps_replace (&priv->possible_formats, caps);
    str = gst_caps_to_string (caps);
    g_debug ("gst-sink %p: updated possible formats %s", sink, str);
    g_free (str);
  }
  gst_query_unref (query);

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
set_state (PinosSink      *sink,
           PinosSinkState  state)
{
  PinosGstSinkPrivate *priv = PINOS_GST_SINK (sink)->priv;

  g_debug ("gst-sink %p: set state %d", sink, state);

  switch (state) {
    case PINOS_SINK_STATE_SUSPENDED:
      gst_element_set_state (priv->pipeline, GST_STATE_NULL);
      break;

    case PINOS_SINK_STATE_INITIALIZING:
      gst_element_set_state (priv->pipeline, GST_STATE_READY);
      break;

    case PINOS_SINK_STATE_IDLE:
      gst_element_set_state (priv->pipeline, GST_STATE_PAUSED);
      break;

    case PINOS_SINK_STATE_RUNNING:
      gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
      break;

    case PINOS_SINK_STATE_ERROR:
      break;
  }
  pinos_sink_update_state (sink, state);
  return TRUE;
}

static GBytes *
get_formats (PinosSink   *sink,
             GBytes      *filter,
             GError     **error)
{
  PinosGstSinkPrivate *priv = PINOS_GST_SINK (sink)->priv;
  GstCaps *caps, *cfilter;
  gchar *str;

  if (filter) {
    cfilter = gst_caps_from_string (g_bytes_get_data (filter, NULL));
    if (cfilter == NULL)
      goto invalid_filter;

    caps = gst_caps_intersect (priv->possible_formats, cfilter);
    gst_caps_unref (cfilter);

    if (caps == NULL)
      goto no_formats;

  } else {
    caps = gst_caps_ref (priv->possible_formats);
  }
  g_object_get (priv->depay, "caps", &cfilter, NULL);
  if (cfilter != NULL) {
    GstCaps *t = caps;

    caps = gst_caps_intersect (t, cfilter);
    gst_caps_unref (cfilter);
    gst_caps_unref (t);
  }
  if (gst_caps_is_empty (caps)) {
    gst_caps_unref (caps);
    goto no_formats;
  }

  str = gst_caps_to_string (caps);
  gst_caps_unref (caps);

  return g_bytes_new_take (str, strlen (str) + 1);

invalid_filter:
  {
    if (error)
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "Invalid filter received");
    return NULL;
  }
no_formats:
  {
    if (error)
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "No compatible format found");
    return NULL;
  }
}

static void
on_socket_notify (GObject    *gobject,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  PinosGstSink *sink = user_data;
  PinosGstSinkPrivate *priv = sink->priv;
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
    g_object_set (priv->src, "socket", NULL, NULL);
    num_handles = 0;
  } else {
    g_object_set (priv->src, "socket", socket, NULL);
    num_handles = 1;
  }

  if (num_handles == 0) {
    pinos_sink_report_idle (PINOS_SINK (sink));
    g_object_set (priv->depay, "caps", NULL, NULL);

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
      g_object_set (priv->depay, "caps", caps, NULL);
      gst_caps_unref (caps);
    } else {
      /* we already have a client, format is whatever is configured already */
      g_bytes_unref (requested_format);

      g_object_get (priv->depay, "caps", &caps, NULL);
      str = gst_caps_to_string (caps);
      format = g_bytes_new_take (str, strlen (str) + 1);
      gst_caps_unref (caps);
    }
    /* this is what we use as the final format for the output */
    g_object_set (gobject, "format", format, NULL);
    pinos_sink_report_busy (PINOS_SINK (sink));
    g_object_unref (socket);
  }
  if (format) {
    pinos_sink_update_possible_formats (PINOS_SINK (sink), format);
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

static PinosChannel *
create_channel (PinosSink       *sink,
                const gchar     *client_path,
                GBytes          *format_filter,
                PinosProperties *props,
                const gchar     *prefix,
                GError          **error)
{
  PinosGstSink *s = PINOS_GST_SINK (sink);
  PinosGstSinkPrivate *priv = s->priv;
  PinosChannel *channel;
  gpointer state = NULL;
  const gchar *key, *val;

  if (priv->n_channels == 0) {
    if (!start_pipeline (s, error))
      return NULL;
  }

  while ((key = pinos_properties_iterate (priv->props, &state))) {
    val = pinos_properties_get (priv->props, key);
    pinos_properties_set (props, key, val);
  }

  channel = PINOS_SINK_CLASS (pinos_gst_sink_parent_class)
                ->create_channel (sink,
                                  client_path,
                                  format_filter,
                                  props,
                                  prefix,
                                  error);
  if (channel == NULL)
    goto no_channel;

  g_signal_connect (channel,
                    "notify::socket",
                    (GCallback) on_socket_notify,
                    sink);

  priv->n_channels++;

  return channel;

  /* ERRORS */
no_channel:
  {
    if (priv->n_channels == 0)
       stop_pipeline (s);
    return NULL;
  }
}

static gboolean
release_channel (PinosSink      *sink,
                 PinosChannel   *channel)
{
  return PINOS_SINK_CLASS (pinos_gst_sink_parent_class)
                ->release_channel (sink, channel);
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
sink_constructed (GObject * object)
{
  PinosGstSink *sink = PINOS_GST_SINK (object);

  setup_pipeline (sink, NULL);

  G_OBJECT_CLASS (pinos_gst_sink_parent_class)->constructed (object);
}

static void
sink_finalize (GObject * object)
{
  PinosGstSink *sink = PINOS_GST_SINK (object);
  PinosGstSinkPrivate *priv = sink->priv;

  destroy_pipeline (sink);
  g_clear_pointer (&priv->possible_formats, gst_caps_unref);
  pinos_properties_free (priv->props);

  G_OBJECT_CLASS (pinos_gst_sink_parent_class)->finalize (object);
}

static void
pinos_gst_sink_class_init (PinosGstSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PinosSinkClass *sink_class = PINOS_SINK_CLASS (klass);

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

  sink_class->get_formats = get_formats;
  sink_class->set_state = set_state;
  sink_class->create_channel = create_channel;
  sink_class->release_channel = release_channel;
}

static void
pinos_gst_sink_init (PinosGstSink * sink)
{
  PinosGstSinkPrivate *priv;

  priv = sink->priv = PINOS_GST_SINK_GET_PRIVATE (sink);
  priv->props = pinos_properties_new (NULL, NULL);
}

PinosSink *
pinos_gst_sink_new (PinosNode   *node,
                    const gchar *name,
                    PinosProperties *properties,
                    GstElement  *element,
                    GstCaps     *caps)
{
  PinosSink *sink;

  sink = g_object_new (PINOS_TYPE_GST_SINK,
                       "node", node,
                       "name", name,
                       "properties", properties,
                       "element", element,
                       "possible-formats", caps,
                       NULL);

  return sink;
}
