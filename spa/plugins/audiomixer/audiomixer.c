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
#include <stdio.h>

#include <spa/node.h>
#include <spa/audio/format.h>

#define MAX_PORTS       128

typedef struct _SpaAudioMixer SpaAudioMixer;

typedef struct {
  SpaProps props;
} SpaAudioMixerProps;

typedef struct {
  SpaProps prop;
} SpaAudioMixerPortProps;

typedef struct _MixerBuffer MixerBuffer;

struct _MixerBuffer {
  SpaBuffer buffer;
  SpaMeta meta[1];
  SpaMetaHeader header;
  SpaData data[1];
  uint16_t samples[4096];
};

typedef struct {
  bool valid;
  bool have_format;
  SpaAudioRawFormat format[2];
  SpaAudioMixerPortProps props[2];
  SpaPortInfo info;
  SpaPortStatus status;
  size_t buffer_index;
  size_t buffer_offset;
  size_t buffer_queued;
  MixerBuffer mix;

  SpaBuffer **buffers;
  unsigned int n_buffers;
  SpaBuffer *buffer;
} SpaAudioMixerPort;

struct _SpaAudioMixer {
  SpaHandle  handle;
  SpaNode  node;

  SpaAudioMixerProps props[2];

  SpaEventCallback event_cb;
  void *user_data;

  int port_count;
  int port_queued;
  SpaAudioMixerPort ports[MAX_PORTS];
};

enum {
  PROP_ID_LAST,
};

static const SpaPropInfo prop_info[] =
{
  { PROP_ID_LAST, },
};

static void
reset_audiomixer_props (SpaAudioMixerProps *props)
{
}

static SpaResult
spa_audiomixer_node_get_props (SpaNode       *node,
                               SpaProps     **props)
{
  SpaAudioMixer *this;

  if (node == NULL || node->handle == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioMixer *) node->handle;

  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));
  *props = &this->props[0].props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_set_props (SpaNode         *node,
                               const SpaProps  *props)
{
  SpaAudioMixer *this;
  SpaAudioMixerProps *p;
  SpaResult res;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioMixer *) node->handle;
  p = &this->props[1];

  if (props == NULL) {
    reset_audiomixer_props (p);
    return SPA_RESULT_OK;
  }
  res = spa_props_copy (props, &p->props);

  return res;
}

static SpaResult
spa_audiomixer_node_send_command (SpaNode       *node,
                                  SpaCommand    *command)
{
  SpaAudioMixer *this;

  if (node == NULL || node->handle == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioMixer *) node->handle;

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

        this->event_cb (node, &event, this->user_data);
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

        this->event_cb (node, &event, this->user_data);
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
spa_audiomixer_node_set_event_callback (SpaNode          *node,
                                        SpaEventCallback  event,
                                        void             *user_data)
{
  SpaAudioMixer *this;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioMixer *) node->handle;

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_get_n_ports (SpaNode       *node,
                                 unsigned int  *n_input_ports,
                                 unsigned int  *max_input_ports,
                                 unsigned int  *n_output_ports,
                                 unsigned int  *max_output_ports)
{
  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports)
    *n_input_ports = 0;
  if (n_output_ports)
    *n_output_ports = 1;
  if (max_input_ports)
    *max_input_ports = MAX_PORTS;
  if (max_output_ports)
    *max_output_ports = 1;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_get_port_ids (SpaNode       *node,
                                  unsigned int   n_input_ports,
                                  uint32_t      *input_ids,
                                  unsigned int   n_output_ports,
                                  uint32_t      *output_ids)
{
  SpaAudioMixer *this;
  int i, idx;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioMixer *) node->handle;

  if (input_ids) {
    for (i = 1, idx = 0; i < MAX_PORTS && idx < n_input_ports; i++) {
      if (this->ports[i].valid)
        input_ids[idx++] = i;
    }
  }
  if (n_output_ports > 0 && output_ids)
    output_ids[0] = 0;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_add_port (SpaNode        *node,
                              SpaDirection    direction,
                              uint32_t        port_id)
{
  SpaAudioMixer *this;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioMixer *) node->handle;

  if (direction != SPA_DIRECTION_INPUT)
    return SPA_RESULT_INVALID_DIRECTION;

  if (port_id >= MAX_PORTS)
    return SPA_RESULT_INVALID_PORT;

  if (this->ports[port_id].valid)
    return SPA_RESULT_INVALID_PORT;

  this->ports[port_id].valid = true;
  this->port_count++;

  this->ports[port_id].info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFER |
                                    SPA_PORT_INFO_FLAG_REMOVABLE |
                                    SPA_PORT_INFO_FLAG_OPTIONAL |
                                    SPA_PORT_INFO_FLAG_IN_PLACE;
  this->ports[port_id].status.flags = SPA_PORT_STATUS_FLAG_NEED_INPUT;

  this->ports[0].status.flags &= ~SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_remove_port (SpaNode        *node,
                                 uint32_t        port_id)
{
  SpaAudioMixer *this;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioMixer *) node->handle;

  if (port_id == 0 || port_id >= MAX_PORTS || !this->ports[port_id].valid)
    return SPA_RESULT_INVALID_PORT;

  this->ports[port_id].valid = false;
  this->port_count--;
  if (this->ports[port_id].buffer) {
    spa_buffer_unref (this->ports[port_id].buffer);
    this->ports[port_id].buffer = NULL;
    this->port_queued--;
  }
  if (this->port_count == this->port_queued)
    this->ports[0].status.flags |= SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;

  return SPA_RESULT_OK;
}


