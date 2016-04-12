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

#include <pinos/server/daemon.h>
#include <pinos/server/client-source.h>

#define PINOS_CLIENT_SOURCE_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_CLIENT_SOURCE, PinosClientSourcePrivate))

struct _PinosClientSourcePrivate
{
  GstElement *pipeline;
  GstElement *src;
  GstElement *sink;
  guint id;

  GstCaps *format;
  GBytes *possible_formats;

  PinosSourceOutput *input;
};

G_DEFINE_TYPE (PinosClientSource, pinos_client_source, PINOS_TYPE_SOURCE);

enum
{
  PROP_0,
  PROP_POSSIBLE_FORMATS
};

static void
client_source_get_property (GObject    *_object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  PinosClientSource *source = PINOS_CLIENT_SOURCE (_object);
  PinosClientSourcePrivate *priv = source->priv;

  switch (prop_id) {
    case PROP_POSSIBLE_FORMATS:
      g_value_set_boxed (value, priv->possible_formats);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (source, prop_id, pspec);
      break;
  }
}

static void
client_source_set_property (GObject      *_object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  PinosClientSource *source = PINOS_CLIENT_SOURCE (_object);
  PinosClientSourcePrivate *priv = source->priv;

  switch (prop_id) {
    case PROP_POSSIBLE_FORMATS:
      if (priv->possible_formats)
        g_bytes_unref (priv->possible_formats);
      priv->possible_formats = g_value_dup_boxed (value);
      pinos_source_update_possible_formats (PINOS_SOURCE (source),
                                            priv->possible_formats);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (source, prop_id, pspec);
      break;
  }
}


static gboolean
bus_handler (GstBus     *bus,
             GstMessage *message,
             gpointer    user_data)
{
  PinosSource *source = user_data;
  PinosClientSourcePrivate *priv = PINOS_CLIENT_SOURCE (source)->priv;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:
    {
      GError *error;
      gchar *debug;

      gst_message_parse_error (message, &error, &debug);
      g_warning ("got error %s (%s)\n", error->message, debug);
      g_free (debug);

      pinos_source_report_error (source, error);
      gst_element_set_state (priv->pipeline, GST_STATE_NULL);
      break;
    }
    case GST_MESSAGE_ELEMENT:
    {
      if (gst_message_has_name (message, "PinosPayloaderFormatChange")) {
        const GstStructure *str = gst_message_get_structure (message);
        GstCaps *caps;
        GBytes *format;
        gchar *caps_str;

        gst_structure_get (str, "format", GST_TYPE_CAPS, &caps, NULL);
        gst_caps_replace (&priv->format, caps);
        caps_str = gst_caps_to_string (caps);

        format = g_bytes_new_take (caps_str, strlen (caps_str) + 1);
        g_object_set (priv->input, "possible-formats", format, "format", format, NULL);
        pinos_source_update_possible_formats (source, format);
        pinos_source_update_format (source, format);
        g_bytes_unref (format);
      }
      break;
    }
    default:
      break;
  }
  return TRUE;
}

static void
setup_pipeline (PinosClientSource *source)
{
  PinosClientSourcePrivate *priv = source->priv;
  GstBus *bus;

  priv->pipeline = gst_parse_launch ("socketsrc "
                                         "name=src "
                                         "caps=application/x-pinos "
                                         "send-messages=true ! "
                                     "pinospay ! "
                                     "multisocketsink "
                                         "buffers-max=2 "
                                         "buffers-soft-max=1 "
                                         "recover-policy=latest "
                                         "sync-method=latest "
                                         "name=sink "
                                         "sync=true "
                                         "enable-last-sample=false "
                                         "send-messages=true "
                                         "send-dispatched=true",
                                      NULL);
  priv->sink = gst_bin_get_by_name (GST_BIN (priv->pipeline), "sink");
  priv->src = gst_bin_get_by_name (GST_BIN (priv->pipeline), "src");

  bus = gst_pipeline_get_bus (GST_PIPELINE (priv->pipeline));
  priv->id = gst_bus_add_watch (bus, bus_handler, source);
  gst_object_unref (bus);

  g_debug ("client-source %p: setup pipeline", source);
}

static GstCaps *
collect_caps (PinosSource *source,
              GstCaps     *filter)
{
  PinosClientSourcePrivate *priv = PINOS_CLIENT_SOURCE (source)->priv;

  if (priv->format)
    return gst_caps_ref (priv->format);
  else
    return gst_caps_new_any ();
}

