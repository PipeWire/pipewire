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

#include <gio/gio.h>

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"

#include "pinos/server/port.h"
#include "pinos/server/node.h"
#include "pinos/server/utils.h"

#define PINOS_PORT_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_PORT, PinosPortPrivate))

#if 0
#define PINOS_DEBUG_TRANSPORT(format,args...) g_debug(format,##args)
#else
#define PINOS_DEBUG_TRANSPORT(format,args...)
#endif

typedef struct {
  gulong id;
  PinosBufferCallback send_buffer_cb;
  gpointer send_buffer_data;
  GDestroyNotify send_buffer_notify;
} SendData;

struct _PinosPortPrivate
{
  PinosDaemon *daemon;

  guint id;

  PinosNode *node;
  PinosDirection direction;
  GBytes *possible_formats;
  GBytes *format;
  PinosProperties *properties;

  PinosBufferCallback received_buffer_cb;
  gpointer received_buffer_data;
  GDestroyNotify received_buffer_notify;

  gint active_count;

  GList *send_datas;
};

G_DEFINE_TYPE (PinosPort, pinos_port, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_NODE,
  PROP_DIRECTION,
  PROP_ID,
  PROP_POSSIBLE_FORMATS,
  PROP_FORMAT,
  PROP_PROPERTIES,
};

enum
{
  SIGNAL_FORMAT_REQUEST,
  SIGNAL_REMOVE,
  SIGNAL_ACTIVATE,
  SIGNAL_DEACTIVATE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };


void
pinos_port_set_received_buffer_cb (PinosPort *port,
                                   PinosBufferCallback cb,
                                   gpointer user_data,
                                   GDestroyNotify notify)
{
  PinosPortPrivate *priv;

  g_return_if_fail (PINOS_IS_PORT (port));
  priv = port->priv;

  g_debug ("port %p: set receive callback", port);

  if (priv->received_buffer_notify)
    priv->received_buffer_notify (priv->received_buffer_data);;
  priv->received_buffer_cb = cb;
  priv->received_buffer_data = user_data;
  priv->received_buffer_notify = notify;
}

gulong
pinos_port_add_send_buffer_cb (PinosPort *port,
                               PinosBufferCallback cb,
                               gpointer user_data,
                               GDestroyNotify notify)
{
  PinosPortPrivate *priv;
  SendData *data;

  g_return_val_if_fail (PINOS_IS_PORT (port), -1);
  g_return_val_if_fail (cb != NULL, -1);
  priv = port->priv;

  g_debug ("port %p: add send callback", port);

  data = g_slice_new (SendData);
  data->id = priv->id++;
  data->send_buffer_cb = cb;
  data->send_buffer_data = user_data;
  data->send_buffer_notify = notify;
  priv->send_datas = g_list_prepend (priv->send_datas, data);

  return data->id;
}

void
pinos_port_remove_send_buffer_cb (PinosPort *port,
                                  gulong     id)
{
  PinosPortPrivate *priv;
  GList *walk;

  g_return_if_fail (PINOS_IS_PORT (port));
  priv = port->priv;

  g_debug ("port %p: remove send callback %lu", port, id);
  for (walk = priv->send_datas; walk; walk = g_list_next (walk)) {
    SendData *data = walk->data;

    if (data->id == id) {
      if (data->send_buffer_notify)
        data->send_buffer_notify (data->send_buffer_data);;
      g_slice_free (SendData, data);
      priv->send_datas = g_list_delete_link (priv->send_datas, walk);
      break;
    }
  }
}

