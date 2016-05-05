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

#include <gio/gio.h>

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"

#include "pinos/server/sink.h"
#include "pinos/server/node.h"

#include "pinos/dbus/org-pinos.h"


#define PINOS_SINK_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_SINK, PinosSinkPrivate))

struct _PinosSinkPrivate
{
  PinosNode *node;
  PinosSink1 *iface;

  gchar *name;
  PinosProperties *properties;

  PinosSinkState state;
  GError *error;
  guint idle_timeout;

  GList *channels;
};

G_DEFINE_ABSTRACT_TYPE (PinosSink, pinos_sink, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_NODE,
  PROP_NAME,
  PROP_STATE,
  PROP_PROPERTIES
};

static void
pinos_sink_get_property (GObject    *_object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  PinosSink *sink = PINOS_SINK (_object);
  PinosSinkPrivate *priv = sink->priv;

  switch (prop_id) {
    case PROP_NODE:
      g_value_set_object (value, priv->node);
      break;

    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    case PROP_STATE:
      g_value_set_enum (value, priv->state);
      break;

    case PROP_PROPERTIES:
      g_value_set_boxed (value, priv->properties);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (sink, prop_id, pspec);
      break;
  }
}

static void
pinos_sink_set_property (GObject      *_object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  PinosSink *sink = PINOS_SINK (_object);
  PinosSinkPrivate *priv = sink->priv;

  switch (prop_id) {
    case PROP_NODE:
      priv->node = g_value_dup_object (value);
      break;

    case PROP_NAME:
      g_free (priv->name);
      priv->name = g_value_dup_string (value);
      break;

    case PROP_PROPERTIES:
      if (priv->properties)
        pinos_properties_free (priv->properties);
      priv->properties = g_value_dup_boxed (value);
      if (priv->iface)
        g_object_set (priv->iface,
            "properties", priv->properties ?
                      pinos_properties_to_variant (priv->properties) : NULL,
            NULL);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (sink, prop_id, pspec);
      break;
  }
}

static void
sink_register_object (PinosSink *sink)
{
  PinosSinkPrivate *priv = sink->priv;
  GBytes *formats;
  GVariant *variant;

  formats = pinos_sink_get_formats (sink, NULL, NULL);

  if (priv->properties)
    variant = pinos_properties_to_variant (priv->properties);
  else
    variant = NULL;

  priv->iface = pinos_sink1_skeleton_new ();
  g_object_set (priv->iface, "name", priv->name,
                             "state", priv->state,
                             "properties", variant,
                             "possible-formats", g_bytes_get_data (formats, NULL),
                             NULL);
  g_bytes_unref (formats);

  pinos_node_set_sink (priv->node, sink, G_OBJECT (priv->iface));

  return;
}

static void
sink_unregister_object (PinosSink *sink)
{
  PinosSinkPrivate *priv = sink->priv;

  pinos_node_set_sink (priv->node, NULL, NULL);
  g_clear_object (&priv->iface);
}

static void
pinos_sink_constructed (GObject * object)
{
  PinosSink *sink = PINOS_SINK (object);

  sink_register_object (sink);

  G_OBJECT_CLASS (pinos_sink_parent_class)->constructed (object);
}

static void
do_remove_channel (PinosChannel *channel,
                  gpointer       user_data)
{
  pinos_channel_remove (channel);
}

static void
pinos_sink_dispose (GObject * object)
{
  PinosSink *sink = PINOS_SINK (object);
  PinosSinkPrivate *priv = sink->priv;

  g_list_foreach (priv->channels, (GFunc) do_remove_channel, sink);
  sink_unregister_object (sink);

  G_OBJECT_CLASS (pinos_sink_parent_class)->dispose (object);
}

static void
pinos_sink_finalize (GObject * object)
{
  PinosSink *sink = PINOS_SINK (object);
  PinosSinkPrivate *priv = sink->priv;

  g_free (priv->name);
  if (priv->properties)
    pinos_properties_free (priv->properties);

  G_OBJECT_CLASS (pinos_sink_parent_class)->finalize (object);
}

static gboolean
default_set_state (PinosSink      *sink,
                   PinosSinkState  state)
{
  pinos_sink_update_state (sink, state);
  return TRUE;
}

static void
handle_remove_channel (PinosChannel *channel,
                       gpointer      user_data)
{
  PinosSink *sink = user_data;

  pinos_sink_release_channel (sink, channel);
}