static gboolean
client_set_state (PinosSource      *source,
                  PinosSourceState  state)
{
  PinosClientSourcePrivate *priv = PINOS_CLIENT_SOURCE (source)->priv;

  switch (state) {
    case PINOS_SOURCE_STATE_SUSPENDED:
      gst_element_set_state (priv->pipeline, GST_STATE_NULL);
      break;

    case PINOS_SOURCE_STATE_INITIALIZING:
      gst_element_set_state (priv->pipeline, GST_STATE_READY);
      break;

    case PINOS_SOURCE_STATE_IDLE:
      gst_element_set_state (priv->pipeline, GST_STATE_PAUSED);
      break;

    case PINOS_SOURCE_STATE_RUNNING:
      gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
      break;

    case PINOS_SOURCE_STATE_ERROR:
      break;
  }
  pinos_source_update_state (source, state);

  return TRUE;
}

static GBytes *
client_get_formats (PinosSource *source,
                    GBytes      *filter,
                    GError     **error)
{
  GstCaps *caps, *cfilter;
  gchar *str;

  if (filter) {
    cfilter = gst_caps_from_string (g_bytes_get_data (filter, NULL));
    if (cfilter == NULL)
      goto invalid_filter;
  } else {
    cfilter = NULL;
  }

  caps = collect_caps (source, cfilter);
  if (caps == NULL)
    goto no_format;

  str = gst_caps_to_string (caps);

  gst_caps_unref (caps);
  if (cfilter)
    gst_caps_unref (cfilter);

  return g_bytes_new_take (str, strlen (str) + 1);

invalid_filter:
  {
    if (error)
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "Invalid filter received");
    return NULL;
  }
no_format:
  {
    if (error)
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "No compatible format found");
    if (cfilter)
      gst_caps_unref (cfilter);
    return NULL;
  }
}

static void
on_socket_notify (GObject    *gobject,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  PinosClientSource *source = user_data;
  PinosClientSourcePrivate *priv = source->priv;
  GSocket *socket;
  guint num_handles;

  g_object_get (gobject, "socket", &socket, NULL);

  g_debug ("client-source %p: output socket notify %p", source, socket);

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
  if (num_handles > 0 && socket) {
    GBytes *format;

    /* suggest what we provide */
    g_object_get (priv->input, "format", &format, NULL);
    g_object_set (gobject, "format", format, NULL);
    g_bytes_unref (format);
  }
}

static PinosSourceOutput *
client_create_source_output (PinosSource     *source,
                             const gchar     *client_path,
                             GBytes          *format_filter,
                             PinosProperties *props,
                             const gchar     *prefix,
                             GError          **error)
{
  PinosClientSourcePrivate *priv = PINOS_CLIENT_SOURCE (source)->priv;
  PinosSourceOutput *output;

  /* propose format of input */
  g_object_get (priv->input, "format", &format_filter, NULL);

  output = PINOS_SOURCE_CLASS (pinos_client_source_parent_class)
                ->create_source_output (source,
                                        client_path,
                                        format_filter,
                                        props,
                                        prefix,
                                        error);
  g_bytes_unref (format_filter);

  if (output == NULL)
    return NULL;

  g_debug ("client-source %p: create output %p", source, output);

  g_signal_connect (output, "notify::socket", (GCallback) on_socket_notify, source);

  return output;
}

static gboolean
client_release_source_output  (PinosSource       *source,
                               PinosSourceOutput *output)
{
  g_debug ("client-source %p: release output %p", source, output);
  return PINOS_SOURCE_CLASS (pinos_client_source_parent_class)->release_source_output (source, output);
}

static void
client_source_dispose (GObject * object)
{
  PinosClientSourcePrivate *priv = PINOS_CLIENT_SOURCE (object)->priv;

  g_debug ("client-source %p: dispose", object);

  g_source_remove (priv->id);
  gst_element_set_state (priv->pipeline, GST_STATE_NULL);

  G_OBJECT_CLASS (pinos_client_source_parent_class)->dispose (object);
}

static void
client_source_finalize (GObject * object)
{
  PinosClientSourcePrivate *priv = PINOS_CLIENT_SOURCE (object)->priv;

  g_debug ("client-source %p: finalize", object);

  g_clear_object (&priv->input);
  g_clear_object (&priv->sink);
  g_clear_object (&priv->src);
  g_clear_object (&priv->pipeline);

  if (priv->possible_formats)
    g_bytes_unref (priv->possible_formats);
  gst_caps_replace (&priv->format, NULL);

  G_OBJECT_CLASS (pinos_client_source_parent_class)->finalize (object);
}