static void
pinos_port_get_property (GObject    *_object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  PinosPort *port = PINOS_PORT (_object);
  PinosPortPrivate *priv = port->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      g_value_set_object (value, priv->daemon);
      break;

    case PROP_NODE:
      g_value_set_object (value, priv->node);
      break;

    case PROP_DIRECTION:
      g_value_set_enum (value, priv->direction);
      break;

    case PROP_ID:
      g_value_set_uint (value, priv->id);
      break;

    case PROP_POSSIBLE_FORMATS:
      g_value_set_boxed (value, priv->possible_formats);
      break;

    case PROP_FORMAT:
      g_value_set_boxed (value, priv->format);
      break;

    case PROP_PROPERTIES:
      g_value_set_boxed (value, priv->properties);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (port, prop_id, pspec);
      break;
  }
}

static void
pinos_port_set_property (GObject      *_object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  PinosPort *port = PINOS_PORT (_object);
  PinosPortPrivate *priv = port->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    case PROP_NODE:
      priv->node = g_value_get_object (value);
      break;

    case PROP_DIRECTION:
      priv->direction = g_value_get_enum (value);
      break;

    case PROP_ID:
      priv->id = g_value_get_uint (value);
      break;

    case PROP_POSSIBLE_FORMATS:
      if (priv->possible_formats)
        g_bytes_unref (priv->possible_formats);
      priv->possible_formats = g_value_dup_boxed (value);
      break;

    case PROP_FORMAT:
      if (priv->format)
        g_bytes_unref (priv->format);
      priv->format = g_value_dup_boxed (value);
      break;

    case PROP_PROPERTIES:
      if (priv->properties)
        pinos_properties_free (priv->properties);
      priv->properties = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (port, prop_id, pspec);
      break;
  }
}

static void
pinos_port_constructed (GObject * object)
{
  PinosPort *port = PINOS_PORT (object);

  g_debug ("port %p: constructed", port);

  G_OBJECT_CLASS (pinos_port_parent_class)->constructed (object);
}

static void
pinos_port_dispose (GObject * object)
{
  PinosPort *port = PINOS_PORT (object);

  g_debug ("port %p: dispose", port);

  G_OBJECT_CLASS (pinos_port_parent_class)->dispose (object);
}

static void
pinos_port_finalize (GObject * object)
{
  PinosPort *port = PINOS_PORT (object);
  PinosPortPrivate *priv = port->priv;
  GList *walk;

  g_debug ("port %p: finalize", port);
  g_clear_pointer (&priv->possible_formats, g_bytes_unref);
  g_clear_pointer (&priv->format, g_bytes_unref);
  g_clear_pointer (&priv->properties, pinos_properties_free);
  if (priv->received_buffer_notify)
    priv->received_buffer_notify (priv->received_buffer_data);
  for (walk = priv->send_datas; walk; walk = g_list_next (walk)) {
    SendData *data = walk->data;
    if (data->send_buffer_notify)
      data->send_buffer_notify (data->send_buffer_data);
    g_slice_free (SendData, data);
  }
  g_list_free (priv->send_datas);

  g_clear_object (&priv->daemon);

  G_OBJECT_CLASS (pinos_port_parent_class)->finalize (object);
}

