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


#include <gio/gunixfdlist.h>

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"
#include "pinos/client/private.h"

#include "pinos/server/daemon.h"
#include "pinos/server/client-node.h"
#include "pinos/server/utils.h"

#include "pinos/dbus/org-pinos.h"

#include "spa/include/spa/control.h"


#define MAX_BUFFER_SIZE 1024
#define MAX_FDS         16

struct _PinosClientNodePrivate
{
  int fd;
  GSource *socket_source;
  GSocket *sockets[2];

  SpaControl recv_control;

  guint8 recv_data[MAX_BUFFER_SIZE];
  int recv_fds[MAX_FDS];

  guint8 send_data[MAX_BUFFER_SIZE];
  int send_fds[MAX_FDS];

  GHashTable *mem_ids;
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

static gboolean
parse_control (PinosClientNode *cnode,
               SpaControl      *ctrl)
{
  PinosNode *node = PINOS_NODE (cnode);
  PinosClientNodePrivate *priv = cnode->priv;
  SpaControlIter it;

  spa_control_iter_init (&it, ctrl);
  while (spa_control_iter_next (&it) == SPA_RESULT_OK) {
    SpaControlCmd cmd = spa_control_iter_get_cmd (&it);

    switch (cmd) {
      case SPA_CONTROL_CMD_ADD_PORT:
      case SPA_CONTROL_CMD_REMOVE_PORT:
      case SPA_CONTROL_CMD_SET_FORMAT:
      case SPA_CONTROL_CMD_SET_PROPERTY:
      case SPA_CONTROL_CMD_END_CONFIGURE:
      case SPA_CONTROL_CMD_PAUSE:
      case SPA_CONTROL_CMD_START:
      case SPA_CONTROL_CMD_STOP:
        g_warning ("client-node %p: got unexpected control %d", node, cmd);
        break;

      case SPA_CONTROL_CMD_NODE_UPDATE:
      case SPA_CONTROL_CMD_PORT_UPDATE:
      case SPA_CONTROL_CMD_PORT_REMOVED:
        g_warning ("client-node %p: command not implemented %d", node, cmd);
        break;

      case SPA_CONTROL_CMD_START_CONFIGURE:
      {
        SpaControlBuilder builder;
        SpaControl control;
        guint8 buffer[1024];

        /* set port format */

        /* send end-configure */
        spa_control_builder_init_into (&builder, buffer, 1024, NULL, 0);
        spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_END_CONFIGURE, NULL);
        spa_control_builder_end (&builder, &control);

        if (spa_control_write (&control, priv->fd) < 0)
          g_warning ("client-node %p: error writing control", node);

        break;
      }
      case SPA_CONTROL_CMD_PORT_STATUS_CHANGE:
      {
        g_warning ("client-node %p: command not implemented %d", node, cmd);
        break;
      }
      case SPA_CONTROL_CMD_START_ALLOC:
      {
        SpaControlBuilder builder;
        SpaControl control;
        guint8 buffer[1024];
        GList *ports, *walk;

        /* FIXME read port memory requirements */
        /* FIXME add_mem */

        /* send start */
        spa_control_builder_init_into (&builder, buffer, 1024, NULL, 0);
        spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_START, NULL);
        spa_control_builder_end (&builder, &control);

        if (spa_control_write (&control, priv->fd) < 0)
          g_warning ("client-node %p: error writing control", node);

        ports = pinos_node_get_ports (node);
        for (walk = ports; walk; walk = g_list_next (walk)) {
          PinosPort *port = walk->data;

          pinos_port_activate (port);
        }
        break;
      }
      case SPA_CONTROL_CMD_NEED_INPUT:
      {
        break;
      }
      case SPA_CONTROL_CMD_HAVE_OUTPUT:
      {
        break;
      }

      case SPA_CONTROL_CMD_ADD_MEM:
        break;
      case SPA_CONTROL_CMD_REMOVE_MEM:
        break;
      case SPA_CONTROL_CMD_ADD_BUFFER:
        break;
      case SPA_CONTROL_CMD_REMOVE_BUFFER:
        break;

      case SPA_CONTROL_CMD_PROCESS_BUFFER:
      {
        break;
      }
      case SPA_CONTROL_CMD_REUSE_BUFFER:
      {
        break;
      }

      default:
        g_warning ("client-node %p: command unhandled %d", node, cmd);
        break;
    }
  }
  spa_control_iter_end (&it);

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
      SpaControl *control = &priv->recv_control;

      if (spa_control_read (control,
                            priv->fd,
                            priv->recv_data,
                            MAX_BUFFER_SIZE,
                            priv->recv_fds,
                            MAX_FDS) < 0) {
        g_warning ("client-node %p: failed to read buffer", node);
        return TRUE;
      }

      parse_control (node, control);

#if 0
      if (!pinos_port_receive_buffer (priv->port, buffer, &error)) {
        g_warning ("client-node %p: port %p failed to receive buffer: %s", node, priv->port, error->message);
        g_clear_error (&error);
      }
#endif
      spa_control_clear (control);
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

