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

#include "pinos/client/client-port.h"

#define PINOS_CLIENT_PORT_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_CLIENT_PORT, PinosClientPortPrivate))

struct _PinosClientPortPrivate
{
  GDBusProxy *proxy;
};

G_DEFINE_TYPE (PinosClientPort, pinos_client_port, PINOS_TYPE_PORT);

enum
{
  PROP_0,
  PROP_PROXY,
};

static void
pinos_client_port_get_property (GObject    *_object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  PinosClientPort *port = PINOS_CLIENT_PORT (_object);
  PinosClientPortPrivate *priv = port->priv;

  switch (prop_id) {
    case PROP_PROXY:
      g_value_set_object (value, priv->proxy);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (port, prop_id, pspec);
      break;
  }
}

static void
pinos_client_port_set_property (GObject      *_object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  PinosClientPort *port = PINOS_CLIENT_PORT (_object);
  PinosClientPortPrivate *priv = port->priv;

  switch (prop_id) {
    case PROP_PROXY:
      priv->proxy = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (port, prop_id, pspec);
      break;
  }
}

static void
proxy_g_properties_changed (GDBusProxy *_proxy,
                            GVariant *changed_properties,
                            GStrv invalidated_properties,
                            gpointer user_data)
{
  PinosClientPort *port = user_data;
  GVariantIter *iter;
  gchar *key;

  g_variant_get (changed_properties, "a{sv}", &iter);
  while (g_variant_iter_next (iter, "{&sv}", &key, NULL)) {
    GVariant *variant;
    gsize size;
    gpointer data;
    GBytes *bytes;

    variant = g_dbus_proxy_get_cached_property (_proxy, key);
    if (variant == NULL)
      continue;

    if (strcmp (key, "PossibleFormats") == 0) {
      data = g_variant_dup_string (variant, &size);
      bytes = g_bytes_new_take (data, size);
      g_object_set (port, "possible-formats", bytes, NULL);
      g_bytes_unref (bytes);
    }
    g_variant_unref (variant);
  }
  g_variant_iter_free (iter);
}

static void
proxy_set_property_cb (GDBusProxy   *proxy,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  GVariant *ret;
  GError *error = NULL;

  ret = g_dbus_proxy_call_finish (proxy, res, &error);
  if (ret == NULL) {
    g_warning ("Error setting property: %s", error->message);
    g_error_free (error);
  } else
    g_variant_unref (ret);
}


static void
on_property_notify (GObject    *obj,
                    GParamSpec *pspec,
                    gpointer    user_data)
{
  PinosPort *port = PINOS_PORT (obj);
  PinosClientPortPrivate *priv = PINOS_CLIENT_PORT (port)->priv;
  const gchar *prop_name = NULL;
  GVariant *variant;

  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "possible-formats") == 0) {
    GBytes *bytes = pinos_port_get_possible_formats (port);
    prop_name = "PossibleFormats";
    variant = bytes ? g_variant_new_string (g_bytes_get_data (bytes, NULL)) : NULL;
  }
  if (prop_name) {
    g_dbus_proxy_call (G_DBUS_PROXY (priv->proxy),
                       "org.freedesktop.DBus.Properties.Set",
                       g_variant_new ("(ssv)",
                         "org.pinos.Port1",
                         prop_name,
                         variant),
                       G_DBUS_CALL_FLAGS_NONE,
                       -1,
                       NULL,
                       (GAsyncReadyCallback) proxy_set_property_cb,
                       port);
  }
}

static void
pinos_client_port_constructed (GObject * object)
{
  PinosClientPort *port = PINOS_CLIENT_PORT (object);
  PinosClientPortPrivate *priv = port->priv;

  g_debug ("client-port %p: constructed", port);
  g_signal_connect (priv->proxy,
                    "g-properties-changed",
                    (GCallback) proxy_g_properties_changed,
                    port);
  g_signal_connect (port, "notify", (GCallback) on_property_notify, port);

  G_OBJECT_CLASS (pinos_client_port_parent_class)->constructed (object);
}

static void
pinos_client_port_dispose (GObject * object)
{
  PinosClientPort *port = PINOS_CLIENT_PORT (object);

  g_debug ("client-port %p: dispose", port);

  G_OBJECT_CLASS (pinos_client_port_parent_class)->dispose (object);
}


static void
pinos_client_port_finalize (GObject * object)
{
  PinosClientPort *port = PINOS_CLIENT_PORT (object);
  PinosClientPortPrivate *priv = port->priv;

  g_debug ("client-port %p: finalize", port);
  g_clear_object (&priv->proxy);

  G_OBJECT_CLASS (pinos_client_port_parent_class)->finalize (object);
}

static void
pinos_client_port_class_init (PinosClientPortClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosClientPortPrivate));

  gobject_class->constructed = pinos_client_port_constructed;
  gobject_class->dispose = pinos_client_port_dispose;
  gobject_class->finalize = pinos_client_port_finalize;
  gobject_class->set_property = pinos_client_port_set_property;
  gobject_class->get_property = pinos_client_port_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_PROXY,
                                   g_param_spec_object ("proxy",
                                                        "Proxy",
                                                        "The Proxy",
                                                        G_TYPE_DBUS_PROXY,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
}

static void
pinos_client_port_init (PinosClientPort * port)
{
  port->priv = PINOS_CLIENT_PORT_GET_PRIVATE (port);
}

/**
 * pinos_client_port_new:
 * @node: a #PinosClientNode
 * @id: an id
 * @socket: a socket with the server port
 *
 * Create a new client port.
 *
 * Returns: a new client port
 */
PinosClientPort *
pinos_client_port_new (PinosClientNode *node,
                       gpointer         id,
                       GSocket         *socket)
{
  PinosClientPort *port;
  GDBusProxy *proxy = id;
  GVariant *variant;
  PinosDirection direction = PINOS_DIRECTION_INVALID;
  const gchar *name = "unknown";
  GBytes *possible_formats = NULL;
  GBytes *format = NULL;
  PinosProperties *properties = NULL;

  variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Direction");
  if (variant != NULL) {
    direction = g_variant_get_uint32 (variant);
    g_variant_unref (variant);
  }
  variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Name");
  if (variant != NULL) {
    name = g_variant_get_string (variant, NULL);
    g_variant_unref (variant);
  }
  variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "PossibleFormats");
  if (variant != NULL) {
    gsize size;
    gpointer data;
    data = g_variant_dup_string (variant, &size);
    possible_formats = g_bytes_new_take (data, size);
    g_variant_unref (variant);
  }
  variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Format");
  if (variant != NULL) {
    gsize size;
    gpointer data;
    data = g_variant_dup_string (variant, &size);
    format = g_bytes_new_take (data, size);
    g_variant_unref (variant);
  }
  variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Properties");
  if (variant != NULL) {
    properties = pinos_properties_from_variant (variant);
    g_variant_unref (variant);
  }

  port = g_object_new (PINOS_TYPE_CLIENT_PORT,
                       "node", node,
                       "direction", direction,
                       "name", name,
                       "possible-formats", possible_formats,
                       "format", format,
                       "properties", properties,
                       "proxy", proxy,
                       "socket", socket,
                       NULL);
  return port;
}
