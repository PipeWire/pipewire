/* Spa
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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
#include <stddef.h>
#include <stdio.h>
#include <poll.h>
#include <sys/socket.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>


#include <spa/node.h>
#include <spa/control.h>

#define MAX_INPUTS       64
#define MAX_OUTPUTS      64
#define MAX_PORTS        (MAX_INPUTS + MAX_OUTPUTS)

#define CHECK_PORT_ID(this,id)  ((id) < MAX_PORTS && (this)->ports[id].valid)
#define CHECK_PORT_ID_DIR(this,id,dir)  (CHECK_PORT_ID(this,id) && (this)->ports[i].direction == (dir))

typedef struct _SpaProxy SpaProxy;

typedef struct {
  SpaProps props;
  int socketfd;
} SpaProxyProps;

typedef struct {
  SpaDirection direction;
  bool valid;
  bool have_format;
  SpaPortInfo info;
  SpaPortStatus status;
  SpaFormat formats[2];
  SpaBuffer **buffers;
  unsigned int n_buffers;
} SpaProxyPort;

struct _SpaProxy {
  SpaHandle  handle;
  SpaNode    node;

  SpaProxyProps props[2];

  SpaEventCallback event_cb;
  void *user_data;

  SpaPollFd fds[1];
  SpaPollItem poll;

  unsigned int n_inputs;
  unsigned int n_outputs;
  SpaProxyPort ports[MAX_PORTS];
};

enum {
  PROP_ID_SOCKET,
  PROP_ID_LAST,
};

static const SpaPropInfo prop_info[PROP_ID_LAST] =
{
  { PROP_ID_SOCKET,            "socket", "The Socket factor",
                               SPA_PROP_FLAG_READWRITE,
                               SPA_PROP_TYPE_INT, sizeof (int),
                               sizeof (int), NULL,
                               SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                               NULL,
                               offsetof (SpaProxyProps, socketfd),
                               0, 0,
                               NULL },
};

static void
reset_proxy_props (SpaProxyProps *props)
{
  props->socketfd = -1;
}

static void
update_poll (SpaProxy *this, int socketfd)
{
  SpaEvent event;
  SpaProxyProps *p;

  p = &this->props[1];

  if (p->socketfd != -1) {
    event.refcount = 1;
    event.notify = NULL;
    event.type = SPA_EVENT_TYPE_REMOVE_POLL;
    event.port_id = 0;
    event.data = &this->poll;
    event.size = sizeof (this->poll);
    this->event_cb (&this->node, &event, this->user_data);
  }
  p->socketfd = socketfd;

  if (p->socketfd != -1) {
    this->fds[0].fd = p->socketfd;
    event.refcount = 1;
    event.notify = NULL;
    event.type = SPA_EVENT_TYPE_ADD_POLL;
    event.port_id = 0;
    event.data = &this->poll;
    event.size = sizeof (this->poll);
    this->event_cb (&this->node, &event, this->user_data);
  }
}

static SpaResult
spa_proxy_node_get_props (SpaNode       *node,
                          SpaProps     **props)
{
  SpaProxy *this;

  if (node == NULL || node->handle == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));
  *props = &this->props[0].props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_set_props (SpaNode         *node,
                          const SpaProps  *props)
{
  SpaProxy *this;
  SpaProxyProps *op, *np;
  SpaResult res;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  op = &this->props[1];
  np = &this->props[0];

  if (props == NULL) {
    reset_proxy_props (np);
    props = &np->props;
  }

  /* copy new properties */
  res = spa_props_copy (props, &np->props);

  /* compare changes */
  if (op->socketfd != np->socketfd)
    update_poll (this, np->socketfd);

  /* commit changes */
  memcpy (op, np, sizeof (*np));

  return res;
}