static void
on_format_change (GObject *obj,
                  GParamSpec *pspec,
                  gpointer user_data)
{
  PinosClientNode *node = user_data;
  PinosClientNodePrivate *priv = node->priv;
  GBytes *format;
  SpaControl control;
  SpaControlBuilder builder;
  SpaControlCmdSetFormat sf;
  guint8 buf[1024];

  g_object_get (obj, "format", &format, NULL);
  if (format == NULL)
    return ;

  g_debug ("port %p: format change %s", obj, (gchar*)g_bytes_get_data (format, NULL));

  spa_control_builder_init_into (&builder, buf, 1024, NULL, 0);
  sf.port = 0;
  sf.format = NULL;
  sf.str = g_bytes_get_data (format, NULL);
  spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_SET_FORMAT, &sf);
  spa_control_builder_end (&builder, &control);

  if (spa_control_write (&control, priv->fd))
    g_warning ("client-node %p: error writing control", node);
}


static int
tmpfile_create (void *data, gsize size)
{
  char filename[] = "/dev/shm/tmpfilepay.XXXXXX";
  int fd;

  fd = mkostemp (filename, O_CLOEXEC);
  if (fd == -1) {
    g_debug ("Failed to create temporary file: %s", strerror (errno));
    return -1;
  }
  unlink (filename);

  if (write (fd, data, size) != (gssize) size)
    g_debug ("Failed to write data: %s", strerror (errno));

  return fd;
}

typedef struct {
  SpaBuffer buffer;
  SpaData  datas[16];
  int idx[16];
  SpaBuffer *orig;
} MyBuffer;

static gboolean
on_received_buffer (PinosPort *port, SpaBuffer *buffer, GError **error, gpointer user_data)
{
  PinosClientNode *node = user_data;
  PinosClientNodePrivate *priv = node->priv;
  SpaControl control;
  SpaControlBuilder builder;
  guint8 buf[1024];
  int fds[16];
  SpaControlCmdAddBuffer ab;
  SpaControlCmdProcessBuffer pb;
  SpaControlCmdRemoveBuffer rb;
  bool tmpfile = false;

  if (pinos_port_get_direction (port) == PINOS_DIRECTION_OUTPUT) {
    /* FIXME, does not happen */
    spa_control_builder_init_into (&builder, buf, 1024, NULL, 0);
    spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_HAVE_OUTPUT, NULL);
    spa_control_builder_end (&builder, &control);

    if (spa_control_write (&control, priv->fd)) {
      g_warning ("client-node %p: error writing control", node);
      return FALSE;
    }
  } else {
    unsigned int i;
    MyBuffer b;

    spa_control_builder_init_into (&builder, buf, 1024, fds, 16);

    b.buffer.refcount = 1;
    b.buffer.notify = NULL;
    b.buffer.id = buffer->id;
    b.buffer.size = buffer->size;
    b.buffer.n_metas = buffer->n_metas;
    b.buffer.metas = buffer->metas;
    b.buffer.n_datas = buffer->n_datas;
    b.buffer.datas = b.datas;

    for (i = 0; i < buffer->n_datas; i++) {
      SpaData *d = &buffer->datas[i];
      int fd;
      SpaControlCmdAddMem am;

      if (d->type == SPA_DATA_TYPE_FD) {
        fd = *((int *)d->ptr);
      } else {
        fd = tmpfile_create (d->ptr, d->size + d->offset);
        tmpfile = true;
      }
      am.port = 0;
      am.id = i;
      am.type = 0;
      am.fd_index = spa_control_builder_add_fd (&builder, fd, tmpfile ? true : false);
      am.offset = 0;
      am.size = d->offset + d->size;
      spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_ADD_MEM, &am);

      b.idx[i] = i;
      b.datas[i].type = SPA_DATA_TYPE_MEMID;
      b.datas[i].ptr_type = NULL;
      b.datas[i].ptr = &b.idx[i];
      b.datas[i].offset = d->offset;
      b.datas[i].size = d->size;
      b.datas[i].stride = d->stride;
    }
    ab.port = 0;
    ab.buffer = &b.buffer;
    spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_ADD_BUFFER, &ab);
    pb.port = 0;
    pb.id = b.buffer.id;
    spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_PROCESS_BUFFER, &pb);
    rb.port = 0;
    rb.id = b.buffer.id;
    spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_REMOVE_BUFFER, &rb);

    for (i = 0; i < buffer->n_datas; i++) {
      SpaControlCmdRemoveMem rm;
      rm.port = 0;
      rm.id = i;
      spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_REMOVE_MEM, &rm);
    }
    spa_control_builder_end (&builder, &control);

    if (spa_control_write (&control, priv->fd))
      g_warning ("client-node %p: error writing control", node);

    spa_control_clear (&control);
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

    g_signal_connect (port, "notify::format", (GCallback) on_format_change, node);
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
  g_hash_table_unref (priv->mem_ids);

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
  node->priv = PINOS_CLIENT_NODE_GET_PRIVATE (node);

  g_debug ("client-node %p: new", node);
}
