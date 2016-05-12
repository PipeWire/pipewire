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
};

G_DEFINE_TYPE (PinosUploadNode, pinos_upload_node, PINOS_TYPE_SERVER_NODE);

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

static gboolean
bus_handler (GstBus     *bus,
             GstMessage *message,
             gpointer    user_data)
{
  PinosServerNode *node = user_data;
  PinosUploadNodePrivate *priv = PINOS_UPLOAD_NODE (node)->priv;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:
    {
      GError *error;
      gchar *debug;

      gst_message_parse_error (message, &error, &debug);
      g_warning ("got error %s (%s)\n", error->message, debug);
      g_free (debug);

      pinos_node_report_error (PINOS_NODE (node), error);
      gst_element_set_state (priv->pipeline, GST_STATE_NULL);
      break;
    }
    case GST_MESSAGE_ELEMENT:
    {
      if (gst_message_has_name (message, "PinosPayloaderFormatChange")) {
        const GstStructure *str = gst_message_get_structure (message);
        GstCaps *caps;

        gst_structure_get (str, "format", GST_TYPE_CAPS, &caps, NULL);
        gst_caps_replace (&priv->format, caps);
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
node_set_state (PinosNode       *node,
                PinosNodeState   state)
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

  g_clear_object (&priv->sink);
  g_clear_object (&priv->src);
  g_clear_object (&priv->pipeline);

  if (priv->possible_formats)
    g_bytes_unref (priv->possible_formats);
  gst_caps_replace (&priv->format, NULL);

  G_OBJECT_CLASS (pinos_upload_node_parent_class)->finalize (object);
}

static void
on_input_port_created (GObject      *source_object,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  PinosNode *node = PINOS_NODE (source_object);
  PinosUploadNodePrivate *priv = PINOS_UPLOAD_NODE (source_object)->priv;

  priv->input = pinos_node_create_port_finish (node, res, NULL);
}

static void
on_output_port_created (GObject      *source_object,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  PinosNode *node = PINOS_NODE (source_object);
  PinosUploadNodePrivate *priv = PINOS_UPLOAD_NODE (source_object)->priv;

  priv->output = pinos_node_create_port_finish (node, res, NULL);
}

static void
upload_node_constructed (GObject * object)
{
  PinosServerNode *node = PINOS_SERVER_NODE (object);
  PinosUploadNode *upload = PINOS_UPLOAD_NODE (object);
  PinosUploadNodePrivate *priv = upload->priv;

  G_OBJECT_CLASS (pinos_upload_node_parent_class)->constructed (object);

  g_debug ("upload-node %p: constructed", upload);
  pinos_node_create_port (PINOS_NODE (node),
                                PINOS_DIRECTION_INPUT,
                                "input",
                                 priv->possible_formats,
                                NULL,
                                NULL,
                                on_input_port_created,
                                node);
  pinos_node_create_port (PINOS_NODE (node),
                                 PINOS_DIRECTION_OUTPUT,
                                 "output",
                                 priv->possible_formats,
                                 NULL,
                                 NULL,
                                 on_output_port_created,
                                 node);
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
 * Make a new #PinosServerNode that can be used to receive data from a client.
 *
 * Returns: a new #PinosServerNode.
 */
PinosServerNode *
pinos_upload_node_new (PinosDaemon *daemon,
                       GBytes      *possible_formats)
{
  return g_object_new (PINOS_TYPE_UPLOAD_NODE,
                       "daemon", daemon,
                       "name", "upload-node",
                       "possible-formats", possible_formats,
                       NULL);
}
