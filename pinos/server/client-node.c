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

#include <gio/gunixfdlist.h>

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"
#include "pinos/client/private.h"

#include "pinos/server/daemon.h"
#include "pinos/server/client-node.h"
#include "pinos/server/utils.h"

#include "pinos/dbus/org-pinos.h"


#define MAX_BUFFER_SIZE 1024
#define MAX_FDS         16

struct _PinosClientNodePrivate
{
  int fd;
  GSource *socket_source;
  GSocket *sockets[2];

  PinosBuffer recv_buffer;

  guint8 recv_data[MAX_BUFFER_SIZE];
  int recv_fds[MAX_FDS];

  guint8 send_data[MAX_BUFFER_SIZE];
  int send_fds[MAX_FDS];
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

static guint signals[LAST_SIGNAL] = { 0 };

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

static gboolean
parse_buffer (PinosClientNode *cnode,
              PinosBuffer *pbuf)
{
  PinosNode *node = PINOS_NODE (cnode);
  PinosBufferIter it;
  PinosClientNodePrivate *priv = cnode->priv;
  PinosPort *port;

  pinos_buffer_iter_init (&it, pbuf);
  while (pinos_buffer_iter_next (&it)) {
    PinosPacketType type = pinos_buffer_iter_get_type (&it);

    switch (type) {
      case PINOS_PACKET_TYPE_FORMAT_CHANGE:
      {
        PinosPacketFormatChange p;
        GBytes *format;

        if (!pinos_buffer_iter_parse_format_change (&it, &p))
          break;

        if (!(port = pinos_node_find_port (node, p.port)))
          break;

        format = g_bytes_new_static (p.format, strlen (p.format) + 1);

        g_object_set (port, "possible-formats", format, NULL);
        g_object_set (port, "format", format, NULL);
        g_debug ("client-node %p: format change %s", node, p.format);
        break;
      }
      case PINOS_PACKET_TYPE_START:
      {
        GBytes *format;
        PinosBufferBuilder builder;
        PinosBuffer obuf;
        guint8 buffer[1024];
        GError *error = NULL;
        GList *ports, *walk;

        pinos_buffer_builder_init_into (&builder, buffer, 1024, NULL, 0);

        ports = pinos_node_get_ports (node);
        for (walk = ports; walk; walk = g_list_next (walk)) {
          PinosPacketFormatChange fc;

          port = walk->data;

          pinos_port_activate (port);

          g_object_get (port, "format", &format, "id", &fc.port, NULL);
          if (format == NULL)
            break;

          fc.id = 0;
          fc.format = g_bytes_get_data (format, NULL);

          g_debug ("client-node %p: port %u we are now streaming in format \"%s\"",
             node, fc.port, fc.format);

          pinos_buffer_builder_add_format_change (&builder, &fc);
        }
        pinos_buffer_builder_add_empty (&builder, PINOS_PACKET_TYPE_STREAMING);
        pinos_buffer_builder_end (&builder, &obuf);

        if (!pinos_io_write_buffer (priv->fd, &obuf, &error)) {
          g_warning ("client-node %p: error writing buffer: %s", node, error->message);
          g_clear_error (&error);
        }

        break;
      }
      case PINOS_PACKET_TYPE_STOP:
      {
        break;
      }
      case PINOS_PACKET_TYPE_REUSE_MEM:
      {
        break;
      }
      default:
        g_warning ("unhandled packet %d", type);
        break;
    }
  }
  pinos_buffer_iter_end (&it);

  return TRUE;
}

static gboolean
on_socket_condition (GSocket      *socket,
                     GIOCondition  condition,
                     gpointer      user_data)
{
  PinosClientNode *node = user_data;
  PinosClientNodePrivate *priv = node->priv;

  switch (condition) {
    case G_IO_IN:
    {
      PinosBuffer *buffer = &priv->recv_buffer;
      GError *error = NULL;

      if (!pinos_io_read_buffer (priv->fd,
                                 buffer,
                                 priv->recv_data,
                                 MAX_BUFFER_SIZE,
                                 priv->recv_fds,
                                 MAX_FDS,
                                 &error)) {
        g_warning ("client-node %p: failed to read buffer: %s", node, error->message);
        g_clear_error (&error);
        return TRUE;
      }

      parse_buffer (node, buffer);

#if 0
      if (!pinos_port_receive_buffer (priv->port, buffer, &error)) {
        g_warning ("client-node %p: port %p failed to receive buffer: %s", node, priv->port, error->message);
        g_clear_error (&error);
      }
#endif
      g_assert (pinos_buffer_unref (buffer) == FALSE);
      break;
    }

    case G_IO_OUT:
      g_warning ("can do IO OUT\n");
      break;

    default:
      break;
  }
  return TRUE;
}

static void
handle_socket (PinosClientNode *node, GSocket *socket)
{
  PinosClientNodePrivate *priv = node->priv;
  GMainContext *context = g_main_context_get_thread_default();

  g_debug ("client-node %p: handle socket in context %p", node, context);
  priv->fd = g_socket_get_fd (socket);
  priv->socket_source = g_socket_create_source (socket, G_IO_IN, NULL);
  g_source_set_callback (priv->socket_source, (GSourceFunc) on_socket_condition, node, NULL);
  g_source_attach (priv->socket_source, context);
}

static void
unhandle_socket (PinosClientNode *node)
{
  PinosClientNodePrivate *priv = node->priv;

  g_debug ("client-node %p: unhandle socket", node);
  if (priv->socket_source) {
    g_source_destroy (priv->socket_source);
    g_clear_pointer (&priv->socket_source, g_source_unref);
    priv->fd = -1;
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
pinos_client_node_get_socket_pair (PinosClientNode  *node,
                               GError       **error)
{
  PinosClientNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_CLIENT_NODE (node), FALSE);
  priv = node->priv;

  if (priv->sockets[1] == NULL) {
    int fd[2];

    if (socketpair (AF_UNIX, SOCK_STREAM, 0, fd) != 0)
      goto no_sockets;

    priv->sockets[0] = g_socket_new_from_fd (fd[0], error);
    if (priv->sockets[0] == NULL)
      goto create_failed;

    priv->sockets[1] = g_socket_new_from_fd (fd[1], error);
    if (priv->sockets[1] == NULL)
      goto create_failed;

    handle_socket (node, priv->sockets[0]);
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

static gboolean
on_received_buffer (PinosPort *port, PinosBuffer *buffer, GError **error, gpointer user_data)
{
  PinosClientNode *node = user_data;
  PinosClientNodePrivate *priv = node->priv;

  if (!pinos_io_write_buffer (priv->fd, buffer, error)) {
    g_warning ("client-node %p: error writing buffer: %s", node, (*error)->message);
    return FALSE;
  }
  return TRUE;
}

static PinosPort *
add_port (PinosNode       *node,
          PinosDirection   direction,
          guint            id,
          GError         **error)
{
  PinosPort *port;

  port = PINOS_NODE_CLASS (pinos_client_node_parent_class)->add_port (node, direction, id, error);

  if (port) {
    pinos_port_set_received_buffer_cb (port, on_received_buffer, node, NULL);
  }
  return port;
}

static gboolean
remove_port (PinosNode       *node,
             guint            id)
{
  return PINOS_NODE_CLASS (pinos_client_node_parent_class)->remove_port (node, id);
}

static void
pinos_client_node_dispose (GObject * object)
{
  PinosClientNode *node = PINOS_CLIENT_NODE (object);
  PinosClientNodePrivate *priv = node->priv;

  g_debug ("client-node %p: dispose", node);
  unhandle_socket (node);

  G_OBJECT_CLASS (pinos_client_node_parent_class)->dispose (object);
}

static void
pinos_client_node_finalize (GObject * object)
{
  PinosClientNode *node = PINOS_CLIENT_NODE (object);
  PinosClientNodePrivate *priv = node->priv;

  g_debug ("client-node %p: finalize", node);

  G_OBJECT_CLASS (pinos_client_node_parent_class)->finalize (object);
}

static void
pinos_client_node_constructed (GObject * object)
{
  PinosClientNode *node = PINOS_CLIENT_NODE (object);

  g_debug ("client-node %p: constructed", node);

  G_OBJECT_CLASS (pinos_client_node_parent_class)->constructed (object);
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

  node_class->add_port = add_port;
  node_class->remove_port = remove_port;
}

static void
pinos_client_node_init (PinosClientNode * node)
{
  PinosClientNodePrivate *priv = node->priv = PINOS_CLIENT_NODE_GET_PRIVATE (node);

  g_debug ("client-node %p: new", node);
}
