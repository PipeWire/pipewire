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
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/eventfd.h>

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"
#include "pinos/client/private.h"
#include "pinos/client/connection.h"
#include "pinos/client/serialize.h"

#include "pinos/server/daemon.h"
#include "pinos/server/client-node.h"

#include "spa/include/spa/node.h"
#include "spa/include/spa/queue.h"
#include "spa/lib/memfd-wrappers.h"

#define MAX_INPUTS       64
#define MAX_OUTPUTS      64

#define MAX_BUFFERS      16

#define CHECK_IN_PORT_ID(this,d,p)       ((d) == SPA_DIRECTION_INPUT && (p) < MAX_INPUTS)
#define CHECK_OUT_PORT_ID(this,d,p)      ((d) == SPA_DIRECTION_OUTPUT && (p) < MAX_OUTPUTS)
#define CHECK_PORT_ID(this,d,p)          (CHECK_IN_PORT_ID(this,d,p) || CHECK_OUT_PORT_ID(this,d,p))
#define CHECK_FREE_IN_PORT(this,d,p)     (CHECK_IN_PORT_ID(this,d,p) && !(this)->in_ports[p].valid)
#define CHECK_FREE_OUT_PORT(this,d,p)    (CHECK_OUT_PORT_ID(this,d,p) && !(this)->out_ports[p].valid)
#define CHECK_FREE_PORT(this,d,p)        (CHECK_FREE_IN_PORT (this,d,p) || CHECK_FREE_OUT_PORT (this,d,p))
#define CHECK_IN_PORT(this,d,p)          (CHECK_IN_PORT_ID(this,d,p) && (this)->in_ports[p].valid)
#define CHECK_OUT_PORT(this,d,p)         (CHECK_OUT_PORT_ID(this,d,p) && (this)->out_ports[p].valid)
#define CHECK_PORT(this,d,p)             (CHECK_IN_PORT (this,d,p) || CHECK_OUT_PORT (this,d,p))

typedef struct _SpaProxy SpaProxy;
typedef struct _ProxyBuffer ProxyBuffer;

struct _ProxyBuffer {
  SpaBuffer *outbuf;
  SpaBuffer  buffer;
  SpaMeta    metas[4];
  SpaData    datas[4];
  off_t      offset;
  size_t     size;
};

typedef struct {
  bool           valid;
  SpaPortInfo   *info;
  SpaFormat     *format;
  unsigned int   n_formats;
  SpaFormat    **formats;
  SpaPortStatus  status;

  unsigned int   n_buffers;
  ProxyBuffer    buffers[MAX_BUFFERS];

  uint32_t       buffer_mem_id;
  int            buffer_mem_fd;
  size_t         buffer_mem_size;
  void          *buffer_mem_ptr;

  uint32_t       buffer_id;
  SpaQueue       ready;
} SpaProxyPort;

struct _SpaProxy
{
  SpaNode    node;

  SpaIDMap *map;
  SpaLog *log;
  SpaPoll *main_loop;
  SpaPoll *data_loop;

  SpaNodeEventCallback event_cb;
  void *user_data;

  SpaPollFd fds[1];
  SpaPollItem poll;
  PinosConnection *conn;

  SpaPollFd rtfds[1];
  SpaPollItem rtpoll;
  PinosConnection *rtconn;

  unsigned int max_inputs;
  unsigned int n_inputs;
  unsigned int max_outputs;
  unsigned int n_outputs;
  SpaProxyPort in_ports[MAX_INPUTS];
  SpaProxyPort out_ports[MAX_OUTPUTS];

  uint32_t seq;
};

struct _PinosClientNodePrivate
{
  SpaProxy *proxy;

