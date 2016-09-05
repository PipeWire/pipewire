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

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <poll.h>
#include <sys/socket.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>


#include <spa/control.h>
#include <spa/debug.h>
#include <spa/memory.h>
#include <spa/node.h>

#define MAX_INPUTS       64
#define MAX_OUTPUTS      64
#define MAX_PORTS        (MAX_INPUTS + MAX_OUTPUTS)

#define CHECK_FREE_PORT_ID(this,id)     ((id) < MAX_PORTS && !(this)->ports[id].valid)
#define CHECK_PORT_ID(this,id)          ((id) < MAX_PORTS && (this)->ports[id].valid)
#define CHECK_PORT_ID_IN(this,id)       (CHECK_PORT_ID(this,id) && (id < this->max_inputs))
#define CHECK_PORT_ID_OUT(this,id)      (CHECK_PORT_ID(this,id) && (id >= this->max_inputs))

typedef struct _SpaProxy SpaProxy;

typedef struct {
  SpaProps props;
  int socketfd;
} SpaProxyProps;

typedef struct {
  bool           valid;
  SpaPortInfo    info;
  SpaFormat     *format;
  unsigned int   n_formats;
  SpaFormat    **formats;
  SpaPortStatus  status;
  unsigned int   n_buffers;
  SpaBuffer    **buffers;
  uint32_t       buffer_id;
} SpaProxyPort;

struct _SpaProxy {
  SpaHandle  handle;
  SpaNode    node;

  SpaProxyProps props[2];

  SpaEventCallback event_cb;
  void *user_data;

  SpaPollFd fds[1];
  SpaPollItem poll;

  unsigned int max_inputs;
  unsigned int n_inputs;
  unsigned int max_outputs;
  unsigned int n_outputs;
  SpaProxyPort ports[MAX_PORTS];
};

enum {
  PROP_ID_SOCKET,
  PROP_ID_LAST,
};

static const SpaPropInfo prop_info[PROP_ID_LAST] =
{
  { PROP_ID_SOCKET,            offsetof (SpaProxyProps, socketfd),
                               "socket", "The Socket factor",
                               SPA_PROP_FLAG_READWRITE,
                               SPA_PROP_TYPE_INT, sizeof (int),
                               SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
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
    event.type = SPA_EVENT_TYPE_REMOVE_POLL;
    event.data = &this->poll;
    event.size = sizeof (this->poll);
    this->event_cb (&this->node, &event, this->user_data);
  }
  p->socketfd = socketfd;

  if (p->socketfd != -1) {
    this->fds[0].fd = p->socketfd;
    event.type = SPA_EVENT_TYPE_ADD_POLL;
    event.data = &this->poll;
    event.size = sizeof (this->poll);
    this->event_cb (&this->node, &event, this->user_data);
  }
}

static SpaResult
update_state (SpaProxy *this, SpaNodeState state)
{
  if (this->node.state != state) {
    SpaEvent event;
    SpaEventStateChange sc;

    this->node.state = state;

    event.type = SPA_EVENT_TYPE_STATE_CHANGE;
    event.data = &sc;
    event.size = sizeof (sc);
    sc.state = state;
    this->event_cb (&this->node, &event, this->user_data);
  }
  return SPA_RESULT_OK;
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
  SpaResult res;

  if (node == NULL || node->handle == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  switch (command->type) {
    case SPA_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_COMMAND_START:
    {
      SpaControlBuilder builder;
      SpaControl control;
      uint8_t buf[128];

      /* send start */
      spa_control_builder_init_into (&builder, buf, sizeof (buf), NULL, 0);
      spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_START, NULL);
      spa_control_builder_end (&builder, &control);

      if ((res = spa_control_write (&control, this->fds[0].fd)) < 0)
        fprintf (stderr, "proxy %p: error writing control %d\n", this, res);

      spa_control_clear (&control);
      break;
    }

    case SPA_COMMAND_PAUSE:
    {
      SpaControlBuilder builder;
      SpaControl control;
      uint8_t buf[128];

      /* send start */
      spa_control_builder_init_into (&builder, buf, sizeof (buf), NULL, 0);
      spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_PAUSE, NULL);
      spa_control_builder_end (&builder, &control);

      if ((res = spa_control_write (&control, this->fds[0].fd)) < 0)
        fprintf (stderr, "proxy %p: error writing control %d\n", this, res);

      spa_control_clear (&control);
      break;
    }

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
    *max_input_ports = this->max_inputs;
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

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  if (input_ids) {
    for (c = 0, i = 0; i < MAX_INPUTS && c < n_input_ports; i++) {
      if (this->ports[i].valid)
        input_ids[c++] = i;
    }
  }
  if (output_ids) {
    for (c = 0, i = 0; i < MAX_OUTPUTS && c < n_output_ports; i++) {
      if (this->ports[MAX_INPUTS + i].valid)
        output_ids[c++] = MAX_INPUTS + i;
    }
  }
  return SPA_RESULT_OK;
}

