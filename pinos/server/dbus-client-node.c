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
#include <sys/socket.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <poll.h>
#include <sys/eventfd.h>

#include "spa/lib/props.h"

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"
#include "pinos/client/private.h"

#include "pinos/server/daemon.h"
#include "pinos/server/dbus-client-node.h"

#include "pinos/dbus/org-pinos.h"

struct _PinosDBusClientNodePrivate
{
  int fd;
  GSocket *sockets[2];
  SpaHandle *handle;
};

#define PINOS_DBUS_CLIENT_NODE_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_DBUS_CLIENT_NODE, PinosDBusClientNodePrivate))

G_DEFINE_TYPE (PinosDBusClientNode, pinos_dbus_client_node, PINOS_TYPE_NODE);

enum
{
  PROP_0,
};

enum
{
  SIGNAL_NONE,
  LAST_SIGNAL
};

//static guint signals[LAST_SIGNAL] = { 0 };

static void
pinos_dbus_client_node_get_property (GObject    *_object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  PinosDBusClientNode *node = PINOS_DBUS_CLIENT_NODE (_object);

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (node, prop_id, pspec);
      break;
  }
}

static void
pinos_dbus_client_node_set_property (GObject      *_object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  PinosDBusClientNode *node = PINOS_DBUS_CLIENT_NODE (_object);

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (node, prop_id, pspec);
      break;
  }
}

/**
 * pinos_dbus_client_node_get_socket_pair:
 * @node: a #PinosDBusClientNode
 * @error: a #GError
 *
 * Create or return a previously create socket pair for @node. The
 * Socket for the other end is returned.
 *
 * Returns: a #GSocket that can be used to send/receive buffers to node.
 */
GSocket *
pinos_dbus_client_node_get_socket_pair (PinosDBusClientNode  *this,
                                   GError          **error)
{
  PinosNode *node;
  PinosDBusClientNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_DBUS_CLIENT_NODE (this), FALSE);
  node = PINOS_NODE (this);
  priv = this->priv;

  if (priv->sockets[1] == NULL) {
    SpaProps *props;
    SpaPropValue value;
    int fd[2];

    if (socketpair (AF_UNIX, SOCK_STREAM, 0, fd) != 0)
      goto no_sockets;

    priv->sockets[0] = g_socket_new_from_fd (fd[0], error);
    if (priv->sockets[0] == NULL)
      goto create_failed;

    priv->sockets[1] = g_socket_new_from_fd (fd[1], error);
    if (priv->sockets[1] == NULL)
      goto create_failed;

    priv->fd = g_socket_get_fd (priv->sockets[0]);

    spa_node_get_props (node->node, &props);
    value.value = &priv->fd;
    value.size = sizeof (int);
    spa_props_set_value (props, spa_props_index_for_name (props, "socket"), &value);
    spa_node_set_props (node->node, props);
  }
  return g_object_ref (priv->sockets[1]);

  /* ERRORS */
no_sockets:
  {
    g_set_error (error,
                 G_IO_ERROR,
                 g_io_error_from_errno (errno),
                 "could not create socketpair: %s", strerror (errno));
    return NULL;
  }
create_failed:
  {
    g_clear_object (&priv->sockets[0]);
    g_clear_object (&priv->sockets[1]);
    return NULL;
  }
}

static void
pinos_dbus_client_node_dispose (GObject * object)
{
  PinosDBusClientNode *this = PINOS_DBUS_CLIENT_NODE (object);
  PinosNode *node = PINOS_NODE (this);
  SpaProps *props;
  SpaPropValue value;
  int fd = -1;

  g_debug ("client-node %p: dispose", this);

  spa_node_get_props (node->node, &props);
  value.value = &fd;
  value.size = sizeof (int);
  spa_props_set_value (props, spa_props_index_for_name (props, "socket"), &value);

  value.value = NULL;
  value.size = sizeof (void *);
  spa_props_set_value (props, spa_props_index_for_name (props, "connection"), &value);
  spa_node_set_props (node->node, props);

  G_OBJECT_CLASS (pinos_dbus_client_node_parent_class)->dispose (object);
}