static PinosChannel *
default_create_channel (PinosSink     *sink,
                        const gchar     *client_path,
                        GBytes          *format_filter,
                        PinosProperties *props,
                        const gchar     *prefix,
                        GError          **error)
{
  PinosSinkPrivate *priv = sink->priv;
  PinosChannel *channel;
  GBytes *possible_formats;

  possible_formats = pinos_sink_get_formats (sink, format_filter, error);
  if (possible_formats == NULL)
    return NULL;

  channel = g_object_new (PINOS_TYPE_CHANNEL, "daemon", pinos_node_get_daemon (priv->node),
                                              "object-path", prefix,
                                              "client-path", client_path,
                                              "owner-path", pinos_node_get_object_path (priv->node),
                                              "possible-formats", possible_formats,
                                              "properties", props,
                                              NULL);
  g_bytes_unref (possible_formats);

  if (channel == NULL)
    goto no_channel;

  g_signal_connect (channel,
                    "remove",
                    (GCallback) handle_remove_channel,
                    sink);

  priv->channels = g_list_prepend (priv->channels, channel);

  return g_object_ref (channel);

  /* ERRORS */
no_channel:
  {
    if (error)
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_FAILED,
                            "Could not create channel");
    return NULL;
  }
}

static gboolean
default_release_channel (PinosSink  *sink,
                         PinosChannel *channel)
{
  PinosSinkPrivate *priv = sink->priv;
  GList *find;

  find = g_list_find (priv->channels, channel);
  if (find == NULL)
    return FALSE;

  priv->channels = g_list_delete_link (priv->channels, find);
  g_object_unref (channel);

  return TRUE;
}

static void
pinos_sink_class_init (PinosSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosSinkPrivate));

  gobject_class->constructed = pinos_sink_constructed;
  gobject_class->dispose = pinos_sink_dispose;
  gobject_class->finalize = pinos_sink_finalize;
  gobject_class->set_property = pinos_sink_set_property;
  gobject_class->get_property = pinos_sink_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_NODE,
                                   g_param_spec_object ("node",
                                                        "Node",
                                                        "The Node",
                                                        PINOS_TYPE_NODE,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Name",
                                                        "The sink name",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_STATE,
                                   g_param_spec_enum ("state",
                                                      "State",
                                                      "The state of the sink",
                                                      PINOS_TYPE_SINK_STATE,
                                                      PINOS_SINK_STATE_SUSPENDED,
                                                      G_PARAM_READABLE |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_PROPERTIES,
                                   g_param_spec_boxed ("properties",
                                                       "Properties",
                                                       "The properties of the sink",
                                                       PINOS_TYPE_PROPERTIES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));


  klass->set_state = default_set_state;
  klass->create_channel = default_create_channel;
  klass->release_channel = default_release_channel;
}

static void
pinos_sink_init (PinosSink * sink)
{
  PinosSinkPrivate *priv = sink->priv = PINOS_SINK_GET_PRIVATE (sink);

  priv->state = PINOS_SINK_STATE_SUSPENDED;
}

/**
 * pinos_sink_get_formats:
 * @sink: a #PinosSink
 * @filter: a #GBytes
 * @error: a #GError or %NULL
 *
 * Get all the currently supported formats for @sink and filter the
 * results with @filter.
 *
 * Returns: the list of supported format. If %NULL is returned, @error will
 * be set.
 */
GBytes *
pinos_sink_get_formats (PinosSink  *sink,
                          GBytes       *filter,
                          GError      **error)
{
  PinosSinkClass *klass;
  GBytes *res;

  g_return_val_if_fail (PINOS_IS_SINK (sink), NULL);

  klass = PINOS_SINK_GET_CLASS (sink);

  if (klass->get_formats)
    res = klass->get_formats (sink, filter, error);
  else {
    res = NULL;
    if (error)
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "Format query is not supported");
  }
  return res;
}

static void
remove_idle_timeout (PinosSink *sink)
{
  PinosSinkPrivate *priv = sink->priv;

  if (priv->idle_timeout) {
    g_source_remove (priv->idle_timeout);
    priv->idle_timeout = 0;
  }
}

/**
 * pinos_sink_set_state:
 * @sink: a #PinosSink
 * @state: a #PinosSinkState
 *
 * Set the state of @sink to @state.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_sink_set_state (PinosSink      *sink,
                        PinosSinkState  state)
{
  PinosSinkClass *klass;
  gboolean res;

  g_return_val_if_fail (PINOS_IS_SINK (sink), FALSE);

  klass = PINOS_SINK_GET_CLASS (sink);

  remove_idle_timeout (sink);

  if (klass->set_state)
    res = klass->set_state (sink, state);
  else
    res = FALSE;

  return res;
}

/**
 * pinos_sink_update_state:
 * @sink: a #PinosSink
 * @state: a #PinosSinkState
 *
 * Update the state of a sink. This method is used from
 * inside @sink itself.
 */
void
pinos_sink_update_state (PinosSink      *sink,
                           PinosSinkState  state)
{
  PinosSinkPrivate *priv;

  g_return_if_fail (PINOS_IS_SINK (sink));
  priv = sink->priv;

  if (priv->state != state) {
    priv->state = state;
    pinos_sink1_set_state (priv->iface, state);
    g_object_notify (G_OBJECT (sink), "state");
  }
}

/**
 * pinos_sink_report_error:
 * @sink: a #PinosSink
 * @error: a #GError
 *
 * Report an error from within @sink.
 */
