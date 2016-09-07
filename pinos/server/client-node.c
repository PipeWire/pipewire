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

#define _GNU_SOURCE

#include <string.h>
#include <sys/socket.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <poll.h>
#include <sys/eventfd.h>

#include <gio/gunixfdlist.h>

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"
#include "pinos/client/private.h"

#include "pinos/server/daemon.h"
#include "pinos/server/client-node.h"
#include "pinos/server/link.h"

#include "pinos/dbus/org-pinos.h"

#include "spa/include/spa/control.h"


struct _PinosClientNodePrivate
{
  int fd;
  GSocket *sockets[2];
};

#define PINOS_CLIENT_NODE_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_CLIENT_NODE, PinosClientNodePrivate))

G_DEFINE_TYPE (PinosClientNode, pinos_client_node, PINOS_TYPE_NODE);

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
pinos_client_node_get_property (GObject    *_object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  PinosClientNode *node = PINOS_CLIENT_NODE (_object);

  switch (prop_id) {
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

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (node, prop_id, pspec);
      break;
  }
}

/**
 * pinos_client_node_get_socket_pair:
 * @node: a #PinosClientNode
 * @error: a #GError
 *
 * Create or return a previously create socket pair for @node. The
 * Socket for the other end is returned.
 *
 * Returns: a #GSocket that can be used to send/receive buffers to node.
 */
GSocket *
pinos_client_node_get_socket_pair (PinosClientNode  *this,
                                   GError          **error)
{
  PinosNode *node;
  PinosClientNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_CLIENT_NODE (this), FALSE);
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
    spa_props_set_prop (props, spa_props_index_for_name (props, "socket"), &value);
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
pinos_client_node_dispose (GObject * object)
{
  PinosClientNode *this = PINOS_CLIENT_NODE (object);

  g_debug ("client-node %p: dispose", this);

  G_OBJECT_CLASS (pinos_client_node_parent_class)->dispose (object);
}

static void
pinos_client_node_finalize (GObject * object)
{
  PinosNode *node = PINOS_NODE (object);
  PinosClientNode *this = PINOS_CLIENT_NODE (object);
  PinosClientNodePrivate *priv = this->priv;

  g_debug ("client-node %p: finalize", this);

  g_clear_object (&priv->sockets[0]);
  g_clear_object (&priv->sockets[1]);
  spa_handle_clear (node->node->handle);
  g_free (node->node->handle);

  G_OBJECT_CLASS (pinos_client_node_parent_class)->finalize (object);
}

static void
pinos_client_node_constructed (GObject * object)
{
  PinosClientNode *this = PINOS_CLIENT_NODE (object);

  g_debug ("client-node %p: constructed", this);

  G_OBJECT_CLASS (pinos_client_node_parent_class)->constructed (object);
}

static void
pinos_client_node_class_init (PinosClientNodeClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosClientNodePrivate));

  gobject_class->constructed = pinos_client_node_constructed;
  gobject_class->dispose = pinos_client_node_dispose;
  gobject_class->finalize = pinos_client_node_finalize;
  gobject_class->set_property = pinos_client_node_set_property;
  gobject_class->get_property = pinos_client_node_get_property;
}

static void
pinos_client_node_init (PinosClientNode * node)
{
  node->priv = PINOS_CLIENT_NODE_GET_PRIVATE (node);

  g_debug ("client-node %p: new", node);
}

static SpaResult
make_node (SpaNode **node, const char *lib, const char *name)
{
  SpaHandle *handle;
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

    handle = calloc (1, factory->size);
    if ((res = factory->init (factory, handle)) < 0) {
      g_error ("can't make factory instance: %d", res);
      return res;
    }
    if ((res = handle->get_interface (handle, SPA_INTERFACE_ID_NODE, &iface)) < 0) {
      g_error ("can't get interface %d", res);
      return res;
    }
    *node = iface;
    return SPA_RESULT_OK;
  }
  return SPA_RESULT_ERROR;
}

/**
 * pinos_client_node_new:
 * @daemon: a #PinosDaemon
 * @sender: the path of the owner
 * @name: a name
 * @properties: extra properties
 *
 * Create a new #PinosNode.
 *
 * Returns: a new #PinosNode
 */
PinosNode *
pinos_client_node_new (PinosDaemon     *daemon,
                       const gchar     *sender,
                       const gchar     *name,
                       PinosProperties *properties)
{
  SpaNode *n;
  SpaResult res;

  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);

  if ((res = make_node (&n,
                        "spa/build/plugins/remote/libspa-remote.so",
                        "proxy")) < 0) {
    g_error ("can't create proxy: %d", res);
    return NULL;
  }

  return g_object_new (PINOS_TYPE_CLIENT_NODE,
                       "daemon", daemon,
                       "sender", sender,
                       "name", name,
                       "properties", properties,
                       "node", n,
                       NULL);
}
