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
#include <pinos/server/upload-node.h>

#define PINOS_UPLOAD_NODE_GET_PRIVATE(obj)  \
     (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_UPLOAD_NODE, PinosUploadNodePrivate))

struct _PinosUploadNodePrivate
{
  GstElement *pipeline;
  GstElement *src;
  GstElement *sink;
  guint id;

  GstCaps *format;
  GBytes *possible_formats;

  PinosPort *input, *output;

  PinosChannel *channel;
};

G_DEFINE_TYPE (PinosUploadNode, pinos_upload_node, PINOS_TYPE_NODE);

enum
{
  PROP_0,
  PROP_POSSIBLE_FORMATS
};

static void
upload_node_get_property (GObject    *_object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  PinosUploadNode *node = PINOS_UPLOAD_NODE (_object);
  PinosUploadNodePrivate *priv = node->priv;

  switch (prop_id) {
    case PROP_POSSIBLE_FORMATS:
      g_value_set_boxed (value, priv->possible_formats);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (node, prop_id, pspec);
      break;
  }
}

static void
upload_node_set_property (GObject      *_object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  PinosUploadNode *node = PINOS_UPLOAD_NODE (_object);
  PinosUploadNodePrivate *priv = node->priv;

  switch (prop_id) {
    case PROP_POSSIBLE_FORMATS:
      if (priv->possible_formats)
        g_bytes_unref (priv->possible_formats);
      priv->possible_formats = g_value_dup_boxed (value);
      if (priv->output)
        g_object_set (priv->output, "possible-formats", priv->possible_formats, NULL);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (node, prop_id, pspec);
      break;
  }
}

static void
update_channel_format (PinosChannel *channel, GBytes *format)
{
  g_object_set (channel, "format", format, NULL);
}

static gboolean
bus_handler (GstBus     *bus,
             GstMessage *message,
             gpointer    user_data)
{
  PinosNode *node = user_data;
  PinosUploadNodePrivate *priv = PINOS_UPLOAD_NODE (node)->priv;

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
        g_list_foreach (pinos_port_get_channels (priv->output), (GFunc) update_channel_format, format);
        g_list_foreach (pinos_port_get_channels (priv->input), (GFunc) update_channel_format, format);
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
setup_pipeline (PinosUploadNode *node)
{
  PinosUploadNodePrivate *priv = node->priv;
  GstBus *bus;

  g_debug ("upload-node %p: setup pipeline", node);
  priv->pipeline = gst_parse_launch ("socketsrc "
                                         "name=src "
                                         "caps=application/x-pinos "
                                         "send-messages=true ! "
                                     "pinossocketsink "
                                         "name=sink "
                                         "enable-last-sample=false ",
                                      NULL);
  priv->sink = gst_bin_get_by_name (GST_BIN (priv->pipeline), "sink");
  priv->src = gst_bin_get_by_name (GST_BIN (priv->pipeline), "src");

  bus = gst_pipeline_get_bus (GST_PIPELINE (priv->pipeline));
  priv->id = gst_bus_add_watch (bus, bus_handler, node);
  gst_object_unref (bus);

}

static gboolean
node_set_state (PinosNode      *node,
                PinosNodeState  state)
{
  PinosUploadNodePrivate *priv = PINOS_UPLOAD_NODE (node)->priv;

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
on_socket_notify (GObject    *gobject,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  PinosUploadNode *node = user_data;
  PinosUploadNodePrivate *priv = node->priv;
  GSocket *socket;
  guint num_handles;

  g_object_get (gobject, "socket", &socket, NULL);

  g_debug ("upload-node %p: output socket notify %p", node, socket);

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
    g_object_get (priv->channel, "format", &format, NULL);
    g_object_set (gobject, "format", format, NULL);
    g_bytes_unref (format);
  }
}

static void
on_channel_added (PinosPort *port, PinosChannel *channel, PinosNode *node)
{
  g_signal_connect (channel, "notify::socket", (GCallback) on_socket_notify, node);

  g_debug ("upload-node %p: create channel %p", node, channel);
}

static void
on_channel_removed (PinosPort *port, PinosChannel *channel, PinosNode *node)
{
  g_debug ("upload-node %p: release channel %p", node, channel);
}

static void
upload_node_dispose (GObject * object)
{
  PinosUploadNodePrivate *priv = PINOS_UPLOAD_NODE (object)->priv;

  g_debug ("upload-node %p: dispose", object);

  g_source_remove (priv->id);
  gst_element_set_state (priv->pipeline, GST_STATE_NULL);

  G_OBJECT_CLASS (pinos_upload_node_parent_class)->dispose (object);
}

static void
upload_node_finalize (GObject * object)
{
  PinosUploadNodePrivate *priv = PINOS_UPLOAD_NODE (object)->priv;

  g_debug ("upload-node %p: finalize", object);

  g_clear_object (&priv->channel);
  g_clear_object (&priv->sink);
  g_clear_object (&priv->src);
  g_clear_object (&priv->pipeline);

  if (priv->possible_formats)
    g_bytes_unref (priv->possible_formats);
  gst_caps_replace (&priv->format, NULL);

  G_OBJECT_CLASS (pinos_upload_node_parent_class)->finalize (object);
}


static void
on_input_socket_notify (GObject    *gobject,
                        GParamSpec *pspec,
                        gpointer    user_data)
{
  PinosUploadNode *node = user_data;
  PinosUploadNodePrivate *priv = node->priv;
  GSocket *socket;
  GBytes *requested_format;
  GstCaps *caps;

  g_object_get (gobject, "socket", &socket, NULL);
  g_debug ("upload-node %p: input socket notify %p", node, socket);

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
    g_debug ("upload-node %p: set pipeline to PLAYING", node);
    gst_element_set_state (priv->pipeline, GST_STATE_PLAYING);
    g_object_unref (socket);
  } else {
    g_debug ("upload-node %p: set pipeline to READY", node);
    gst_element_set_state (priv->pipeline, GST_STATE_READY);
  }
}