static void
pinos_port_class_init (PinosPortClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  //PinosPortClass *port_class = PINOS_PORT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosPortPrivate));

  gobject_class->constructed = pinos_port_constructed;
  gobject_class->dispose = pinos_port_dispose;
  gobject_class->finalize = pinos_port_finalize;
  gobject_class->set_property = pinos_port_set_property;
  gobject_class->get_property = pinos_port_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "The Daemon",
                                                        PINOS_TYPE_DAEMON,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
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
                                   PROP_DIRECTION,
                                   g_param_spec_enum ("direction",
                                                      "Direction",
                                                      "The direction of the port",
                                                      PINOS_TYPE_DIRECTION,
                                                      PINOS_DIRECTION_INVALID,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT_ONLY |
                                                      G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
                                   PROP_ID,
                                   g_param_spec_uint ("id",
                                                      "Id",
                                                      "The id of the port",
                                                      0,
                                                      G_MAXUINT,
                                                      0,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT_ONLY |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_POSSIBLE_FORMATS,
                                   g_param_spec_boxed ("possible-formats",
                                                       "Possible Formats",
                                                       "The possbile formats of the port",
                                                       G_TYPE_BYTES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_FORMAT,
                                   g_param_spec_boxed ("format",
                                                       "Format",
                                                       "The format of the port",
                                                       G_TYPE_BYTES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_PROPERTIES,
                                   g_param_spec_boxed ("properties",
                                                       "Properties",
                                                       "The properties of the port",
                                                       PINOS_TYPE_PROPERTIES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));


  signals[SIGNAL_FORMAT_REQUEST] = g_signal_new ("format-request",
                                                 G_TYPE_FROM_CLASS (klass),
                                                 G_SIGNAL_RUN_LAST,
                                                 0,
                                                 NULL,
                                                 NULL,
                                                 g_cclosure_marshal_generic,
                                                 G_TYPE_NONE,
                                                 0,
                                                 G_TYPE_NONE);

  signals[SIGNAL_REMOVE] = g_signal_new ("remove",
                                         G_TYPE_FROM_CLASS (klass),
                                         G_SIGNAL_RUN_LAST,
                                         0,
                                         NULL,
                                         NULL,
                                         g_cclosure_marshal_generic,
                                         G_TYPE_NONE,
                                         0,
                                         G_TYPE_NONE);
 signals[SIGNAL_ACTIVATE] = g_signal_new ("activate",
                                          G_TYPE_FROM_CLASS (klass),
                                          G_SIGNAL_RUN_LAST,
                                          0,
                                          NULL,
                                          NULL,
                                          g_cclosure_marshal_generic,
                                          G_TYPE_NONE,
                                          0,
                                          G_TYPE_NONE);
  signals[SIGNAL_DEACTIVATE] = g_signal_new ("deactivate",
                                             G_TYPE_FROM_CLASS (klass),
                                             G_SIGNAL_RUN_LAST,
                                             0,
                                             NULL,
                                             NULL,
                                             g_cclosure_marshal_generic,
                                             G_TYPE_NONE,
                                             0,
                                             G_TYPE_NONE);
}

static void
pinos_port_init (PinosPort * port)
{
  PinosPortPrivate *priv = port->priv = PINOS_PORT_GET_PRIVATE (port);

  g_debug ("port %p: new", port);
  priv->direction = PINOS_DIRECTION_INVALID;
}

/**
 * pinos_port_remove:
 * @port: a #PinosPort
 *
 * Trigger removal of @port
 */
void
pinos_port_remove (PinosPort *port)
{
  g_return_if_fail (PINOS_IS_PORT (port));

  g_debug ("port %p: remove", port);
  g_signal_emit (port, signals[SIGNAL_REMOVE], 0, NULL);
}

/**
 * pinos_port_get_node:
 * @port: a #PinosPort
 *
 * Get the parent #PinosNode of @port
 *
 * Returns: the parent node or %NULL
 */
PinosNode *
pinos_port_get_node (PinosPort *port)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), NULL);
  priv = port->priv;

  return priv->node;
}

/**
 * pinos_port_get_direction:
 * @port: a #PinosPort
 *
 * Get the direction of @port
 *
 * Returns: the direction or %NULL
 */
PinosDirection
pinos_port_get_direction (PinosPort *port)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), PINOS_DIRECTION_INVALID);
  priv = port->priv;

  return priv->direction;
}

/**
 * pinos_port_get_id:
 * @port: a #PinosPort
 *
 * Get the id of @port
 *
 * Returns: the id or %NULL
 */
guint
pinos_port_get_id (PinosPort *port)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), -1);
  priv = port->priv;

  return priv->id;
}

/**
 * pinos_port_get_possible_formats:
 * @port: a #PinosPort
 *
 * Get the possible formats of @port
 *
 * Returns: the possible formats or %NULL
 */
GBytes *
pinos_port_get_possible_formats (PinosPort *port)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), NULL);
  priv = port->priv;

  g_signal_emit (port, signals[SIGNAL_FORMAT_REQUEST], 0, NULL);
  return priv->possible_formats;
}