  GSocket *sockets[2];
  GSocket *rtsockets[2];
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
send_async_complete (SpaProxy *this, uint32_t seq, SpaResult res)
{
  SpaNodeEvent event;
  SpaNodeEventAsyncComplete ac;

  event.type = SPA_NODE_EVENT_TYPE_ASYNC_COMPLETE;
  event.data = &ac;
  event.size = sizeof (ac);
  ac.seq = seq;
  ac.res = res;
  this->event_cb (&this->node, &event, this->user_data);
}

static SpaResult
clear_buffers (SpaProxy *this, SpaProxyPort *port)
{
  if (port->n_buffers) {
    spa_log_info (this->log, "proxy %p: clear buffers\n", this);

    munmap (port->buffer_mem_ptr, port->buffer_mem_size);
    close (port->buffer_mem_fd);

    port->n_buffers = 0;
    SPA_QUEUE_INIT (&port->ready);
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_get_props (SpaNode       *node,
                          SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_proxy_node_set_props (SpaNode         *node,
                          const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_proxy_node_send_command (SpaNode        *node,
                             SpaNodeCommand *command)
{
  SpaProxy *this;
  SpaResult res = SPA_RESULT_OK;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  switch (command->type) {
    case SPA_NODE_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_NODE_COMMAND_START:
    case SPA_NODE_COMMAND_PAUSE:
    case SPA_NODE_COMMAND_FLUSH:
    case SPA_NODE_COMMAND_DRAIN:
    case SPA_NODE_COMMAND_MARKER:
    {
      PinosControlCmdNodeCommand cnc;

      /* send start */
      cnc.seq = this->seq++;
      cnc.command = command;
      pinos_connection_add_cmd (this->conn, PINOS_CONTROL_CMD_NODE_COMMAND, &cnc);

      if (!pinos_connection_flush (this->conn))
        spa_log_error (this->log, "proxy %p: error writing connection\n", this);

      res = SPA_RESULT_RETURN_ASYNC (cnc.seq);
      break;
    }

    case SPA_NODE_COMMAND_CLOCK_UPDATE:
    {
      PinosControlCmdNodeCommand cnc;

      /* send start */
      cnc.command = command;
      pinos_connection_add_cmd (this->conn, PINOS_CONTROL_CMD_NODE_COMMAND, &cnc);

      if (!pinos_connection_flush (this->conn))
        spa_log_error (this->log, "proxy %p: error writing connection\n", this);

      break;
    }
  }
  return res;
}

static SpaResult
spa_proxy_node_set_event_callback (SpaNode              *node,
                                   SpaNodeEventCallback  event,
                                   void                 *user_data)
{
  SpaProxy *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);
  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_get_n_ports (SpaNode       *node,
                            unsigned int  *n_input_ports,
                            unsigned int  *max_input_ports,
                            unsigned int  *n_output_ports,
                            unsigned int  *max_output_ports)
{
  SpaProxy *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (n_input_ports)
    *n_input_ports = this->n_inputs;
  if (max_input_ports)
    *max_input_ports = this->max_inputs;
  if (n_output_ports)
    *n_output_ports = this->n_outputs;
  if (max_output_ports)
    *max_output_ports = this->max_outputs;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_get_port_ids (SpaNode       *node,
                             unsigned int   n_input_ports,
                             uint32_t      *input_ids,
                             unsigned int   n_output_ports,
                             uint32_t      *output_ids)
{
  SpaProxy *this;
  int c, i;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (input_ids) {
    for (c = 0, i = 0; i < MAX_INPUTS && c < n_input_ports; i++) {
      if (this->in_ports[i].valid)
        input_ids[c++] = i;
    }
  }
  if (output_ids) {
    for (c = 0, i = 0; i < MAX_OUTPUTS && c < n_output_ports; i++) {
      if (this->out_ports[i].valid)
        output_ids[c++] = i;
    }
  }
  return SPA_RESULT_OK;
}

static void
do_update_port (SpaProxy                *this,
                PinosControlCmdPortUpdate *pu)
{
  SpaProxyPort *port;
  unsigned int i;
  size_t size;

  if (pu->direction == SPA_DIRECTION_INPUT) {
    port = &this->in_ports[pu->port_id];
  } else {
    port = &this->out_ports[pu->port_id];
  }

  if (pu->change_mask & PINOS_CONTROL_CMD_PORT_UPDATE_POSSIBLE_FORMATS) {
    for (i = 0; i < port->n_formats; i++)
      free (port->formats[i]);
    port->n_formats = pu->n_possible_formats;
    port->formats = realloc (port->formats, port->n_formats * sizeof (SpaFormat *));
    for (i = 0; i < port->n_formats; i++) {
      size = pinos_serialize_format_get_size (pu->possible_formats[i]);
      port->formats[i] = pinos_serialize_format_copy_into (malloc (size), pu->possible_formats[i]);
    }
  }
  if (pu->change_mask & PINOS_CONTROL_CMD_PORT_UPDATE_FORMAT) {
    if (port->format)
      free (port->format);
    size = pinos_serialize_format_get_size (pu->format);
    port->format = pinos_serialize_format_copy_into (malloc (size), pu->format);
  }

  if (pu->change_mask & PINOS_CONTROL_CMD_PORT_UPDATE_PROPS) {
  }

  if (pu->change_mask & PINOS_CONTROL_CMD_PORT_UPDATE_INFO && pu->info) {
    if (port->info)
      free (port->info);
    size = pinos_serialize_port_info_get_size (pu->info);
    port->info = pinos_serialize_port_info_copy_into (malloc (size), pu->info);
  }

  if (!port->valid) {
    spa_log_info (this->log, "proxy %p: adding port %d\n", this, pu->port_id);
    port->format = NULL;
    port->valid = true;

    if (pu->direction == SPA_DIRECTION_INPUT)
      this->n_inputs++;
    else
      this->n_outputs++;
  }
}

static void
clear_port (SpaProxy     *this,
            SpaProxyPort *port,
            SpaDirection  direction,
            uint32_t      port_id)
{
  PinosControlCmdPortUpdate pu;

  pu.change_mask = PINOS_CONTROL_CMD_PORT_UPDATE_POSSIBLE_FORMATS |
                   PINOS_CONTROL_CMD_PORT_UPDATE_FORMAT |
                   PINOS_CONTROL_CMD_PORT_UPDATE_PROPS |
                   PINOS_CONTROL_CMD_PORT_UPDATE_INFO;
  pu.direction = direction;
  pu.port_id = port_id;
  pu.n_possible_formats = 0;
  pu.possible_formats = NULL;
  pu.format = NULL;
  pu.props = NULL;
  pu.info = NULL;
  do_update_port (this, &pu);
  clear_buffers (this, port);
}

static void
do_uninit_port (SpaProxy     *this,
                SpaDirection  direction,
                uint32_t      port_id)
{
  SpaProxyPort *port;

  spa_log_info (this->log, "proxy %p: removing port %d\n", this, port_id);
  if (direction == SPA_DIRECTION_INPUT) {
    port = &this->in_ports[port_id];
    this->n_inputs--;
  } else {
    port = &this->out_ports[port_id];
    this->n_outputs--;
  }
  clear_port (this, port, direction, port_id);
  port->valid = false;
}

static SpaResult
spa_proxy_node_add_port (SpaNode        *node,
                         SpaDirection    direction,
                         uint32_t        port_id)
{
  SpaProxy *this;
  SpaProxyPort *port;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_FREE_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];
  clear_port (this, port, direction, port_id);

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_remove_port (SpaNode        *node,
                            SpaDirection    direction,
                            uint32_t        port_id)
{
  SpaProxy *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  do_uninit_port (this, direction, port_id);

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_enum_formats (SpaNode          *node,
                                  SpaDirection      direction,
                                  uint32_t          port_id,
                                  SpaFormat       **format,
                                  const SpaFormat  *filter,
                                  void            **state)
{
  SpaProxy *this;
  SpaProxyPort *port;
  int index;

  if (node == NULL || format == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  index = (*state == NULL ? 0 : *(int*)state);

  if (index >= port->n_formats)
    return SPA_RESULT_ENUM_END;

  *format = port->formats[index];
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_set_format (SpaNode            *node,
                                SpaDirection        direction,
                                uint32_t            port_id,
                                SpaPortFormatFlags  flags,
                                const SpaFormat    *format)
{
  SpaProxy *this;
  PinosControlCmdSetFormat sf;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  sf.seq = this->seq++;
  sf.direction = direction;
  sf.port_id = port_id;
  sf.flags = flags;
  sf.format = (SpaFormat *) format;
  pinos_connection_add_cmd (this->conn, PINOS_CONTROL_CMD_SET_FORMAT, &sf);

  if (!pinos_connection_flush (this->conn))
    spa_log_error (this->log, "proxy %p: error writing connection\n", this);

  return SPA_RESULT_RETURN_ASYNC (sf.seq);
}

static SpaResult
spa_proxy_node_port_get_format (SpaNode          *node,
                                SpaDirection      direction,
                                uint32_t          port_id,
                                const SpaFormat **format)
{
  SpaProxy *this;
  SpaProxyPort *port;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (!port->format)
    return SPA_RESULT_NO_FORMAT;

  *format = port->format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_get_info (SpaNode            *node,
                              SpaDirection        direction,
                              uint32_t            port_id,
                              const SpaPortInfo **info)
{
  SpaProxy *this;
  SpaProxyPort *port;

  if (node == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  *info = port->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_get_props (SpaNode        *node,
                               SpaDirection    direction,
                               uint32_t        port_id,
                               SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_proxy_node_port_set_props (SpaNode        *node,
                               SpaDirection    direction,
                               uint32_t        port_id,
                               const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_proxy_node_port_get_status (SpaNode              *node,
                                SpaDirection          direction,
                                uint32_t              port_id,
                                const SpaPortStatus **status)
{
  SpaProxy *this;
  SpaProxyPort *port;

  if (node == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (!port->format)
    return SPA_RESULT_NO_FORMAT;

  *status = &port->status;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_use_buffers (SpaNode         *node,
                                 SpaDirection     direction,
                                 uint32_t         port_id,
                                 SpaBuffer      **buffers,
                                 uint32_t         n_buffers)
{
  SpaProxy *this;
  SpaProxyPort *port;
  unsigned int i, j;
  PinosControlCmdAddMem am;
  PinosControlCmdUseBuffers ub;
  size_t size, n_mem;
  PinosControlMemRef *memref;
  void *p;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);
  spa_log_info (this->log, "proxy %p: use buffers %p %u\n", this, buffers, n_buffers);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (!port->format)
    return SPA_RESULT_NO_FORMAT;

  clear_buffers (this, port);

  /* find size to store buffers */
  size = 0;
  n_mem = 0;
  for (i = 0; i < n_buffers; i++) {
    ProxyBuffer *b = &port->buffers[i];

    b->outbuf = buffers[i];
    memcpy (&b->buffer, buffers[i], sizeof (SpaBuffer));
    b->buffer.datas = b->datas;
    b->buffer.metas = b->metas;

    b->size = pinos_serialize_buffer_get_size (buffers[i]);
    b->offset = size;

    for (j = 0; j < buffers[i]->n_metas; j++) {
      memcpy (&b->buffer.metas[j], &buffers[i]->metas[j], sizeof (SpaMeta));
    }

    for (j = 0; j < buffers[i]->n_datas; j++) {
      SpaData *d = &buffers[i]->datas[j];

      memcpy (&b->buffer.datas[j], d, sizeof (SpaData));

      switch (d->type) {
        case SPA_DATA_TYPE_DMABUF:
        case SPA_DATA_TYPE_MEMFD:
          am.direction = direction;
          am.port_id = port_id;
          am.mem_id = n_mem;
          am.type = d->type;
          am.fd_index = pinos_connection_add_fd (this->conn, d->fd, false);
          am.flags = d->flags;
          am.offset = d->offset;
          am.size = d->maxsize;
          pinos_connection_add_cmd (this->conn, PINOS_CONTROL_CMD_ADD_MEM, &am);

          b->buffer.datas[j].type = SPA_DATA_TYPE_ID;
          b->buffer.datas[j].data = SPA_UINT32_TO_PTR (n_mem);
          n_mem++;
          break;
        case SPA_DATA_TYPE_MEMPTR:
          b->buffer.datas[j].data = SPA_INT_TO_PTR (b->size);
          b->size += d->size;
          break;
        default:
          b->buffer.datas[j].type = SPA_DATA_TYPE_INVALID;
          b->buffer.datas[j].data = 0;
          spa_log_error (this->log, "invalid memory type %d\n", d->type);
          break;
      }
    }
    size += b->size;
  }

  if (n_buffers > 0) {
    /* make mem for the buffers */
    port->buffer_mem_id = n_mem++;
    port->buffer_mem_size = size;
    port->buffer_mem_fd = memfd_create ("spa-memfd", MFD_CLOEXEC | MFD_ALLOW_SEALING);

    if (ftruncate (port->buffer_mem_fd, size) < 0) {
      spa_log_error (this->log, "Failed to truncate temporary file: %s\n", strerror (errno));
      close (port->buffer_mem_fd);
      return SPA_RESULT_ERROR;
    }
#if 0
    {
      unsigned int seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
      if (fcntl (port->buffer_mem_fd, F_ADD_SEALS, seals) == -1) {
        spa_log_error (this->log, "Failed to add seals: %s\n", strerror (errno));
      }
    }
#endif
    p = port->buffer_mem_ptr = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, port->buffer_mem_fd, 0);

    for (i = 0; i < n_buffers; i++) {
      ProxyBuffer *b = &port->buffers[i];
      SpaBuffer *sb;
      SpaMeta *sbm;
      SpaData *sbd;

      pinos_serialize_buffer_serialize (p, &b->buffer);

      sb = p;
      b->buffer.datas = SPA_MEMBER (sb, SPA_PTR_TO_INT (sb->datas), SpaData);
      sbm = SPA_MEMBER (sb, SPA_PTR_TO_INT (sb->metas), SpaMeta);
      sbd = SPA_MEMBER (sb, SPA_PTR_TO_INT (sb->datas), SpaData);

      for (j = 0; j < b->buffer.n_metas; j++)
        b->metas[j].data = SPA_MEMBER (sb, SPA_PTR_TO_INT (sbm[j].data), void);

      for (j = 0; j < b->buffer.n_datas; j++) {
        if (b->datas[j].type == SPA_DATA_TYPE_MEMPTR)
          b->datas[j].data = SPA_MEMBER (sb, SPA_PTR_TO_INT (sbd[j].data), void);
      }
      p += b->size;
    }

    am.direction = direction;
    am.port_id = port_id;
    am.mem_id = port->buffer_mem_id;
    am.type = SPA_DATA_TYPE_MEMFD;
    am.fd_index = pinos_connection_add_fd (this->conn, port->buffer_mem_fd, false);
    am.flags = 0;
    am.offset = 0;
    am.size = size;
    pinos_connection_add_cmd (this->conn, PINOS_CONTROL_CMD_ADD_MEM, &am);

    memref = alloca (n_buffers * sizeof (PinosControlMemRef));
    for (i = 0; i < n_buffers; i++) {
      memref[i].mem_id = port->buffer_mem_id;
      memref[i].offset = port->buffers[i].offset;
      memref[i].size = port->buffers[i].size;
    }
  } else {
    memref = NULL;
  }
  port->n_buffers = n_buffers;

  ub.seq = this->seq++;
  ub.direction = direction;
  ub.port_id = port_id;
  ub.n_buffers = n_buffers;
  ub.buffers = memref;
  pinos_connection_add_cmd (this->conn, PINOS_CONTROL_CMD_USE_BUFFERS, &ub);

  if (!pinos_connection_flush (this->conn))
    spa_log_error (this->log, "proxy %p: error writing connection\n", this);

  return SPA_RESULT_RETURN_ASYNC (ub.seq);
}

static SpaResult
spa_proxy_node_port_alloc_buffers (SpaNode         *node,
                                   SpaDirection     direction,
                                   uint32_t         port_id,
                                   SpaAllocParam  **params,
                                   uint32_t         n_params,
                                   SpaBuffer      **buffers,
                                   uint32_t        *n_buffers)
{
  SpaProxy *this;
  SpaProxyPort *port;

  if (node == NULL || buffers == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (!port->format)
    return SPA_RESULT_NO_FORMAT;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static void
copy_meta_in (SpaProxy *this, SpaProxyPort *port, uint32_t buffer_id)
{
  ProxyBuffer *b = &port->buffers[buffer_id];
  unsigned int i;

  for (i = 0; i < b->outbuf->n_metas; i++) {
    SpaMeta *sm = &b->metas[i];
    SpaMeta *dm = &b->outbuf->metas[i];
    memcpy (dm->data, sm->data, dm->size);
  }
  for (i = 0; i < b->outbuf->n_datas; i++) {
    b->outbuf->datas[i].size = b->buffer.datas[i].size;
    if (b->outbuf->datas[i].type == SPA_DATA_TYPE_MEMPTR) {
      spa_log_info (this->log, "memcpy in %zd\n", b->buffer.datas[i].size);
      memcpy (b->outbuf->datas[i].data, b->datas[i].data, b->buffer.datas[i].size);
    }
  }
}

static void
copy_meta_out (SpaProxy *this, SpaProxyPort *port, uint32_t buffer_id)
{
  ProxyBuffer *b = &port->buffers[buffer_id];
  unsigned int i;

  for (i = 0; i < b->outbuf->n_metas; i++) {
    SpaMeta *sm = &b->outbuf->metas[i];
    SpaMeta *dm = &b->buffer.metas[i];
    memcpy (dm->data, sm->data, dm->size);
  }
  for (i = 0; i < b->outbuf->n_datas; i++) {
    b->buffer.datas[i].size = b->outbuf->datas[i].size;
    if (b->datas[i].type == SPA_DATA_TYPE_MEMPTR) {
      spa_log_info (this->log, "memcpy out %zd\n", b->outbuf->datas[i].size);
      memcpy (b->datas[i].data, b->outbuf->datas[i].data, b->outbuf->datas[i].size);
    }
  }
}

static SpaResult
spa_proxy_node_port_push_input (SpaNode          *node,
                                unsigned int      n_info,
                                SpaPortInputInfo *info)
{
  SpaProxy *this;
  SpaProxyPort *port;
  unsigned int i;
  bool have_error = false;
  bool have_enough = false;
  PinosControlCmdProcessBuffer pb;

  if (node == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  for (i = 0; i < n_info; i++) {
    if (!CHECK_IN_PORT (this, SPA_DIRECTION_INPUT, info[i].port_id)) {
      spa_log_warn (this->log, "invalid port %d\n", info[i].port_id);
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }
    port = &this->in_ports[info[i].port_id];

    if (!port->format) {
      info[i].status = SPA_RESULT_NO_FORMAT;
      have_error = true;
      continue;
    }
    if (info[i].buffer_id >= port->n_buffers) {
      if (port->n_buffers == 0)
        info[i].status = SPA_RESULT_NO_BUFFERS;
      else
        info[i].status = SPA_RESULT_INVALID_BUFFER_ID;
      have_error = true;
      continue;
    }

    copy_meta_out (this, port, info[i].buffer_id);

    pb.direction = SPA_DIRECTION_INPUT;
    pb.port_id = info[i].port_id;
    pb.buffer_id = info[i].buffer_id;
    pinos_connection_add_cmd (this->rtconn, PINOS_CONTROL_CMD_PROCESS_BUFFER, &pb);

    info[i].status = SPA_RESULT_OK;
  }

  if (have_error)
    return SPA_RESULT_ERROR;
  if (have_enough)
    return SPA_RESULT_HAVE_ENOUGH_INPUT;

  if (!pinos_connection_flush (this->rtconn))
    spa_log_error (this->log, "proxy %p: error writing connection\n", this);

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_pull_output (SpaNode           *node,
                                 unsigned int       n_info,
                                 SpaPortOutputInfo *info)
{
  SpaProxy *this;
  SpaProxyPort *port;
  unsigned int i;
  bool have_error = false;
  bool need_more = false;

  if (node == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  for (i = 0; i < n_info; i++) {
    if (!CHECK_OUT_PORT (this, SPA_DIRECTION_OUTPUT, info[i].port_id)) {
      spa_log_warn (this->log, "invalid port %u\n", info[i].port_id);
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }

    port = &this->out_ports[info[i].port_id];

    if (!port->format) {
      info[i].status = SPA_RESULT_NO_FORMAT;
      have_error = true;
      continue;
    }

    info[i].buffer_id = port->buffer_id;
    info[i].status = SPA_RESULT_OK;

    port->buffer_id = SPA_ID_INVALID;
  }
  if (have_error)
    return SPA_RESULT_ERROR;
  if (need_more)
    return SPA_RESULT_NEED_MORE_INPUT;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_reuse_buffer (SpaNode         *node,
                                  uint32_t         port_id,
                                  uint32_t         buffer_id)
{
  SpaProxy *this;
  PinosControlCmdNodeEvent cne;
  SpaNodeEvent ne;
  SpaNodeEventReuseBuffer rb;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  if (!CHECK_OUT_PORT (this, SPA_DIRECTION_OUTPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  /* send start */
  cne.event = &ne;
  ne.type = SPA_NODE_EVENT_TYPE_REUSE_BUFFER;
  ne.data = &rb;
  ne.size = sizeof (rb);
  rb.port_id = port_id;
  rb.buffer_id = buffer_id;
  pinos_connection_add_cmd (this->rtconn, PINOS_CONTROL_CMD_NODE_EVENT, &cne);

  if (!pinos_connection_flush (this->rtconn))
    spa_log_error (this->log, "proxy %p: error writing connection\n", this);

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_send_command (SpaNode        *node,
                                  SpaDirection    direction,
                                  uint32_t        port_id,
                                  SpaNodeCommand *command)
{
  SpaProxy *this;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaProxy, node);

  switch (command->type) {
    default:
      spa_log_warn (this->log, "unhandled command %d\n", command->type);
      break;
  }
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
handle_node_event (SpaProxy     *this,
                   SpaNodeEvent *event)
{
  switch (event->type) {
    case SPA_NODE_EVENT_TYPE_INVALID:
      break;

    case SPA_NODE_EVENT_TYPE_ASYNC_COMPLETE:
    case SPA_NODE_EVENT_TYPE_HAVE_OUTPUT:
    case SPA_NODE_EVENT_TYPE_NEED_INPUT:
    case SPA_NODE_EVENT_TYPE_REUSE_BUFFER:
    case SPA_NODE_EVENT_TYPE_ERROR:
    case SPA_NODE_EVENT_TYPE_BUFFERING:
    case SPA_NODE_EVENT_TYPE_REQUEST_REFRESH:
    case SPA_NODE_EVENT_TYPE_REQUEST_CLOCK_UPDATE:
      this->event_cb (&this->node, event, this->user_data);
      break;
  }

  return SPA_RESULT_OK;
}

static SpaResult
parse_connection (SpaProxy   *this)
{
  PinosConnection *conn = this->conn;

  while (pinos_connection_has_next (conn)) {
    PinosControlCmd cmd = pinos_connection_get_cmd (conn);

    switch (cmd) {
      case PINOS_CONTROL_CMD_INVALID:
      case PINOS_CONTROL_CMD_ADD_PORT:
      case PINOS_CONTROL_CMD_REMOVE_PORT:
      case PINOS_CONTROL_CMD_SET_FORMAT:
      case PINOS_CONTROL_CMD_SET_PROPERTY:
      case PINOS_CONTROL_CMD_NODE_COMMAND:
      case PINOS_CONTROL_CMD_PROCESS_BUFFER:
        spa_log_error (this->log, "proxy %p: got unexpected command %d\n", this, cmd);
        break;

      case PINOS_CONTROL_CMD_NODE_UPDATE:
      {
        PinosControlCmdNodeUpdate nu;

        if (!pinos_connection_parse_cmd (conn, &nu))
          break;

        if (nu.change_mask & PINOS_CONTROL_CMD_NODE_UPDATE_MAX_INPUTS)
          this->max_inputs = nu.max_input_ports;
        if (nu.change_mask & PINOS_CONTROL_CMD_NODE_UPDATE_MAX_OUTPUTS)
          this->max_outputs = nu.max_output_ports;

        spa_log_info (this->log, "proxy %p: got node update %d, max_in %u, max_out %u\n", this, cmd,
            this->max_inputs, this->max_outputs);

        break;
      }

      case PINOS_CONTROL_CMD_PORT_UPDATE:
      {
        PinosControlCmdPortUpdate pu;
        bool remove;

        spa_log_info (this->log, "proxy %p: got port update %d\n", this, cmd);
        if (!pinos_connection_parse_cmd (conn, &pu))
          break;

        if (!CHECK_PORT_ID (this, pu.direction, pu.port_id))
          break;

        remove = (pu.change_mask == 0);

        if (remove) {
          do_uninit_port (this, pu.direction, pu.port_id);
        } else {
          do_update_port (this, &pu);
        }
        break;
      }

      case PINOS_CONTROL_CMD_PORT_STATUS_CHANGE:
      {
        spa_log_warn (this->log, "proxy %p: command not implemented %d\n", this, cmd);
        break;
      }

      case PINOS_CONTROL_CMD_NODE_STATE_CHANGE:
      {
        PinosControlCmdNodeStateChange sc;
        SpaNodeState old = this->node.state;

        if (!pinos_connection_parse_cmd (conn, &sc))
          break;

        spa_log_info (this->log, "proxy %p: got node state change %d -> %d\n", this, old, sc.state);
        this->node.state = sc.state;
        if (old == SPA_NODE_STATE_INIT)
          send_async_complete (this, 0, SPA_RESULT_OK);

        break;
      }

      case PINOS_CONTROL_CMD_ADD_MEM:
        break;
      case PINOS_CONTROL_CMD_USE_BUFFERS:
        break;

      case PINOS_CONTROL_CMD_NODE_EVENT:
      {
        PinosControlCmdNodeEvent cne;

        if (!pinos_connection_parse_cmd (conn, &cne))
          break;

        handle_node_event (this, cne.event);
        break;
      }
    }
  }

  return SPA_RESULT_OK;
}

static SpaResult
parse_rtconnection (SpaProxy   *this)
{
  PinosConnection *conn = this->rtconn;

  while (pinos_connection_has_next (conn)) {
    PinosControlCmd cmd = pinos_connection_get_cmd (conn);

    switch (cmd) {
      case PINOS_CONTROL_CMD_INVALID:
      case PINOS_CONTROL_CMD_NODE_UPDATE:
      case PINOS_CONTROL_CMD_PORT_UPDATE:
      case PINOS_CONTROL_CMD_NODE_STATE_CHANGE:
      case PINOS_CONTROL_CMD_PORT_STATUS_CHANGE:
      case PINOS_CONTROL_CMD_ADD_PORT:
      case PINOS_CONTROL_CMD_REMOVE_PORT:
      case PINOS_CONTROL_CMD_SET_FORMAT:
      case PINOS_CONTROL_CMD_SET_PROPERTY:
      case PINOS_CONTROL_CMD_NODE_COMMAND:
      case PINOS_CONTROL_CMD_ADD_MEM:
      case PINOS_CONTROL_CMD_USE_BUFFERS:
        spa_log_error (this->log, "proxy %p: got unexpected connection %d\n", this, cmd);
        break;

      case PINOS_CONTROL_CMD_PROCESS_BUFFER:
      {
        PinosControlCmdProcessBuffer cmd;
        SpaProxyPort *port;

        if (!pinos_connection_parse_cmd (conn, &cmd))
          break;

        if (!CHECK_PORT (this, cmd.direction, cmd.port_id))
          break;

        port = cmd.direction == SPA_DIRECTION_INPUT ? &this->in_ports[cmd.port_id] : &this->out_ports[cmd.port_id];

        if (port->buffer_id != SPA_ID_INVALID)
          spa_log_warn (this->log, "proxy %p: unprocessed buffer: %d\n", this, port->buffer_id);

        copy_meta_in (this, port, cmd.buffer_id);

        port->buffer_id = cmd.buffer_id;
        break;
      }
      case PINOS_CONTROL_CMD_NODE_EVENT:
      {
        PinosControlCmdNodeEvent cne;

        if (!pinos_connection_parse_cmd (conn, &cne))
          break;

        handle_node_event (this, cne.event);
        break;
      }
    }
  }

  return SPA_RESULT_OK;
}

static int
proxy_on_fd_events (SpaPollNotifyData *data)
{
  SpaProxy *this = data->user_data;

  if (data->fds[0].revents & POLLIN) {
    parse_connection (this);
  }
  return 0;
}

static int
proxy_on_rtfd_events (SpaPollNotifyData *data)
{
  SpaProxy *this = data->user_data;

  if (data->fds[0].revents & POLLIN) {
    parse_rtconnection (this);
  }
  return 0;
}

static const SpaNode proxy_node = {
  sizeof (SpaNode),
  NULL,
  SPA_NODE_STATE_INIT,
  spa_proxy_node_get_props,
  spa_proxy_node_set_props,
  spa_proxy_node_send_command,
  spa_proxy_node_set_event_callback,
  spa_proxy_node_get_n_ports,
  spa_proxy_node_get_port_ids,
  spa_proxy_node_add_port,
  spa_proxy_node_remove_port,
  spa_proxy_node_port_enum_formats,
  spa_proxy_node_port_set_format,
  spa_proxy_node_port_get_format,
  spa_proxy_node_port_get_info,
  spa_proxy_node_port_get_props,
  spa_proxy_node_port_set_props,
  spa_proxy_node_port_use_buffers,
  spa_proxy_node_port_alloc_buffers,
  spa_proxy_node_port_get_status,
  spa_proxy_node_port_push_input,
  spa_proxy_node_port_pull_output,
  spa_proxy_node_port_reuse_buffer,
  spa_proxy_node_port_send_command,
};

static SpaResult
proxy_init (SpaProxy         *this,
            SpaDict          *info,
            const SpaSupport *support,
            unsigned int      n_support)
{
  unsigned int i;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].uri, SPA_LOG_URI) == 0)
      this->log = support[i].data;
    else if (strcmp (support[i].uri, SPA_POLL__MainLoop) == 0)
      this->main_loop = support[i].data;
    else if (strcmp (support[i].uri, SPA_POLL__DataLoop) == 0)
      this->data_loop = support[i].data;
  }
  if (this->data_loop == NULL) {
    spa_log_error (this->log, "a main-loop is needed");
  }
  if (this->data_loop == NULL) {
    spa_log_error (this->log, "a data-loop is needed");
  }

  this->node = proxy_node;

  this->fds[0].fd = -1;
  this->fds[0].events = POLLIN | POLLPRI | POLLERR;
  this->fds[0].revents = 0;
  this->poll.id = 0;
  this->poll.enabled = true;
  this->poll.fds = this->fds;
  this->poll.n_fds = 1;
  this->poll.idle_cb = NULL;
  this->poll.before_cb = NULL;
  this->poll.after_cb = proxy_on_fd_events;
  this->poll.user_data = this;

  this->rtfds[0].fd = -1;
  this->rtfds[0].events = POLLIN | POLLPRI | POLLERR;
  this->rtfds[0].revents = 0;
  this->rtpoll.id = 0;
  this->rtpoll.enabled = true;
  this->rtpoll.fds = this->rtfds;
  this->rtpoll.n_fds = 1;
  this->rtpoll.idle_cb = NULL;
  this->rtpoll.before_cb = NULL;
  this->rtpoll.after_cb = proxy_on_rtfd_events;
  this->rtpoll.user_data = this;

  return SPA_RESULT_OK;
}

static SpaResult
proxy_clear (SpaProxy *this)
{
  unsigned int i;

  for (i = 0; i < MAX_INPUTS; i++) {
    if (this->in_ports[i].valid)
      clear_port (this, &this->in_ports[i], SPA_DIRECTION_INPUT, i);
  }
  for (i = 0; i < MAX_OUTPUTS; i++) {
    if (this->out_ports[i].valid)
      clear_port (this, &this->out_ports[i], SPA_DIRECTION_OUTPUT, i);
  }
  if (this->fds[0].fd != -1)
    spa_poll_remove_item (this->main_loop, &this->poll);
  if (this->rtfds[0].fd != -1)
    spa_poll_remove_item (this->data_loop, &this->rtpoll);

  return SPA_RESULT_OK;
}

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

static void
pinos_client_node_dispose (GObject * object)
{
  PinosClientNode *this = PINOS_CLIENT_NODE (object);
  PinosClientNodePrivate *priv = this->priv;

  g_debug ("client-node %p: dispose", this);

  proxy_clear (priv->proxy);

  G_OBJECT_CLASS (pinos_client_node_parent_class)->dispose (object);
}

static void
pinos_client_node_finalize (GObject * object)
{
  PinosClientNode *this = PINOS_CLIENT_NODE (object);
  PinosClientNodePrivate *priv = this->priv;

  g_debug ("client-node %p: finalize", this);

  g_clear_object (&priv->sockets[0]);
  g_clear_object (&priv->sockets[1]);
  g_clear_object (&priv->rtsockets[0]);
  g_clear_object (&priv->rtsockets[1]);

  G_OBJECT_CLASS (pinos_client_node_parent_class)->finalize (object);
}

static void
pinos_client_node_constructed (GObject * object)
{
  PinosClientNode *this = PINOS_CLIENT_NODE (object);
  PinosClientNodePrivate *priv = this->priv;

  g_debug ("client-node %p: constructed", this);

  G_OBJECT_CLASS (pinos_client_node_parent_class)->constructed (object);


  priv->proxy = (SpaProxy *) PINOS_NODE (this)->node;
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

/**
 * pinos_client_node_new:
 * @daemon: a #PinosDaemon
 * @client: the client owner
 * @name: a name
 * @properties: extra properties
 *
 * Create a new #PinosNode.
 *
 * Returns: a new #PinosNode
 */
PinosNode *
pinos_client_node_new (PinosDaemon     *daemon,
                       PinosClient     *client,
                       const gchar     *name,
                       PinosProperties *properties)
{
  PinosNode *node;
  SpaProxy *p;

  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);

  p = g_malloc0 (sizeof (SpaProxy));
  proxy_init (p, NULL, daemon->support, daemon->n_support);

  node =  g_object_new (PINOS_TYPE_CLIENT_NODE,
                       "daemon", daemon,
                       "client", client,
                       "name", name,
                       "properties", properties,
                       "node", p,
                       NULL);

  return node;
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
  PinosClientNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_CLIENT_NODE (this), FALSE);
  priv = this->priv;

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

    priv->proxy->fds[0].fd = g_socket_get_fd (priv->sockets[0]);
    priv->proxy->conn = pinos_connection_new (priv->proxy->fds[0].fd);

    spa_poll_add_item (priv->proxy->main_loop, &priv->proxy->poll);

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

/**
 * pinos_client_node_get_rtsocket_pair:
 * @node: a #PinosClientNode
 * @error: a #GError
 *
 * Create or return a previously create socket pair for @node. The
 * Socket for the other end is returned.
 *
 * Returns: a #GSocket that can be used to send/receive buffers to node.
 */
GSocket *
pinos_client_node_get_rtsocket_pair (PinosClientNode  *this,
                                     GError          **error)
{
  PinosClientNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_CLIENT_NODE (this), FALSE);
  priv = this->priv;

  if (priv->rtsockets[1] == NULL) {
    int fd[2];

    if (socketpair (AF_UNIX, SOCK_STREAM, 0, fd) != 0)
      goto no_sockets;

    priv->rtsockets[0] = g_socket_new_from_fd (fd[0], error);
    if (priv->rtsockets[0] == NULL)
      goto create_failed;

    priv->rtsockets[1] = g_socket_new_from_fd (fd[1], error);
    if (priv->rtsockets[1] == NULL)
      goto create_failed;

    priv->proxy->rtfds[0].fd = g_socket_get_fd (priv->rtsockets[0]);
    priv->proxy->rtconn = pinos_connection_new (priv->proxy->rtfds[0].fd);

    spa_poll_add_item (priv->proxy->data_loop, &priv->proxy->rtpoll);
  }
  return g_object_ref (priv->rtsockets[1]);

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
    g_clear_object (&priv->rtsockets[0]);
    g_clear_object (&priv->rtsockets[1]);
    return NULL;
  }
}
