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

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"

#include "pinos/client/port.h"
#include "pinos/client/node.h"

#define PINOS_PORT_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_PORT, PinosPortPrivate))

struct _PinosPortPrivate
{
  PinosNode *node;

  gchar *name;
  PinosDirection direction;
  GBytes *possible_formats;
  PinosProperties *properties;
};

G_DEFINE_ABSTRACT_TYPE (PinosPort, pinos_port, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_NODE,
  PROP_NAME,
  PROP_DIRECTION,
  PROP_POSSIBLE_FORMATS,
  PROP_PROPERTIES
};

enum
{
  SIGNAL_FORMAT_REQUEST,
  SIGNAL_REMOVE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
pinos_port_get_property (GObject    *_object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  PinosPort *port = PINOS_PORT (_object);
  PinosPortPrivate *priv = port->priv;

  switch (prop_id) {
    case PROP_NODE:
      g_value_set_object (value, priv->node);
      break;

    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    case PROP_DIRECTION:
      g_value_set_enum (value, priv->direction);
      break;

    case PROP_POSSIBLE_FORMATS:
      g_value_set_boxed (value, priv->possible_formats);
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
    case PROP_NODE:
      priv->node = g_value_get_object (value);
      break;

    case PROP_NAME:
      priv->name = g_value_dup_string (value);
      break;

    case PROP_DIRECTION:
      priv->direction = g_value_get_enum (value);
      break;

    case PROP_POSSIBLE_FORMATS:
    {
      if (priv->possible_formats)
        g_bytes_unref (priv->possible_formats);
      priv->possible_formats = g_value_dup_boxed (value);
      break;
    }

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

  g_debug ("port %p: finalize", port);

  g_free (priv->name);
  if (priv->possible_formats)
    g_bytes_unref (priv->possible_formats);
  if (priv->properties)
    pinos_properties_free (priv->properties);

  G_OBJECT_CLASS (pinos_port_parent_class)->finalize (object);
}

static void
pinos_port_class_init (PinosPortClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosPortPrivate));

  gobject_class->constructed = pinos_port_constructed;
  gobject_class->dispose = pinos_port_dispose;
  gobject_class->finalize = pinos_port_finalize;
  gobject_class->set_property = pinos_port_set_property;
  gobject_class->get_property = pinos_port_get_property;

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
                                                        "The port name",
                                                        NULL,
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
                                   PROP_POSSIBLE_FORMATS,
                                   g_param_spec_boxed ("possible-formats",
                                                       "Possible Formats",
                                                       "The possbile formats of the port",
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
}

static void
pinos_port_init (PinosPort * port)
{
  PinosPortPrivate *priv = port->priv = PINOS_PORT_GET_PRIVATE (port);

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
 * pinos_port_get_formats:
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
pinos_port_get_formats (PinosPort  *port,
                        GBytes     *filter,
                        GError    **error)
{
  GstCaps *tmp, *caps, *cfilter;
  gchar *str;
  PinosPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_PORT (port), NULL);
  priv = port->priv;

  if (filter) {
    cfilter = gst_caps_from_string (g_bytes_get_data (filter, NULL));
    if (cfilter == NULL)
      goto invalid_filter;
  } else {
    cfilter = NULL;
  }

  g_signal_emit (port, signals[SIGNAL_FORMAT_REQUEST], 0, NULL);

  if (priv->possible_formats)
    caps = gst_caps_from_string (g_bytes_get_data (priv->possible_formats, NULL));
  else
    caps = gst_caps_new_any ();

  if (caps && cfilter) {
    tmp = gst_caps_intersect_full (caps, cfilter, GST_CAPS_INTERSECT_FIRST);
    g_clear_pointer (&cfilter, gst_caps_unref);
    gst_caps_take (&caps, tmp);
  }
  if (caps == NULL || gst_caps_is_empty (caps))
    goto no_format;

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
no_format:
  {
    if (error)
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "No compatible format found");
    if (cfilter)
      gst_caps_unref (cfilter);
    if (caps)
      gst_caps_unref (caps);
    return NULL;
  }
}