static SpaResult
spa_proxy_node_send_command (SpaNode       *node,
                             SpaCommand    *command)
{
  SpaProxy *this;

  if (node == NULL || node->handle == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  switch (command->type) {
    case SPA_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_COMMAND_START:
      if (this->event_cb) {
        SpaEvent event;

        event.refcount = 1;
        event.notify = NULL;
        event.type = SPA_EVENT_TYPE_STARTED;
        event.port_id = -1;
        event.data = NULL;
        event.size = 0;

        this->event_cb (&this->node, &event, this->user_data);
      }
      break;

    case SPA_COMMAND_STOP:
      if (this->event_cb) {
        SpaEvent event;

        event.refcount = 1;
        event.notify = NULL;
        event.type = SPA_EVENT_TYPE_STOPPED;
        event.port_id = -1;
        event.data = NULL;
        event.size = 0;

        this->event_cb (&this->node, &event, this->user_data);
      }
      break;

    case SPA_COMMAND_FLUSH:
    case SPA_COMMAND_DRAIN:
    case SPA_COMMAND_MARKER:
      return SPA_RESULT_NOT_IMPLEMENTED;
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_set_event_callback (SpaNode          *node,
                                   SpaEventCallback  event,
                                   void             *user_data)
{
  SpaProxy *this;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;
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

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  if (n_input_ports)
    *n_input_ports = this->n_inputs;
  if (n_output_ports)
    *n_output_ports = this->n_outputs;
  if (max_input_ports)
    *max_input_ports = MAX_INPUTS;
  if (max_output_ports)
    *max_output_ports = MAX_OUTPUTS;

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

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  if (input_ids) {
    n_input_ports = SPA_MIN (n_input_ports, MAX_PORTS);
    for (c = 0, i = 0; i < n_input_ports; i++) {
      if (this->ports[i].valid && this->ports[i].direction == SPA_DIRECTION_INPUT)
        input_ids[c++] = i;
    }
  }
  if (output_ids) {
    n_output_ports = SPA_MIN (n_output_ports, MAX_PORTS);
    for (c = 0, i = 0; i < n_output_ports; i++) {
      if (this->ports[i].valid && this->ports[i].direction == SPA_DIRECTION_OUTPUT)
        output_ids[c++] = i;
    }
  }
  return SPA_RESULT_OK;
}


static SpaResult
spa_proxy_node_add_port (SpaNode        *node,
                         SpaDirection    direction,
                         uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_proxy_node_remove_port (SpaNode        *node,
                            uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_proxy_node_port_enum_formats (SpaNode          *node,
                                  uint32_t          port_id,
                                  SpaFormat       **format,
                                  const SpaFormat  *filter,
                                  void            **state)
{
  SpaProxy *this;
  SpaProxyPort *port;
  int index;

  if (node == NULL || node->handle == NULL || format == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  if (!CHECK_PORT_ID (this, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = &port->formats[0];
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_set_format (SpaNode            *node,
                                uint32_t            port_id,
                                SpaPortFormatFlags  flags,
                                const SpaFormat    *format)
{
  SpaProxy *this;
  SpaProxyPort *port;
  SpaControl control;
  SpaControlBuilder builder;
  SpaControlCmdSetFormat sf;
  uint8_t buf[128];
  SpaResult res;

  if (node == NULL || node->handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this  = (SpaProxy *) node->handle;

  if (!CHECK_PORT_ID (this, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];

  spa_control_builder_init_into (&builder, buf, sizeof (buf), NULL, 0);
  sf.port = port_id;
  sf.format = format;
  spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_SET_FORMAT, &sf);
  spa_control_builder_end (&builder, &control);

  if ((res = spa_control_write (&control, this->fds[0].fd)) < 0)
    fprintf (stderr, "proxy %p: error writing control", this);

  port->have_format = format != NULL;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_get_format (SpaNode          *node,
                                uint32_t          port_id,
                                const SpaFormat **format)
{
  SpaProxy *this;
  SpaProxyPort *port;

  if (node == NULL || node->handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  if (!CHECK_PORT_ID (this, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = &port->formats[1];

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_get_info (SpaNode            *node,
                              uint32_t            port_id,
                              const SpaPortInfo **info)
{
  SpaProxy *this;
  SpaProxyPort *port;

  if (node == NULL || node->handle == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  if (!CHECK_PORT_ID (this, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];

  *info = &port->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_get_props (SpaNode    *node,
                               uint32_t    port_id,
                               SpaProps **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_proxy_node_port_set_props (SpaNode        *node,
                               uint32_t        port_id,
                               const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_proxy_node_port_get_status (SpaNode              *node,
                                uint32_t              port_id,
                                const SpaPortStatus **status)
{
  SpaProxy *this;
  SpaProxyPort *port;

  if (node == NULL || node->handle == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  if (!CHECK_PORT_ID (this, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  *status = &port->status;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_use_buffers (SpaNode         *node,
                                 uint32_t         port_id,
                                 SpaBuffer      **buffers,
                                 uint32_t         n_buffers)
{
  SpaProxy *this;
  SpaProxyPort *port;

  if (node == NULL || node->handle == NULL || buffers == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  if (!CHECK_PORT_ID (this, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  port->buffers = buffers;
  port->n_buffers = n_buffers;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_alloc_buffers (SpaNode         *node,
                                   uint32_t         port_id,
                                   SpaAllocParam  **params,
                                   uint32_t         n_params,
                                   SpaBuffer      **buffers,
                                   uint32_t        *n_buffers)
{
  SpaProxy *this;
  SpaProxyPort *port;

  if (node == NULL || node->handle == NULL || buffers == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  if (!CHECK_PORT_ID (this, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  return SPA_RESULT_OK;
}

static int
tmpfile_create (void *data, size_t size)
{
  char filename[] = "/dev/shm/tmpfilepay.XXXXXX";
  int fd;

  fd = mkostemp (filename, O_CLOEXEC);
  if (fd == -1) {
    fprintf (stderr, "Failed to create temporary file: %s", strerror (errno));
    return -1;
  }
  unlink (filename);

  if (write (fd, data, size) != (ssize_t) size)
    fprintf (stderr, "Failed to write data: %s", strerror (errno));

  return fd;
}

typedef struct {
  SpaBuffer buffer;
  SpaData  datas[16];
  int idx[16];
  SpaBuffer *orig;
} MyBuffer;

static SpaResult
send_buffer (SpaProxy *this, SpaBuffer *buffer)
{
  SpaControl control;
  SpaControlBuilder builder;
  uint8_t buf[1024];
  int fds[16];
  SpaControlCmdAddBuffer ab;
  SpaControlCmdProcessBuffer pb;
  SpaControlCmdRemoveBuffer rb;
  bool tmpfile = false;
  unsigned int i;
  MyBuffer b;
  SpaResult res;

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

  if ((res = spa_control_write (&control, this->fds[0].fd)) < 0)
    fprintf (stderr, "proxy %p: error writing control", this);

  spa_control_clear (&control);

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_push_input (SpaNode        *node,
                                unsigned int    n_info,
                                SpaInputInfo   *info)
{
  SpaProxy *this;
  SpaProxyPort *port;
  unsigned int i;
  bool have_error = false;
  bool have_enough = false;

  if (node == NULL || node->handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  for (i = 0; i < n_info; i++) {
    if (!CHECK_PORT_ID_DIR (this, info[i].port_id, SPA_DIRECTION_INPUT)) {
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }
    port = &this->ports[info[i].port_id];

    if (!port->have_format) {
      info[i].status = SPA_RESULT_NO_FORMAT;
      have_error = true;
      continue;
    }

    info[i].status = SPA_RESULT_OK;
  }
  if (have_error)
    return SPA_RESULT_ERROR;
  if (have_enough)
    return SPA_RESULT_HAVE_ENOUGH_INPUT;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_pull_output (SpaNode        *node,
                                 unsigned int    n_info,
                                 SpaOutputInfo  *info)
{
  SpaProxy *this;
  SpaProxyPort *port;
  unsigned int i;
  bool have_error = false;
  bool need_more = false;

  if (node == NULL || node->handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  for (i = 0; i < n_info; i++) {
    if (!CHECK_PORT_ID_DIR (this, info[i].port_id, SPA_DIRECTION_OUTPUT)) {
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }

    port = &this->ports[info[i].port_id];

    if (!port->have_format) {
      info[i].status = SPA_RESULT_NO_FORMAT;
      have_error = true;
      continue;
    }
  }
  if (have_error)
    return SPA_RESULT_ERROR;
  if (need_more)
    return SPA_RESULT_NEED_MORE_INPUT;

  return SPA_RESULT_OK;
}

static SpaResult
parse_control (SpaProxy   *this,
               SpaControl *ctrl)
{
  SpaControlIter it;
  SpaResult res;

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
        fprintf (stderr, "proxy %p: got unexpected control %d", this, cmd);
        break;

      case SPA_CONTROL_CMD_NODE_UPDATE:
      case SPA_CONTROL_CMD_PORT_UPDATE:
      case SPA_CONTROL_CMD_PORT_REMOVED:
        fprintf (stderr, "proxy %p: command not implemented %d", this, cmd);
        break;

      case SPA_CONTROL_CMD_START_CONFIGURE:
      {
        SpaControlBuilder builder;
        SpaControl control;
        uint8_t buf[128];

        /* set port format */

        /* send end-configure */
        spa_control_builder_init_into (&builder, buf, sizeof (buf), NULL, 0);
        spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_END_CONFIGURE, NULL);
        spa_control_builder_end (&builder, &control);

        if ((res = spa_control_write (&control, this->fds[0].fd)) < 0)
          fprintf (stderr, "proxy %p: error writing control: %d", this, res);
        break;
      }
      case SPA_CONTROL_CMD_PORT_STATUS_CHANGE:
      {
        fprintf (stderr, "proxy %p: command not implemented %d", this, cmd);
        break;
      }
      case SPA_CONTROL_CMD_START_ALLOC:
      {
        SpaControlBuilder builder;
        SpaControl control;
        uint8_t buf[128];

        /* FIXME read port memory requirements */
        /* FIXME add_mem */

        /* send start */
        spa_control_builder_init_into (&builder, buf, sizeof (buf), NULL, 0);
        spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_START, NULL);
        spa_control_builder_end (&builder, &control);

        if ((res = spa_control_write (&control, this->fds[0].fd)) < 0)
          fprintf (stderr, "proxy %p: error writing control %d", this, res);
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
        fprintf (stderr, "proxy %p: command unhandled %d", this, cmd);
        break;
    }
  }
  spa_control_iter_end (&it);

  return SPA_RESULT_OK;
}

static int
proxy_on_fd_events (SpaPollNotifyData *data)
{
  SpaProxy *this = data->user_data;
  SpaResult res;

  if (data->fds[0].revents & POLLIN) {
    SpaControl control;
    uint8_t buf[1024];
    int fds[16];

    if ((res = spa_control_read (&control, data->fds[0].fd, buf, 1024, fds, 16)) < 0) {
      fprintf (stderr, "proxy %p: failed to read control: %d", this, res);
      return 0;
    }
    parse_control (this, &control);
    spa_control_clear (&control);
  }
  return 0;
}

static const SpaNode proxy_node = {
  NULL,
  sizeof (SpaNode),
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
};

static SpaResult
spa_proxy_get_interface (SpaHandle               *handle,
                         uint32_t                 interface_id,
                         void                   **interface)
{
  SpaProxy *this = (SpaProxy *) handle;

  if (handle == NULL || interface == 0)
    return SPA_RESULT_INVALID_ARGUMENTS;

  switch (interface_id) {
    case SPA_INTERFACE_ID_NODE:
      *interface = &this->node;
      break;
    default:
      return SPA_RESULT_UNKNOWN_INTERFACE;

  }
  return SPA_RESULT_OK;
}

static SpaResult
proxy_instantiate (const SpaHandleFactory  *factory,
                   SpaHandle               *handle)
{
  SpaProxy *this;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_proxy_get_interface;

  this = (SpaProxy *) handle;
  this->node = proxy_node;
  this->node.handle = handle;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  this->props[1].props.set_prop = spa_props_generic_set_prop;
  this->props[1].props.get_prop = spa_props_generic_get_prop;
  reset_proxy_props (&this->props[1]);
  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));

  this->fds[0].fd = -1;
  this->fds[0].events = POLLIN | POLLPRI | POLLERR;
  this->fds[0].revents = 0;
  this->poll.id = 0;
  this->poll.fds = this->fds;
  this->poll.n_fds = 1;
  this->poll.idle_cb = NULL;
  this->poll.before_cb = NULL;
  this->poll.after_cb = proxy_on_fd_events;
  this->poll.user_data = this;

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo proxy_interfaces[] =
{
  { SPA_INTERFACE_ID_NODE,
    SPA_INTERFACE_ID_NODE_NAME,
    SPA_INTERFACE_ID_NODE_DESCRIPTION,
  },
};

static SpaResult
proxy_enum_interface_info (const SpaHandleFactory  *factory,
                           const SpaInterfaceInfo **info,
                           void                   **state)
{
  int index;

  if (factory == NULL || info == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      *info = &proxy_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_proxy_factory =
{ "proxy",
  NULL,
  sizeof (SpaProxy),
  proxy_instantiate,
  proxy_enum_interface_info,
};