static void
do_update_port (SpaProxy                *this,
                SpaControlCmdPortUpdate *pu)
{
  SpaEvent event;
  SpaProxyPort *port;
  SpaEventPortAdded pa;
  unsigned int i;

  port = &this->ports[pu->port_id];

  if (pu->change_mask & SPA_CONTROL_CMD_PORT_UPDATE_POSSIBLE_FORMATS) {
    port->n_formats = pu->n_possible_formats;
    port->formats = pu->possible_formats;

    for (i = 0; i < port->n_formats; i++)
      spa_debug_format (port->formats[i]);
  }

  if (pu->change_mask & SPA_CONTROL_CMD_PORT_UPDATE_PROPS) {
  }

  if (pu->change_mask & SPA_CONTROL_CMD_PORT_UPDATE_INFO) {
    port->info = *pu->info;
  }

  if (!port->valid) {
    fprintf (stderr, "proxy %p: adding port %d\n", this, pu->port_id);
    port->format = NULL;
    port->valid = true;

    if (pu->port_id < MAX_INPUTS)
      this->n_inputs++;
    else
      this->n_outputs++;

    event.type = SPA_EVENT_TYPE_PORT_ADDED;
    event.size = sizeof (pa);
    event.data = &pa;
    pa.port_id = pu->port_id;
    this->event_cb (&this->node, &event, this->user_data);
  }
}

static void
do_uninit_port (SpaProxy     *this,
                uint32_t      port_id)
{
  SpaEvent event;
  SpaProxyPort *port;
  SpaEventPortRemoved pr;

  fprintf (stderr, "proxy %p: removing port %d\n", this, port_id);
  port = &this->ports[port_id];

  if (port_id < MAX_INPUTS)
    this->n_inputs--;
  else
    this->n_outputs--;

  port->valid = false;
  if (port->format)
    spa_format_unref (port->format);
  port->format = NULL;

  event.type = SPA_EVENT_TYPE_PORT_REMOVED;
  event.size = sizeof (pr);
  event.data = &pr;
  pr.port_id = port_id;
  this->event_cb (&this->node, &event, this->user_data);
}

