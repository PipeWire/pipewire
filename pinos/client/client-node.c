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
#include <gio/gunixfdlist.h>

#include "pinos/client/pinos.h"
#include "pinos/client/subscribe.h"
#include "pinos/client/enumtypes.h"

#include "pinos/client/context.h"
#include "pinos/client/private.h"
#include "pinos/client/client-node.h"
#include "pinos/client/client-port.h"

#define PINOS_CLIENT_NODE_GET_PRIVATE(node)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((node), PINOS_TYPE_CLIENT_NODE, PinosClientNodePrivate))

struct _PinosClientNodePrivate
{
  PinosContext *context;
  GDBusProxy *proxy;
};

G_DEFINE_TYPE (PinosClientNode, pinos_client_node, PINOS_TYPE_NODE);

enum
{
  PROP_0,
  PROP_CONTEXT,
  PROP_PROXY,
};

static gboolean
client_node_set_state (PinosNode       *node,
                       PinosNodeState   state)
{
  return FALSE;
}

typedef struct {
  PinosDirection direction;
  gchar *name;
  PinosProperties *properties;
  GBytes *possible_formats;
  GSocket *socket;
} CreatePortData;

static void
create_port_data_free (CreatePortData *data)
{
  g_free (data->name);
  if (data->properties)
    pinos_properties_free (data->properties);
  g_clear_object (&data->socket);
  g_slice_free (CreatePortData, data);
}

static void
on_port_proxy (GObject      *source_object,
               GAsyncResult *res,
               gpointer      user_data)
{
  GTask *task = user_data;
  CreatePortData *data = g_task_get_task_data (task);
  PinosClientNode *node = g_task_get_source_object (task);
  PinosClientNodePrivate *priv = node->priv;
  PinosContext *context = priv->context;
  GError *error = NULL;
  GDBusProxy *proxy;
  PinosClientPort *port;

  proxy = pinos_subscribe_get_proxy_finish (context->priv->subscribe,
                                            res,
                                            &error);
  if (proxy == NULL)
    goto port_failed;

  port = pinos_client_port_new (node,
                                proxy,
                                data->socket);
  g_task_return_pointer (task, port, (GDestroyNotify) g_object_unref);
  g_object_unref (task);

  return;

port_failed:
  {
    g_warning ("failed to get port proxy: %s", error->message);
    g_task_return_error (task, error);
    g_object_unref (task);
    return;
  }
}

static void
on_port_created (GObject      *source_object,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  GTask *task = user_data;
  PinosClientNode *node = g_task_get_source_object (task);
  CreatePortData *data = g_task_get_task_data (task);
  PinosClientNodePrivate *priv = node->priv;
  PinosContext *context = priv->context;
  GVariant *ret;
  GError *error = NULL;
  const gchar *port_path;
  GUnixFDList *fdlist;
  gint fd, fd_idx;

  g_assert (priv->proxy == G_DBUS_PROXY (source_object));

  ret = g_dbus_proxy_call_with_unix_fd_list_finish (priv->proxy, &fdlist, res, &error);
  if (ret == NULL)
    goto create_failed;

  g_variant_get (ret, "(&oh)", &port_path, &fd_idx);

  fd = g_unix_fd_list_get (fdlist, fd_idx, &error);
  g_object_unref (fdlist);
  if (fd == -1)
    goto create_failed;

  data->socket = g_socket_new_from_fd (fd, &error);
  if (data->socket == NULL)
    goto create_failed;

  pinos_subscribe_get_proxy (context->priv->subscribe,
                             PINOS_DBUS_SERVICE,
                             port_path,
                             "org.pinos.Port1",
                             NULL,
                             on_port_proxy,
                             task);
  g_variant_unref (ret);

  return;

  /* ERRORS */
create_failed:
  {
    g_warning ("failed to create port: %s", error->message);
    g_task_return_error (task, error);
    g_object_unref (task);
    if (ret)
      g_variant_unref (ret);
    return;
  }
}


static gboolean
do_create_port (GTask *task)
{
  PinosClientNode *node = g_task_get_source_object (task);
  PinosClientNodePrivate *priv = node->priv;
  CreatePortData *data = g_task_get_task_data (task);

  g_dbus_proxy_call (priv->proxy,
                     "CreatePort",
                     g_variant_new ("(us@a{sv}s)",
                       data->direction,
                       data->name,
                       pinos_properties_to_variant (data->properties),
                       g_bytes_get_data (data->possible_formats, NULL)),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     NULL, /* GCancellable *cancellable */
                     on_port_created,
                     task);
  return FALSE;
}


