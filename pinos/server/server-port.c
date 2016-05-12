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

#include "pinos/server/server-port.h"
#include "pinos/server/server-node.h"

#include "pinos/dbus/org-pinos.h"

#define PINOS_SERVER_PORT_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_SERVER_PORT, PinosServerPortPrivate))

struct _PinosServerPortPrivate
{
  PinosDaemon *daemon;
  PinosPort1 *iface;
  gchar *object_path;
};

G_DEFINE_TYPE (PinosServerPort, pinos_server_port, PINOS_TYPE_PORT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_OBJECT_PATH,
};

static gboolean
handle_remove (PinosPort1             *interface,
               GDBusMethodInvocation  *invocation,
               gpointer                user_data)
{
  PinosPort *port = user_data;

  g_debug ("server-port %p: remove", port);
  pinos_port_remove (port);

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("()"));
  return TRUE;
}

static void
pinos_server_port_get_property (GObject    *_object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  PinosServerPort *port = PINOS_SERVER_PORT (_object);
  PinosServerPortPrivate *priv = port->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      g_value_set_object (value, priv->daemon);
      break;

    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (port, prop_id, pspec);
      break;
  }
}

static void
pinos_server_port_set_property (GObject      *_object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  PinosServerPort *port = PINOS_SERVER_PORT (_object);
  PinosServerPortPrivate *priv = port->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (port, prop_id, pspec);
      break;
  }
}

static void
port_register_object (PinosServerPort *port)
{
  PinosServerPortPrivate *priv = port->priv;
  PinosObjectSkeleton *skel;
  gchar *name;
  PinosNode *node;

  node = pinos_port_get_node (PINOS_PORT (port));

  name = g_strdup_printf ("%s/port", pinos_server_node_get_object_path (PINOS_SERVER_NODE (node)));
  skel = pinos_object_skeleton_new (name);
  g_free (name);

  pinos_object_skeleton_set_port1 (skel, priv->iface);

  g_free (priv->object_path);
  priv->object_path = pinos_daemon_export_uniquely (priv->daemon,
      G_DBUS_OBJECT_SKELETON (skel));
  g_object_unref (skel);
  g_debug ("server-port %p: register object %s", port, priv->object_path);

  return;
}

static void
port_unregister_object (PinosServerPort *port)
{
  PinosServerPortPrivate *priv = port->priv;

  g_debug ("server-port %p: unregister object %s", port, priv->object_path);
  pinos_daemon_unexport (priv->daemon, priv->object_path);
}

static void
pinos_server_port_constructed (GObject * object)
{
  PinosServerPort *port = PINOS_SERVER_PORT (object);

  g_debug ("server-port %p: constructed", port);
  port_register_object (port);

  G_OBJECT_CLASS (pinos_server_port_parent_class)->constructed (object);
}

static void
pinos_server_port_dispose (GObject * object)
{
  PinosServerPort *port = PINOS_SERVER_PORT (object);

  g_debug ("server-port %p: dispose", port);
  port_unregister_object (port);

  G_OBJECT_CLASS (pinos_server_port_parent_class)->dispose (object);
}

static void
pinos_server_port_finalize (GObject * object)
{
  PinosServerPort *port = PINOS_SERVER_PORT (object);
  PinosServerPortPrivate *priv = port->priv;

  g_debug ("server-port %p: finalize", port);
  g_free (priv->object_path);
  g_clear_object (&priv->iface);
  g_clear_object (&priv->daemon);

  G_OBJECT_CLASS (pinos_server_port_parent_class)->finalize (object);
}

static void
pinos_server_port_class_init (PinosServerPortClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosServerPortPrivate));

  gobject_class->constructed = pinos_server_port_constructed;
  gobject_class->dispose = pinos_server_port_dispose;
  gobject_class->finalize = pinos_server_port_finalize;
  gobject_class->set_property = pinos_server_port_set_property;
  gobject_class->get_property = pinos_server_port_get_property;

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
                                   PROP_OBJECT_PATH,
                                   g_param_spec_string ("object-path",
                                                        "Object Path",
                                                        "The object path",
                                                        NULL,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));
}

static void
pinos_server_port_init (PinosServerPort * port)
{
  PinosServerPortPrivate *priv = port->priv = PINOS_SERVER_PORT_GET_PRIVATE (port);

  g_debug ("server-port %p: new", port);
  priv->iface = pinos_port1_skeleton_new ();
  g_signal_connect (priv->iface, "handle-remove",
                                 (GCallback) handle_remove,
                                 port);

}

const gchar *
pinos_server_port_get_object_path (PinosServerPort *port)
{
  PinosServerPortPrivate *priv;

  g_return_val_if_fail (PINOS_IS_SERVER_PORT (port), NULL);
  priv = port->priv;

  return priv->object_path;
}