void
pinos_sink_report_error (PinosSink *sink,
                           GError      *error)
{
  PinosSinkPrivate *priv;

  g_return_if_fail (PINOS_IS_SINK (sink));
  priv = sink->priv;

  g_clear_error (&priv->error);
  remove_idle_timeout (sink);
  priv->error = error;
  priv->state = PINOS_SINK_STATE_ERROR;
  g_debug ("got error state %s", error->message);
  pinos_sink1_set_state (priv->iface, priv->state);
  g_object_notify (G_OBJECT (sink), "state");
}

static gboolean
idle_timeout (PinosSink *sink)
{
  PinosSinkPrivate *priv = sink->priv;

  priv->idle_timeout = 0;
  pinos_sink_set_state (sink, PINOS_SINK_STATE_SUSPENDED);

  return G_SOURCE_REMOVE;
}

/**
 * pinos_sink_report_idle:
 * @sink: a #PinosSink
 *
 * Mark @sink as being idle. This will start a timeout that will
 * set the sink to SUSPENDED.
 */
void
pinos_sink_report_idle (PinosSink *sink)
{
  PinosSinkPrivate *priv;

  g_return_if_fail (PINOS_IS_SINK (sink));
  priv = sink->priv;

  pinos_sink_set_state (sink, PINOS_SINK_STATE_IDLE);

  priv->idle_timeout = g_timeout_add_seconds (3,
                                              (GSourceFunc) idle_timeout,
                                              sink);
}

/**
 * pinos_sink_report_busy:
 * @sink: a #PinosSink
 *
 * Mark @sink as being busy. This will set the state of the sink
 * to the RUNNING state.
 */
void
pinos_sink_report_busy (PinosSink *sink)
{
  g_return_if_fail (PINOS_IS_SINK (sink));

  pinos_sink_set_state (sink, PINOS_SINK_STATE_RUNNING);
}

/**
 * pinos_sink_update_possible_formats:
 * @sink: a #PinosSink
 * @formats: a #GBytes
 *
 * Update the possible formats in @sink to @formats. This function also
 * updates the possible formats of the channels.
 */
void
pinos_sink_update_possible_formats (PinosSink *sink, GBytes *formats)
{
  PinosSinkPrivate *priv;
  GList *walk;

  g_return_if_fail (PINOS_IS_SINK (sink));
  priv = sink->priv;

  if (priv->iface)
    g_object_set (priv->iface, "possible-formats",
                  g_bytes_get_data (formats, NULL),
                  NULL);

  for (walk = priv->channels; walk; walk = g_list_next (walk))
    g_object_set (walk->data, "possible-formats", formats, NULL);
}

/**
 * pinos_sink_update_format:
 * @sink: a #PinosSink
 * @format: a #GBytes
 *
 * Update the current format in @sink to @format. This function also
 * updates the current format of the channels.
 */
void
pinos_sink_update_format (PinosSink *sink, GBytes *format)
{
  PinosSinkPrivate *priv;
  GList *walk;

  g_return_if_fail (PINOS_IS_SINK (sink));
  priv = sink->priv;

  for (walk = priv->channels; walk; walk = g_list_next (walk))
    g_object_set (walk->data, "format", format, NULL);
}

/**
 * pinos_sink_create_channel:
 * @sink: a #PinosSink
 * @client_path: the client path
 * @format_filter: a #GBytes
 * @props: #PinosProperties
 * @prefix: a prefix
 * @error: a #GError or %NULL
 *
 * Create a new #PinosChannel for @sink.
 *
 * Returns: a new #PinosChannel or %NULL, in wich case @error will contain
 *          more information about the error.
 */
PinosChannel *
pinos_sink_create_channel (PinosSink     *sink,
                             const gchar     *client_path,
                             GBytes          *format_filter,
                             PinosProperties *props,
                             const gchar     *prefix,
                             GError          **error)
{
  PinosSinkClass *klass;
  PinosChannel *res;

  g_return_val_if_fail (PINOS_IS_SINK (sink), NULL);

  klass = PINOS_SINK_GET_CLASS (sink);

  if (klass->create_channel) {
    res = klass->create_channel (sink, client_path, format_filter, props, prefix, error);
  } else {
    if (error) {
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "CreateChannel not implemented");
    }
    res = NULL;
  }

  return res;
}

/**
 * pinos_sink_release_channel:
 * @sink: a #PinosSink
 * @channel: a #PinosChannel
 *
 * Release the @channel in @sink.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_sink_release_channel (PinosSink  *sink,
                              PinosChannel *channel)
{
  PinosSinkClass *klass;
  gboolean res;

  g_return_val_if_fail (PINOS_IS_SINK (sink), FALSE);
  g_return_val_if_fail (PINOS_IS_CHANNEL (channel), FALSE);

  klass = PINOS_SINK_GET_CLASS (sink);

  if (klass->release_channel)
    res = klass->release_channel (sink, channel);
  else
    res = FALSE;

  return res;
}
