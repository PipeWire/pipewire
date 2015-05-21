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

#include "pv-gst-source.h"

#define PV_GST_SOURCE_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PV_TYPE_GST_SOURCE, PvGstSourcePrivate))

struct _PvGstSourcePrivate
{
  GstElement *pipeline;
  GstElement *element;
  GstElement *filter;
  GstElement *sink;

  GstCaps *possible_formats;
};

enum {
  PROP_0,
  PROP_ELEMENT,
};

G_DEFINE_TYPE (PvGstSource, pv_gst_source, PV_TYPE_SOURCE);

static gboolean
bus_handler (GstBus * bus, GstMessage * message, gpointer user_data)
{
  PvSource *source = user_data;
  PvGstSourcePrivate *priv = PV_GST_SOURCE (source)->priv;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:
    {
      GError *error;
      gchar *debug;

      gst_message_parse_error (message, &error, &debug);
      g_warning ("got error %s (%s)\n", error->message, debug);
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
setup_pipeline (PvGstSource *source)
{
  PvGstSourcePrivate *priv = source->priv;
  GstBus *bus;
  GstElement *elem;

  priv->pipeline = gst_pipeline_new (NULL);

  gst_bin_add (GST_BIN (priv->pipeline), priv->element);

  priv->filter = gst_element_factory_make ("capsfilter", NULL);
  gst_bin_add (GST_BIN (priv->pipeline), priv->filter);
  gst_element_link (priv->element, priv->filter);

  elem = gst_element_factory_make ("pvfdpay", NULL);
  gst_bin_add (GST_BIN (priv->pipeline), elem);
  gst_element_link (priv->filter, elem);

  priv->sink = gst_element_factory_make ("multisocketsink", NULL);
  g_object_set (priv->sink, "buffers-max", 2,
                            "buffers-soft-max", 1,
                            "recover-policy", 1, /* latest */
                            "sync-method", 0, /* latest */
                            "sync", TRUE,
                            "enable-last-sample", FALSE,
                            NULL);
  gst_bin_add (GST_BIN (priv->pipeline), priv->sink);
  gst_element_link (elem, priv->sink);

  bus = gst_pipeline_get_bus (GST_PIPELINE (priv->pipeline));
  gst_bus_add_watch (bus, bus_handler, source);
  gst_object_unref (bus);

  gst_element_set_state (priv->pipeline, GST_STATE_READY);
}

static void
destroy_pipeline (PvGstSource *source)
{
  PvGstSourcePrivate *priv = source->priv;

  if (priv->pipeline) {
    gst_element_set_state (priv->pipeline, GST_STATE_NULL);
    gst_object_unref (priv->pipeline);
    priv->pipeline = NULL;
  }
}

static GstCaps *
collect_caps (PvSource * source, GstCaps *filter)
{
  PvGstSourcePrivate *priv = PV_GST_SOURCE (source)->priv;
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
set_state (PvSource *source, PvSourceState state)
{
  PvGstSourcePrivate *priv = PV_GST_SOURCE (source)->priv;

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
get_formats (PvSource *source, GBytes *filter)
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
  PvGstSource *source = user_data;
  PvGstSourcePrivate *priv = source->priv;
  GSocket *socket;
  guint num_handles;
  GstCaps *caps;
  GBytes *requested_format, *format;

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
    g_object_set (gobject, "format", format, NULL);
    g_bytes_unref (format);

    gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
  }
}

static PvSourceOutput *
create_source_output (PvSource    *source,
                      const gchar *client_path,
                      GBytes      *format_filter,
                      const gchar *prefix,
                      GError      **error)
{
  PvSourceOutput *output;
  GstCaps *caps, *filtered;
  gchar *str;

  str = (gchar *) g_bytes_get_data (format_filter, NULL);
  caps = gst_caps_from_string (str);
  if (caps == NULL)
    goto invalid_caps;

  filtered = collect_caps (source, caps);
  if (filtered == NULL || gst_caps_is_empty (filtered))
    goto no_format;

  str = gst_caps_to_string (filtered);
  format_filter = g_bytes_new_take (str, strlen (str) + 1);

  output = PV_SOURCE_CLASS (pv_gst_source_parent_class)
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
release_source_output  (PvSource *source, PvSourceOutput *output)
{
  return PV_SOURCE_CLASS (pv_gst_source_parent_class)->release_source_output (source, output);
}

static void
get_property (GObject    *object,
              guint       prop_id,
              GValue     *value,
              GParamSpec *pspec)
{
  PvGstSource *source = PV_GST_SOURCE (object);
  PvGstSourcePrivate *priv = source->priv;

  switch (prop_id) {
    case PROP_ELEMENT:
      g_value_set_object (value, priv->element);
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
  PvGstSource *source = PV_GST_SOURCE (object);
  PvGstSourcePrivate *priv = source->priv;

  switch (prop_id) {
    case PROP_ELEMENT:
      priv->element = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
source_constructed (GObject * object)
{
  PvGstSource *source = PV_GST_SOURCE (object);

  setup_pipeline (source);

  G_OBJECT_CLASS (pv_gst_source_parent_class)->constructed (object);
}

static void
source_finalize (GObject * object)
{
  PvGstSource *source = PV_GST_SOURCE (object);

  destroy_pipeline (source);

  G_OBJECT_CLASS (pv_gst_source_parent_class)->finalize (object);
}

static void
pv_gst_source_class_init (PvGstSourceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PvSourceClass *source_class = PV_SOURCE_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PvGstSourcePrivate));

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


  source_class->get_formats = get_formats;
  source_class->set_state = set_state;
  source_class->create_source_output = create_source_output;
  source_class->release_source_output = release_source_output;
}

static void
pv_gst_source_init (PvGstSource * source)
{
  source->priv = PV_GST_SOURCE_GET_PRIVATE (source);
}

PvSource *
pv_gst_source_new (PvDaemon *daemon, const gchar *name, GstElement *element)
{
  return g_object_new (PV_TYPE_GST_SOURCE, "daemon", daemon, "name", name, "element", element, NULL);
}