static SpaResult
spa_audiomixer_node_port_enum_formats (SpaNode         *node,
                                       uint32_t         port_id,
                                       SpaFormat      **format,
                                       const SpaFormat *filter,
                                       void           **state)
{
  SpaAudioMixer *this;
  SpaAudioMixerPort *port;
  int index;

  if (node == NULL || node->handle == NULL || format == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioMixer *) node->handle;

  if (port_id > MAX_PORTS || !this->ports[port_id].valid)
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      spa_audio_raw_format_init (&port->format[0]);
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = &port->format[0].format;
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_set_format (SpaNode            *node,
                                     uint32_t            port_id,
                                     SpaPortFormatFlags  flags,
                                     const SpaFormat    *format)
{
  SpaAudioMixer *this;
  SpaAudioMixerPort *port;
  SpaResult res;

  if (node == NULL || node->handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioMixer *) node->handle;

  if (port_id > MAX_PORTS || !this->ports[port_id].valid)
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];

  if (format == NULL) {
    port->have_format = false;
    return SPA_RESULT_OK;
  }

  if ((res = spa_audio_raw_format_parse (format, &port->format[1])) < 0)
    return res;

  port->have_format = true;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_get_format (SpaNode          *node,
                                     uint32_t          port_id,
                                     const SpaFormat **format)
{
  SpaAudioMixer *this;
  SpaAudioMixerPort *port;

  if (node == NULL || node->handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioMixer *) node->handle;

  if (port_id >= MAX_PORTS || !this->ports[port_id].valid)
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = &port->format[1].format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_get_info (SpaNode            *node,
                                   uint32_t            port_id,
                                   const SpaPortInfo **info)
{
  SpaAudioMixer *this;
  SpaAudioMixerPort *port;

  if (node == NULL || node->handle == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioMixer *) node->handle;

  if (port_id >= MAX_PORTS || !this->ports[port_id].valid)
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];
  *info = &port->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_get_props (SpaNode   *node,
                                    uint32_t   port_id,
                                    SpaProps **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiomixer_node_port_set_props (SpaNode        *node,
                                    uint32_t        port_id,
                                    const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiomixer_node_port_get_status (SpaNode              *node,
                                     uint32_t              port_id,
                                     const SpaPortStatus **status)
{
  SpaAudioMixer *this;
  SpaAudioMixerPort *port;

  if (node == NULL || node->handle == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioMixer *) node->handle;

  if (port_id >= MAX_PORTS || !this->ports[port_id].valid)
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];
  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  *status = &port->status;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_use_buffers (SpaNode         *node,
                                      uint32_t         port_id,
                                      SpaBuffer      **buffers,
                                      uint32_t         n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiomixer_node_port_alloc_buffers (SpaNode         *node,
                                        uint32_t         port_id,
                                        SpaAllocParam  **params,
                                        uint32_t         n_params,
                                        SpaBuffer      **buffers,
                                        uint32_t        *n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiomixer_node_port_push_input (SpaNode        *node,
                                     unsigned int    n_info,
                                     SpaInputInfo   *info)
{
  SpaAudioMixer *this;
  unsigned int i;
  bool have_error = false;

  if (node == NULL || node->handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioMixer *) node->handle;

  if (this->ports[0].status.flags & SPA_PORT_STATUS_FLAG_HAVE_OUTPUT)
    return SPA_RESULT_HAVE_ENOUGH_INPUT;

  for (i = 0; i < n_info; i++) {
    SpaBuffer *buffer;
    SpaAudioMixerPort *port;
    int idx = info[i].port_id;

    if (idx >= MAX_PORTS || !this->ports[idx].valid) {
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }
    port = &this->ports[idx];
    buffer = port->buffers[info[i].id];

    if (buffer == NULL) {
      info[i].status = SPA_RESULT_INVALID_ARGUMENTS;
      have_error = true;
      continue;
    }

    if (buffer) {
      if (!port->have_format) {
        info[i].status = SPA_RESULT_NO_FORMAT;
        have_error = true;
        continue;
      }

      if (this->ports[idx].buffer != NULL) {
        info[i].status = SPA_RESULT_HAVE_ENOUGH_INPUT;
        have_error = true;
        continue;
      }
      this->ports[idx].buffer = spa_buffer_ref (buffer);
      this->ports[idx].buffer_queued = buffer->size;
      this->ports[idx].buffer_index = 0;
      this->ports[idx].buffer_offset = 0;
      this->port_queued++;

      if (this->port_queued == this->port_count)
        this->ports[0].status.flags |= SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;
    }
    info[i].status = SPA_RESULT_OK;
  }
  if (have_error)
    return SPA_RESULT_ERROR;

  return SPA_RESULT_OK;
}


static void
pull_port (SpaAudioMixer *this, uint32_t port_id, SpaOutputInfo *info, size_t pull_size)
{
  SpaEvent event;
  MixerBuffer *buffer = &this->ports[port_id].mix;

  event.refcount = 1;
  event.notify = NULL;
  event.type = SPA_EVENT_TYPE_PULL_INPUT;
  event.port_id = port_id;
  event.data = buffer;

  buffer->buffer.refcount = 1;
  buffer->buffer.notify = NULL;
  buffer->buffer.size = pull_size;
  buffer->buffer.n_metas = 1;
  buffer->buffer.metas = buffer->meta;
  buffer->buffer.n_datas = 1;
  buffer->buffer.datas = buffer->data;

  buffer->header.flags = 0;
  buffer->header.seq = 0;
  buffer->header.pts = 0;
  buffer->header.dts_offset = 0;

  buffer->meta[0].type = SPA_META_TYPE_HEADER;
  buffer->meta[0].data = &buffer->header;
  buffer->meta[0].size = sizeof (buffer->header);

  buffer->data[0].type = SPA_DATA_TYPE_MEMPTR;
  buffer->data[0].ptr = buffer->samples;
  buffer->data[0].ptr_type = "sysmem";
  buffer->data[0].size = pull_size;

  this->event_cb (&this->node, &event, this->user_data);
}

static void
add_port_data (SpaAudioMixer *this, SpaBuffer *out, SpaAudioMixerPort *port)
{
  int i, oi = 0;
  uint8_t *op, *ip;
  size_t os, is, chunk;

  op = ip = NULL;

  while (true) {
    if (op == NULL) {
      op = out->datas[oi].ptr;
      os = out->datas[oi].size;
    }
    if (ip == NULL) {
      ip = port->buffer->datas[port->buffer_index].ptr;
      is = port->buffer->datas[port->buffer_index].size;
      ip += port->buffer_offset;
      is -= port->buffer_offset;
    }

    chunk = os < is ? os : is;

    for (i = 0; i < chunk; i++)
      op[i] += ip[i];

    if ((is -= chunk) == 0) {
      if (++port->buffer_index == port->buffer->n_datas) {
        spa_buffer_unref (port->buffer);
        port->buffer = NULL;
        port->status.flags = SPA_PORT_STATUS_FLAG_NEED_INPUT;
        this->ports[0].status.flags &= ~SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;
        break;
      }
      port->buffer_offset = 0;
      ip = NULL;
    } else {
      port->buffer_offset += chunk;
    }
    port->buffer_queued -= chunk;

    if ((os -= chunk) == 0) {
      if (++oi == out->n_datas)
        break;
      op = NULL;
    }
  }
}

static SpaResult
mix_data (SpaAudioMixer *this, SpaOutputInfo *info)
{
  int i, min_size, min_port, pull_size;
  SpaBuffer *buf;

  if (info->port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  pull_size = info->size;

  min_size = 0;
  min_port = 0;
  for (i = 1; i < MAX_PORTS; i++) {
    if (!this->ports[i].valid)
      continue;

    if (this->ports[i].buffer == NULL) {
      if (pull_size && info->flags & SPA_OUTPUT_FLAG_PULL) {
        pull_port (this, i, info, pull_size);
      }
      if (this->ports[i].buffer == NULL)
        return SPA_RESULT_NEED_MORE_INPUT;
    }

    if (min_size == 0 || this->ports[i].buffer_queued < min_size) {
      min_size = this->ports[i].buffer_queued;
      min_port = i;
    }
  }
  if (min_port == 0)
    return SPA_RESULT_NEED_MORE_INPUT;

  buf = this->ports[min_port].buffer;
  info->id = buf->id;
  this->ports[min_port].buffer = NULL;
  this->ports[min_port].status.flags = SPA_PORT_STATUS_FLAG_NEED_INPUT;
  this->ports[0].status.flags &= ~SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;

  for (i = 1; i < MAX_PORTS; i++) {
    if (!this->ports[i].valid || this->ports[i].buffer == NULL)
      continue;

    add_port_data (this, buf, &this->ports[i]);
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_pull_output (SpaNode        *node,
                                      unsigned int    n_info,
                                      SpaOutputInfo  *info)
{
  SpaAudioMixer *this;
  SpaAudioMixerPort *port;
  int i;
  bool have_error = false;

  if (node == NULL || node->handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioMixer *) node->handle;

  if (info->port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[info->port_id];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

//  if (!(this->ports[0].status.flags & SPA_PORT_STATUS_FLAG_HAVE_OUTPUT))
//    return SPA_RESULT_NEED_MORE_INPUT;

  for (i = 0; i < n_info; i++) {
    if ((info[i].status = mix_data (this, &info[i])) < 0) {
      printf ("error mixing: %d\n", info[i].status);
      have_error = true;
      continue;
    }
  }
  if (have_error)
    return SPA_RESULT_ERROR;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_push_event (SpaNode        *node,
                                     uint32_t        port_id,
                                     SpaEvent       *event)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static const SpaNode audiomixer_node = {
  NULL,
  sizeof (SpaNode),
  spa_audiomixer_node_get_props,
  spa_audiomixer_node_set_props,
  spa_audiomixer_node_send_command,
  spa_audiomixer_node_set_event_callback,
  spa_audiomixer_node_get_n_ports,
  spa_audiomixer_node_get_port_ids,
  spa_audiomixer_node_add_port,
  spa_audiomixer_node_remove_port,
  spa_audiomixer_node_port_enum_formats,
  spa_audiomixer_node_port_set_format,
  spa_audiomixer_node_port_get_format,
  spa_audiomixer_node_port_get_info,
  spa_audiomixer_node_port_get_props,
  spa_audiomixer_node_port_set_props,
  spa_audiomixer_node_port_use_buffers,
  spa_audiomixer_node_port_alloc_buffers,
  spa_audiomixer_node_port_get_status,
  spa_audiomixer_node_port_push_input,
  spa_audiomixer_node_port_pull_output,
  spa_audiomixer_node_port_push_event,
};

static SpaResult
spa_audiomixer_get_interface (SpaHandle   *handle,
                              uint32_t     interface_id,
                              void       **interface)
{
  SpaAudioMixer *this;

  if (handle == NULL || interface == 0)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioMixer *) handle;

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
spa_audiomixer_init (const SpaHandleFactory *factory,
                     SpaHandle              *handle)
{
  SpaAudioMixer *this;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_audiomixer_get_interface;

  this = (SpaAudioMixer *) handle;
  this->node = audiomixer_node;
  this->node.handle = handle;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  this->props[1].props.set_prop = spa_props_generic_set_prop;
  this->props[1].props.get_prop = spa_props_generic_get_prop;
  reset_audiomixer_props (&this->props[1]);

  this->ports[0].valid = true;
  this->ports[0].info.flags = SPA_PORT_INFO_FLAG_CAN_GIVE_BUFFER |
                              SPA_PORT_INFO_FLAG_CAN_USE_BUFFER |
                              SPA_PORT_INFO_FLAG_NO_REF;
  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo audiomixer_interfaces[] =
{
  { SPA_INTERFACE_ID_NODE,
    SPA_INTERFACE_ID_NODE_NAME,
    SPA_INTERFACE_ID_NODE_DESCRIPTION,
  },
};

static SpaResult
audiomixer_enum_interface_info (const SpaHandleFactory  *factory,
                                const SpaInterfaceInfo **info,
                                void                   **state)
{
  int index;

  if (factory == NULL || info == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      *info = &audiomixer_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *(int*)state = ++index;
  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_audiomixer_factory =
{ "audiomixer",
  NULL,
  sizeof (SpaAudioMixer),
  spa_audiomixer_init,
  audiomixer_enum_interface_info,
};