static SpaResult
spa_proxy_node_add_port (SpaNode        *node,
                         uint32_t        port_id)
{
  SpaProxy *this;
  SpaControlCmdPortUpdate pu;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  if (!CHECK_FREE_PORT_ID (this, port_id))
    return SPA_RESULT_INVALID_PORT;

  pu.change_mask = SPA_CONTROL_CMD_PORT_UPDATE_POSSIBLE_FORMATS |
                   SPA_CONTROL_CMD_PORT_UPDATE_PROPS |
                   SPA_CONTROL_CMD_PORT_UPDATE_INFO;
  pu.port_id = port_id;
  pu.n_possible_formats = 0;
  pu.possible_formats = NULL;
  pu.props = NULL;
  pu.info = NULL;
  do_update_port (this, &pu);

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_remove_port (SpaNode        *node,
                            uint32_t        port_id)
{
  SpaProxy *this;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  if (!CHECK_PORT_ID (this, port_id))
    return SPA_RESULT_INVALID_PORT;

  do_uninit_port (this, port_id);

  return SPA_RESULT_OK;
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

  if (index >= port->n_formats)
    return SPA_RESULT_ENUM_END;

  *format = port->formats[index];
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

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this  = (SpaProxy *) node->handle;

  if (!CHECK_PORT_ID (this, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];

  spa_control_builder_init_into (&builder, buf, sizeof (buf), NULL, 0);
  sf.port_id = port_id;
  sf.format = (SpaFormat *) format;
  spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_SET_FORMAT, &sf);
  spa_control_builder_end (&builder, &control);

  if ((res = spa_control_write (&control, this->fds[0].fd)) < 0)
    fprintf (stderr, "proxy %p: error writing control\n", this);

  spa_control_clear (&control);

  if (port->format)
    spa_format_unref (port->format);
  if (format)
    spa_format_ref ((SpaFormat *) format);
  port->format = (SpaFormat *)format;

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

  if (!port->format)
    return SPA_RESULT_NO_FORMAT;

  *format = port->format;

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

  if (!port->format)
    return SPA_RESULT_NO_FORMAT;

  *status = &port->status;

  return SPA_RESULT_OK;
}

static SpaResult
add_buffer_mem (SpaProxy *this, SpaControlBuilder *builder, uint32_t port_id, SpaBuffer *buffer)
{
  SpaControlCmdAddMem am;
  int i;
  SpaBuffer *b;
  SpaMemory *bmem;

  if (buffer->mem.mem.id == SPA_ID_INVALID) {
    fprintf (stderr, "proxy %p: alloc buffer space %zd\n", this, buffer->mem.size);
    bmem = spa_memory_alloc_with_fd (SPA_MEMORY_POOL_SHARED, buffer, buffer->mem.size);
    b = spa_memory_ensure_ptr (bmem);
    b->mem.mem = bmem->mem;
    b->mem.offset = 0;
  } else {
    bmem = spa_memory_find (&buffer->mem.mem);
    b = buffer;
  }

  am.port_id = port_id;
  am.mem = bmem->mem;
  am.mem_type = 0;
  am.fd_index = spa_control_builder_add_fd (builder, bmem->fd, false);
  am.flags = bmem->flags;
  am.size = bmem->size;
  spa_control_builder_add_cmd (builder, SPA_CONTROL_CMD_ADD_MEM, &am);

  for (i = 0; i < b->n_datas; i++) {
    SpaData *d = &SPA_BUFFER_DATAS (b)[i];
    SpaMemory *mem;

    if (!(mem = spa_memory_find (&d->mem.mem))) {
      fprintf (stderr, "proxy %p: error invalid memory\n", this);
      continue;
    }
    if (mem->fd == -1) {
      fprintf (stderr, "proxy %p: error memory without fd\n", this);
      continue;
    }

    am.port_id = port_id;
    am.mem = mem->mem;
    am.mem_type = 0;
    am.fd_index = spa_control_builder_add_fd (builder, mem->fd, false);
    am.flags = mem->flags;
    am.size = mem->size;
    spa_control_builder_add_cmd (builder, SPA_CONTROL_CMD_ADD_MEM, &am);
  }

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
  unsigned int i;
  SpaControl control;
  SpaControlBuilder builder;
  uint8_t buf[4096];
  int fds[32];
  SpaResult res;
  SpaControlCmdUseBuffers ub;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;
  fprintf (stderr, "proxy %p: use buffers %p %u\n", this, buffers, n_buffers);

  if (!CHECK_PORT_ID (this, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];

  if (!port->format)
    return SPA_RESULT_NO_FORMAT;

  spa_control_builder_init_into (&builder, buf, sizeof (buf), fds, sizeof (fds));

  if (buffers == NULL || n_buffers == 0) {
    port->buffers = NULL;
    port->n_buffers = 0;
  } else {
    port->buffers = buffers;
    port->n_buffers = n_buffers;
  }
  for (i = 0; i < port->n_buffers; i++)
    add_buffer_mem (this, &builder, port_id, port->buffers[i]);

  ub.port_id = port_id;
  ub.n_buffers = port->n_buffers;
  ub.buffers = port->buffers;
  spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_USE_BUFFERS, &ub);

  spa_control_builder_end (&builder, &control);

  if ((res = spa_control_write (&control, this->fds[0].fd)) < 0)
    fprintf (stderr, "proxy %p: error writing control\n", this);

  spa_control_clear (&control);

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

  if (!port->format)
    return SPA_RESULT_NO_FORMAT;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_proxy_node_port_reuse_buffer (SpaNode         *node,
                                  uint32_t         port_id,
                                  uint32_t         buffer_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
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
  SpaControl control;
  SpaControlBuilder builder;
  SpaControlCmdProcessBuffer pb;
  uint8_t buf[64];
  SpaResult res;

  if (node == NULL || node->handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  spa_control_builder_init_into (&builder, buf, sizeof(buf), NULL, 0);

  for (i = 0; i < n_info; i++) {
    if (!CHECK_PORT_ID_IN (this, info[i].port_id)) {
      printf ("invalid port %d\n", info[i].port_id);
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }
    port = &this->ports[info[i].port_id];

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

    pb.port_id = info[i].port_id;
    pb.buffer_id = info[i].buffer_id;
    spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_PROCESS_BUFFER, &pb);

    info[i].status = SPA_RESULT_OK;
  }
  spa_control_builder_end (&builder, &control);

  if (have_error)
    return SPA_RESULT_ERROR;
  if (have_enough)
    return SPA_RESULT_HAVE_ENOUGH_INPUT;

  if ((res = spa_control_write (&control, this->fds[0].fd)) < 0)
    fprintf (stderr, "proxy %p: error writing control\n", this);

  spa_control_clear (&control);

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
    if (!CHECK_PORT_ID_OUT (this, info[i].port_id)) {
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }

    port = &this->ports[info[i].port_id];

    if (!port->format) {
      info[i].status = SPA_RESULT_NO_FORMAT;
      have_error = true;
      continue;
    }

    info[i].buffer_id = port->buffer_id;
    info[i].status = SPA_RESULT_OK;
  }
  if (have_error)
    return SPA_RESULT_ERROR;
  if (need_more)
    return SPA_RESULT_NEED_MORE_INPUT;

  return SPA_RESULT_OK;
}

static SpaResult
spa_proxy_node_port_push_event (SpaNode        *node,
                                uint32_t        port_id,
                                SpaEvent       *event)
{
  SpaProxy *this;
  SpaResult res;

  if (node == NULL || node->handle == NULL || event == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaProxy *) node->handle;

  switch (event->type) {
    case SPA_EVENT_TYPE_REUSE_BUFFER:
    {
      SpaEventReuseBuffer *rb = event->data;
      SpaControlCmdReuseBuffer crb;
      SpaControlBuilder builder;
      SpaControl control;
      uint8_t buf[128];

      /* send start */
      spa_control_builder_init_into (&builder, buf, sizeof (buf), NULL, 0);
      crb.port_id = rb->port_id;
      crb.buffer_id = rb->buffer_id;
      spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_REUSE_BUFFER, &crb);
      spa_control_builder_end (&builder, &control);

      if ((res = spa_control_write (&control, this->fds[0].fd)) < 0)
        fprintf (stderr, "proxy %p: error writing control %d\n", this, res);

      spa_control_clear (&control);
      return SPA_RESULT_OK;
    }
    default:
      break;
  }
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
parse_control (SpaProxy   *this,
               SpaControl *ctrl)
{
  SpaControlIter it;

  spa_control_iter_init (&it, ctrl);
  while (spa_control_iter_next (&it) == SPA_RESULT_OK) {
    SpaControlCmd cmd = spa_control_iter_get_cmd (&it);

    switch (cmd) {
      case SPA_CONTROL_CMD_ADD_PORT:
      case SPA_CONTROL_CMD_REMOVE_PORT:
      case SPA_CONTROL_CMD_SET_FORMAT:
      case SPA_CONTROL_CMD_SET_PROPERTY:
      case SPA_CONTROL_CMD_START:
      case SPA_CONTROL_CMD_PAUSE:
        fprintf (stderr, "proxy %p: got unexpected control %d\n", this, cmd);
        break;

      case SPA_CONTROL_CMD_PORT_REMOVED:
        fprintf (stderr, "proxy %p: command not implemented %d\n", this, cmd);
        break;

      case SPA_CONTROL_CMD_NODE_UPDATE:
      {
        SpaControlCmdNodeUpdate nu;

        if (spa_control_iter_parse_cmd (&it, &nu) < 0)
          break;

        this->max_inputs = nu.max_input_ports;
        this->max_outputs = nu.max_output_ports;
        fprintf (stderr, "proxy %p: got node update %d, %u, %u\n", this, cmd,
            this->max_inputs, this->max_outputs);
        break;
      }

      case SPA_CONTROL_CMD_PORT_UPDATE:
      {
        SpaControlCmdPortUpdate pu;
        bool remove;

        fprintf (stderr, "proxy %p: got port update %d\n", this, cmd);
        if (spa_control_iter_parse_cmd (&it, &pu) < 0)
          break;

        if (pu.port_id >= MAX_PORTS)
          break;

        remove = (pu.change_mask == 0);

        if (remove) {
          do_uninit_port (this, pu.port_id);
        } else {
          do_update_port (this, &pu);
        }
        break;
      }

      case SPA_CONTROL_CMD_STATE_CHANGE:
      {
        SpaControlCmdStateChange sc;

        if (spa_control_iter_parse_cmd (&it, &sc) < 0)
          break;

        fprintf (stderr, "proxy %p: got state-change to %d\n", this, sc.state);
        update_state (this, sc.state);
        break;
      }
      case SPA_CONTROL_CMD_PORT_STATUS_CHANGE:
      {
        fprintf (stderr, "proxy %p: command not implemented %d\n", this, cmd);
        break;
      }
      case SPA_CONTROL_CMD_NEED_INPUT:
      {
        break;
      }
      case SPA_CONTROL_CMD_HAVE_OUTPUT:
      {
        SpaEvent event;
        SpaEventHaveOutput hu;
        SpaControlCmdHaveOutput cmd;

        if (spa_control_iter_parse_cmd (&it, &cmd) < 0)
          break;

        event.type = SPA_EVENT_TYPE_HAVE_OUTPUT;
        event.data = &hu;
        event.size = sizeof (hu);
        hu.port_id = cmd.port_id;
        this->event_cb (&this->node, &event, this->user_data);
        break;
      }

      case SPA_CONTROL_CMD_ADD_MEM:
        break;
      case SPA_CONTROL_CMD_REMOVE_MEM:
        break;
      case SPA_CONTROL_CMD_USE_BUFFERS:
        break;

      case SPA_CONTROL_CMD_PROCESS_BUFFER:
      {
        SpaControlCmdProcessBuffer cmd;
        SpaProxyPort *port;

        if (spa_control_iter_parse_cmd (&it, &cmd) < 0)
          break;

        port = &this->ports[cmd.port_id];
        port->buffer_id = cmd.buffer_id;
        break;
      }
      case SPA_CONTROL_CMD_REUSE_BUFFER:
      {
        SpaEvent event;
        SpaEventReuseBuffer rb;
        SpaControlCmdReuseBuffer crb;

        if (spa_control_iter_parse_cmd (&it, &crb) < 0)
          break;

        event.type = SPA_EVENT_TYPE_REUSE_BUFFER;
        event.data = &rb;
        event.size = sizeof (rb);
        rb.port_id = crb.port_id;
        rb.buffer_id = crb.buffer_id;
        this->event_cb (&this->node, &event, this->user_data);

        break;
      }
      default:
        fprintf (stderr, "proxy %p: command unhandled %d\n", this, cmd);
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
      fprintf (stderr, "proxy %p: failed to read control: %d\n", this, res);
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
  spa_proxy_node_port_reuse_buffer,
  spa_proxy_node_port_get_status,
  spa_proxy_node_port_push_input,
  spa_proxy_node_port_pull_output,
  spa_proxy_node_port_push_event,
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
proxy_clear (SpaHandle *handle)
{
  return SPA_RESULT_OK;
}

static SpaResult
proxy_init (const SpaHandleFactory  *factory,
            SpaHandle               *handle)
{
  SpaProxy *this;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_proxy_get_interface;
  handle->clear = proxy_clear;

  this = (SpaProxy *) handle;
  this->node = proxy_node;
  this->node.handle = handle;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  reset_proxy_props (&this->props[1]);
  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));

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
  proxy_init,
  proxy_enum_interface_info,
};