static void
on_input_socket_notify (GObject    *gobject,
                        GParamSpec *pspec,
                        gpointer    user_data)
{
  PinosClientSource *source = user_data;
  PinosClientSourcePrivate *priv = source->priv;
  GSocket *socket;
  GBytes *requested_format;
  GstCaps *caps;

  g_object_get (gobject, "socket", &socket, NULL);
  g_debug ("client-source %p: input socket notify %p", source, socket);

  if (socket) {
    /* requested format is final format */
    g_object_get (gobject, "requested-format", &requested_format, NULL);
    g_assert (requested_format != NULL);
    g_object_set (gobject, "format", requested_format, NULL);

    /* and set as the current format */
    caps = gst_caps_from_string (g_bytes_get_data (requested_format, NULL));
    g_assert (caps != NULL);
    gst_caps_take (&priv->format, caps);
    g_bytes_unref (requested_format);
  } else {
    gst_caps_replace (&priv->format, NULL);
  }
  g_object_set (priv->src, "socket", socket, NULL);

  if (socket) {
    g_debug ("client-source %p: set pipeline to PLAYING", source);
    gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
    g_object_unref (socket);
  } else {
    g_debug ("client-source %p: set pipeline to READY", source);
    gst_element_set_state (priv->pipeline, GST_STATE_READY);
  }
}

static void
handle_remove_source_input (PinosSourceOutput *output,
                            gpointer           user_data)
{
  PinosClientSource *source = user_data;
  PinosClientSourcePrivate *priv = source->priv;

  g_debug ("client-source %p: remove source input %p", source, priv->input);
  g_clear_pointer (&priv->input, g_object_unref);
}

/**
 * pinos_client_source_get_source_input:
 * @source: a #PinosClientSource
 * @client_path: the client path
 * @format_filter: a #GBytes
 * @props: extra properties
 * @prefix: a path prefix
 * @error: a #GError or %NULL
 *
 * Create a new #PinosSourceOutput that can be used to send data to
 * the pinos server.
 *
 * Returns: a new #PinosSourceOutput.
 */
PinosSourceOutput *
pinos_client_source_get_source_input (PinosClientSource *source,
                                      const gchar       *client_path,
                                      GBytes            *format_filter,
                                      PinosProperties   *props,
                                      const gchar       *prefix,
                                      GError            **error)
{
  PinosClientSourcePrivate *priv;

  g_return_val_if_fail (PINOS_IS_CLIENT_SOURCE (source), NULL);
  priv = source->priv;

  if (priv->input == NULL) {
    GstCaps *caps = gst_caps_from_string (g_bytes_get_data (format_filter, NULL));

    gst_caps_take (&priv->format, caps);

    priv->input = PINOS_SOURCE_CLASS (pinos_client_source_parent_class)
                        ->create_source_output (PINOS_SOURCE (source),
                                                client_path,
                                                format_filter,
                                                props,
                                                prefix,
                                                error);
    if (priv->input == NULL)
      return NULL;

    g_signal_connect (priv->input,
                      "remove",
                      (GCallback) handle_remove_source_input,
                      source);

    g_debug ("client-source %p: get source input %p", source, priv->input);
    g_signal_connect (priv->input, "notify::socket", (GCallback) on_input_socket_notify, source);
  }
  return g_object_ref (priv->input);
}

static void
pinos_client_source_class_init (PinosClientSourceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PinosSourceClass *source_class = PINOS_SOURCE_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosClientSourcePrivate));

  gobject_class->dispose = client_source_dispose;
  gobject_class->finalize = client_source_finalize;

  gobject_class->get_property = client_source_get_property;
  gobject_class->set_property = client_source_set_property;

  g_object_class_install_property (gobject_class,
                                   PROP_POSSIBLE_FORMATS,
                                   g_param_spec_boxed ("possible-formats",
                                                       "Possible Format",
                                                       "The possible formats of the stream",
                                                       G_TYPE_BYTES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));

  source_class->get_formats = client_get_formats;
  source_class->set_state = client_set_state;
  source_class->create_source_output = client_create_source_output;
  source_class->release_source_output = client_release_source_output;
}

static void
pinos_client_source_init (PinosClientSource * source)
{
  source->priv = PINOS_CLIENT_SOURCE_GET_PRIVATE (source);

  g_debug ("client-source %p: new", source);
  setup_pipeline (source);
}

/**
 * pinos_client_source_new:
 * @daemon: the parent #PinosDaemon
 * @possible_formats: a #GBytes
 *
 * Make a new #PinosSource that can be used to receive data from a client.
 *
 * Returns: a new #PinosSource.
 */
PinosSource *
pinos_client_source_new (PinosDaemon *daemon,
                         GBytes      *possible_formats)
{
  return g_object_new (PINOS_TYPE_CLIENT_SOURCE,
                       "daemon", daemon,
                       "name", "client-source",
                       "possible-formats", possible_formats,
                       NULL);
}