static void
handle_remove_channel (PinosChannel *channel,
                       gpointer      user_data)
{
  PinosUploadNode *node = user_data;
  PinosUploadNodePrivate *priv = node->priv;

  g_debug ("upload-node %p: remove channel %p", node, priv->channel);
  g_clear_pointer (&priv->channel, g_object_unref);
}

/**
 * pinos_upload_node_get_channel:
 * @node: a #PinosUploadNode
 * @client_path: the client path
 * @format_filter: a #GBytes
 * @props: extra properties
 * @prefix: a path prefix
 * @error: a #GError or %NULL
 *
 * Create a new #PinosChannel that can be used to send data to
 * the pinos server.
 *
 * Returns: a new #PinosChannel.
 */
PinosChannel *
pinos_upload_node_get_channel (PinosUploadNode   *node,
                               const gchar       *client_path,
                               GBytes            *format_filter,
                               PinosProperties   *props,
                               GError            **error)
{
  PinosUploadNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_UPLOAD_NODE (node), NULL);
  priv = node->priv;

  if (priv->channel == NULL) {
    GstCaps *caps = gst_caps_from_string (g_bytes_get_data (format_filter, NULL));

    gst_caps_take (&priv->format, caps);

    priv->channel = pinos_port_create_channel (priv->input,
                                               client_path,
                                               format_filter,
                                               props,
                                               error);
    if (priv->channel == NULL)
      return NULL;

    g_signal_connect (priv->channel,
                      "remove",
                      (GCallback) handle_remove_channel,
                      node);

    g_debug ("upload-node %p: get input %p", node, priv->channel);
    g_signal_connect (priv->channel, "notify::socket", (GCallback) on_input_socket_notify, node);
  }
  return g_object_ref (priv->channel);
}

static void
upload_node_constructed (GObject * object)
{
  PinosNode *node = PINOS_NODE (object);
  PinosUploadNode *upload = PINOS_UPLOAD_NODE (object);
  PinosUploadNodePrivate *priv = upload->priv;

  G_OBJECT_CLASS (pinos_upload_node_parent_class)->constructed (object);

  g_debug ("upload-node %p: constructed", upload);
  priv->input = pinos_port_new (pinos_node_get_daemon (node),
                                pinos_node_get_object_path (node),
                                PINOS_DIRECTION_INPUT,
                                "input",
                                 priv->possible_formats,
                                NULL);
  priv->output = pinos_port_new (pinos_node_get_daemon (node),
                                 pinos_node_get_object_path (node),
                                 PINOS_DIRECTION_OUTPUT,
                                 "output",
                                 priv->possible_formats,
                                 NULL);
  g_signal_connect (priv->output, "channel-added", (GCallback) on_channel_added, upload);
  g_signal_connect (priv->output, "channel-removed", (GCallback) on_channel_removed, upload);

  pinos_node_add_port (node, priv->input);
  pinos_node_add_port (node, priv->output);

  setup_pipeline (upload);
}

static void
pinos_upload_node_class_init (PinosUploadNodeClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PinosNodeClass *node_class = PINOS_NODE_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosUploadNodePrivate));

  gobject_class->constructed = upload_node_constructed;
  gobject_class->dispose = upload_node_dispose;
  gobject_class->finalize = upload_node_finalize;

  gobject_class->get_property = upload_node_get_property;
  gobject_class->set_property = upload_node_set_property;

  g_object_class_install_property (gobject_class,
                                   PROP_POSSIBLE_FORMATS,
                                   g_param_spec_boxed ("possible-formats",
                                                       "Possible Format",
                                                       "The possible formats of the stream",
                                                       G_TYPE_BYTES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));

  node_class->set_state = node_set_state;
}

static void
pinos_upload_node_init (PinosUploadNode * node)
{
  node->priv = PINOS_UPLOAD_NODE_GET_PRIVATE (node);
  g_debug ("upload-node %p: new", node);
}

/**
 * pinos_upload_node_new:
 * @daemon: the parent #PinosDaemon
 * @possible_formats: a #GBytes
 *
 * Make a new #PinosNode that can be used to receive data from a client.
 *
 * Returns: a new #PinosNode.
 */
PinosNode *
pinos_upload_node_new (PinosDaemon *daemon,
                       GBytes      *possible_formats)
{
  return g_object_new (PINOS_TYPE_UPLOAD_NODE,
                       "daemon", daemon,
                       "name", "upload-node",
                       "possible-formats", possible_formats,
                       NULL);
}