static void
client_node_create_port (PinosNode       *node,
                         PinosDirection   direction,
                         const gchar     *name,
                         GBytes          *possible_formats,
                         PinosProperties *properties,
                         GTask           *task)
{
  PinosClientNodePrivate *priv = PINOS_CLIENT_NODE (node)->priv;
  PinosContext *context = priv->context;
  CreatePortData *data;

  data = g_slice_new (CreatePortData);
  data->direction = direction;
  data->name = g_strdup (name);
  data->possible_formats = possible_formats ? g_bytes_ref (possible_formats) : NULL;
  data->properties = pinos_properties_merge (pinos_node_get_properties (node), properties);
  data->socket = NULL;

  g_task_set_task_data (task, data, (GDestroyNotify) create_port_data_free);

  g_main_context_invoke (context->priv->context,
                        (GSourceFunc) do_create_port,
                        task);

}

static void
client_node_remove_port (PinosNode       *node,
                         PinosPort       *port)
{
}

static void
pinos_client_node_get_property (GObject    *_object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  PinosClientNode *node = PINOS_CLIENT_NODE (_object);
  PinosClientNodePrivate *priv = node->priv;

  switch (prop_id) {
    case PROP_CONTEXT:
      g_value_set_object (value, priv->context);
      break;

    case PROP_PROXY:
      g_value_set_object (value, priv->proxy);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (node, prop_id, pspec);
      break;
  }
}

static void
pinos_client_node_set_property (GObject      *_object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  PinosClientNode *node = PINOS_CLIENT_NODE (_object);
  PinosClientNodePrivate *priv = node->priv;

  switch (prop_id) {
    case PROP_CONTEXT:
      priv->context = g_value_dup_object (value);
      break;

    case PROP_PROXY:
      priv->proxy = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (node, prop_id, pspec);
      break;
  }
}

static void
pinos_client_node_constructed (GObject * obj)
{
  PinosClientNode *node = PINOS_CLIENT_NODE (obj);

  g_debug ("client-node %p: constructed", node);

  G_OBJECT_CLASS (pinos_client_node_parent_class)->constructed (obj);
}

static void
pinos_client_node_dispose (GObject * obj)
{
  PinosClientNode *node = PINOS_CLIENT_NODE (obj);

  g_debug ("client-node %p: dispose", node);

  G_OBJECT_CLASS (pinos_client_node_parent_class)->dispose (obj);
}

static void
pinos_client_node_finalize (GObject * obj)
{
  PinosClientNode *node = PINOS_CLIENT_NODE (obj);
  PinosClientNodePrivate *priv = node->priv;

  g_debug ("client-node %p: finalize", node);
  g_clear_object (&priv->context);
  g_clear_object (&priv->proxy);

  G_OBJECT_CLASS (pinos_client_node_parent_class)->finalize (obj);
}

static void
pinos_client_node_class_init (PinosClientNodeClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PinosNodeClass *node_class = PINOS_NODE_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosClientNodePrivate));

  gobject_class->constructed = pinos_client_node_constructed;
  gobject_class->dispose = pinos_client_node_dispose;
  gobject_class->finalize = pinos_client_node_finalize;
  gobject_class->set_property = pinos_client_node_set_property;
  gobject_class->get_property = pinos_client_node_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_CONTEXT,
                                   g_param_spec_object ("context",
                                                        "Context",
                                                        "The Context",
                                                        PINOS_TYPE_CONTEXT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
                                   PROP_PROXY,
                                   g_param_spec_object ("proxy",
                                                        "Proxy",
                                                        "The Proxy",
                                                        G_TYPE_DBUS_PROXY,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
  node_class->set_state = client_node_set_state;
  node_class->create_port = client_node_create_port;
  node_class->remove_port = client_node_remove_port;
}

static void
pinos_client_node_init (PinosClientNode * node)
{
  node->priv = PINOS_CLIENT_NODE_GET_PRIVATE (node);

  g_debug ("client-node %p: new", node);
}

/**
 * pinos_client_node_get_context:
 * @node: a #PinosClientNode
 *
 * Get the context of @node.
 *
 * Returns: the context of @node.
 */
PinosContext *
pinos_client_node_get_context (PinosClientNode *node)
{
  PinosClientNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_CLIENT_NODE (node), NULL);
  priv = node->priv;

 return priv->context;
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
PinosClientNode *
pinos_client_node_new (PinosContext    *context,
                       gpointer         id)
{
  PinosClientNode *node;
  GDBusProxy *proxy = id;
  GVariant *variant;
  PinosProperties *properties = NULL;
  const gchar *name = NULL;

  variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Name");
  if (variant != NULL) {
    name = g_variant_get_string (variant, NULL);
    g_variant_unref (variant);
  }
  variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Properties");
  if (variant != NULL) {
    properties = pinos_properties_from_variant (variant);
    g_variant_unref (variant);
  }

  node = g_object_new (PINOS_TYPE_CLIENT_NODE,
                       "context", context,
                       "proxy", proxy,
                       "name", name,
                       "properties", properties,
                       NULL);
  return node;
}