/**
 * pinos_port_get_format:
 * @port: a #PinosPort
 *
 * Get the format of @port
 *
 * Returns: the format or %NULL
 */
GBytes *
pinos_port_get_format (PinosPort *port)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), NULL);
  priv = port->priv;

  return priv->format;
}

/**
 * pinos_port_get_properties:
 * @port: a #PinosPort
 *
 * Get the properties of @port
 *
 * Returns: the properties or %NULL
 */
PinosProperties *
pinos_port_get_properties (PinosPort *port)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), NULL);
  priv = port->priv;

  return priv->properties;
}

/**
 * pinos_port_filter_formats:
 * @port: a #PinosPort
 * @filter: a #GBytes
 * @error: a #GError or %NULL
 *
 * Get all the currently supported formats for @port and filter the
 * results with @filter.
 *
 * Returns: the list of supported format. If %NULL is returned, @error will
 * be set.
 */
GBytes *
pinos_port_filter_formats (PinosPort  *port,
                           GBytes     *filter,
                           GError    **error)
{
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), NULL);
  priv = port->priv;

  g_signal_emit (port, signals[SIGNAL_FORMAT_REQUEST], 0, NULL);

  return pinos_format_filter (priv->possible_formats, filter, error);
}

void
pinos_port_activate (PinosPort *port)
{
  PinosPortPrivate *priv;

  g_return_if_fail (PINOS_IS_PORT (port));
  priv = port->priv;
  g_return_if_fail (priv->active_count >= 0);

  g_debug ("port %p: activate count now %d", port, priv->active_count);

  if (priv->active_count++ == 0)
    g_signal_emit (port, signals[SIGNAL_ACTIVATE], 0, NULL);
}

void
pinos_port_deactivate (PinosPort *port)
{
  PinosPortPrivate *priv;

  g_return_if_fail (PINOS_IS_PORT (port));
  priv = port->priv;
  g_return_if_fail (priv->active_count > 0);

  g_debug ("port %p: deactivate count now %d", port, priv->active_count);

  if (--priv->active_count == 0)
    g_signal_emit (port, signals[SIGNAL_DEACTIVATE], 0, NULL);
}


/**
 * pinos_port_receive_buffer:
 * @port: a #PinosPort
 * @buffer: a #PinosBuffer
 * @error: a #GError or %NULL
 *
 * Receive @buffer on @port
 *
 * Returns: %TRUE on success. @error is set when %FALSE is returned.
 */
gboolean
pinos_port_receive_buffer (PinosPort   *port,
                           SpaBuffer   *buffer,
                           GError     **error)
{
  gboolean res = TRUE;
  PinosPortPrivate *priv = port->priv;

  PINOS_DEBUG_TRANSPORT ("port %p: receive buffer %p", port, buffer);

  if (priv->received_buffer_cb)
    res = priv->received_buffer_cb (port, buffer, error, priv->received_buffer_data);

  return res;
}

/**
 * pinos_port_send_buffer:
 * @port: a #PinosPort
 * @buffer: a #SpaBuffer
 * @error: a #GError or %NULL
 *
 * Send @buffer out on @port.
 *
 * Returns: %TRUE on success. @error is set when %FALSE is returned.
 */
gboolean
pinos_port_send_buffer (PinosPort   *port,
                        SpaBuffer   *buffer,
                        GError     **error)
{
  gboolean res = TRUE;
  PinosPortPrivate *priv;
  GList *walk;

  g_return_val_if_fail (PINOS_IS_PORT (port), FALSE);

  PINOS_DEBUG_TRANSPORT ("port %p: send buffer %p", port, buffer);

  priv = port->priv;

  for (walk = priv->send_datas; walk; walk = g_list_next (walk)) {
    SendData *data = walk->data;
    data->send_buffer_cb (port, buffer, error, data->send_buffer_data);
  }
  return res;
}