static void
pinos_dbus_client_node_finalize (GObject * object)
{
  PinosDBusClientNode *this = PINOS_DBUS_CLIENT_NODE (object);
  PinosDBusClientNodePrivate *priv = this->priv;

  g_debug ("client-node %p: finalize", this);

  g_clear_object (&priv->sockets[0]);
  g_clear_object (&priv->sockets[1]);
  spa_handle_clear (priv->handle);
  g_free (priv->handle);

  G_OBJECT_CLASS (pinos_dbus_client_node_parent_class)->finalize (object);
}

static void
pinos_dbus_client_node_constructed (GObject * object)
{
  PinosDBusClientNode *this = PINOS_DBUS_CLIENT_NODE (object);

  g_debug ("client-node %p: constructed", this);

  G_OBJECT_CLASS (pinos_dbus_client_node_parent_class)->constructed (object);
}

static void
pinos_dbus_client_node_class_init (PinosDBusClientNodeClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosDBusClientNodePrivate));

  gobject_class->constructed = pinos_dbus_client_node_constructed;
  gobject_class->dispose = pinos_dbus_client_node_dispose;
  gobject_class->finalize = pinos_dbus_client_node_finalize;
  gobject_class->set_property = pinos_dbus_client_node_set_property;
  gobject_class->get_property = pinos_dbus_client_node_get_property;
}

static void
pinos_dbus_client_node_init (PinosDBusClientNode * node)
{
  node->priv = PINOS_DBUS_CLIENT_NODE_GET_PRIVATE (node);

  g_debug ("client-node %p: new", node);
}

static SpaResult
make_node (PinosDaemon *daemon, SpaHandle **handle, SpaNode **node, const char *lib, const char *name)
{
  SpaResult res;
  void *hnd, *state = NULL;
  SpaEnumHandleFactoryFunc enum_func;

  if ((hnd = dlopen (lib, RTLD_NOW)) == NULL) {
    g_error ("can't load %s: %s", lib, dlerror());
    return SPA_RESULT_ERROR;
  }
  if ((enum_func = dlsym (hnd, "spa_enum_handle_factory")) == NULL) {
    g_error ("can't find enum function");
    return SPA_RESULT_ERROR;
  }

  while (true) {
    const SpaHandleFactory *factory;
    void *iface;

    if ((res = enum_func (&factory, &state)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        g_error ("can't enumerate factories: %d", res);
      break;
    }
    if (strcmp (factory->name, name))
      continue;

    *handle = calloc (1, factory->size);
    if ((res = factory->init (factory,
                              *handle,
                              NULL,
                              daemon->support,
                              daemon->n_support)) < 0) {
      g_error ("can't make factory instance: %d", res);
      return res;
    }
    if ((res = spa_handle_get_interface (*handle,
                                      daemon->registry.uri.spa_node,
                                      &iface)) < 0) {
      g_error ("can't get interface %d", res);
      return res;
    }
    *node = iface;
    return SPA_RESULT_OK;
  }
  return SPA_RESULT_ERROR;
}

/**
 * pinos_dbus_client_node_new:
 * @daemon: a #PinosDaemon
 * @client: the client owner
 * @path: a dbus path
 * @properties: extra properties
 *
 * Create a new #PinosNode.
 *
 * Returns: a new #PinosNode
 */
PinosNode *
pinos_dbus_client_node_new (PinosDaemon     *daemon,
                            PinosClient     *client,
                            const gchar     *path,
                            PinosProperties *properties)
{
  SpaNode *n;
  SpaResult res;
  SpaHandle *handle;
  PinosNode *node;
  SpaProps *props;
  SpaPropValue value;

  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);

  if ((res = make_node (daemon,
                        &handle,
                        &n,
                        "build/spa/plugins/remote/libspa-remote.so",
                        "dbus-proxy")) < 0) {
    g_error ("can't create proxy: %d", res);
    return NULL;
  }

  spa_node_get_props (n, &props);
  g_object_get (daemon, "connection", &value.value, NULL);
  value.size = sizeof (void *);
  spa_props_set_value (props, spa_props_index_for_name (props, "connection"), &value);
  spa_node_set_props (n, props);

  node =  g_object_new (PINOS_TYPE_DBUS_CLIENT_NODE,
                       "daemon", daemon,
                       "client", client,
                       "name", path,
                       "properties", properties,
                       "node", n,
                       NULL);

  PINOS_DBUS_CLIENT_NODE (node)->priv->handle = handle;

  return node;
}
