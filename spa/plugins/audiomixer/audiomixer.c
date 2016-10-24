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

#include <spa/log.h>
#include <spa/id-map.h>
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
  SpaFormatAudio format[2];
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

typedef struct {
  uint32_t node;
} URI;

struct _SpaAudioMixer {
  SpaHandle  handle;
  SpaNode  node;

  URI uri;
  SpaIDMap *map;
  SpaLog *log;

  SpaAudioMixerProps props[2];

  SpaNodeEventCallback event_cb;
  void *user_data;

  int port_count;
  int port_queued;
  SpaAudioMixerPort in_ports[MAX_PORTS];
  SpaAudioMixerPort out_ports[1];
};

#define CHECK_FREE_IN_PORT(this,d,p) ((d) == SPA_DIRECTION_INPUT && (p) < MAX_PORTS && !this->in_ports[(p)].valid)
#define CHECK_IN_PORT(this,d,p)      ((d) == SPA_DIRECTION_INPUT && (p) < MAX_PORTS && this->in_ports[(p)].valid)
#define CHECK_OUT_PORT(this,d,p)     ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)
#define CHECK_PORT(this,d,p)         (CHECK_OUT_PORT(this,d,p) || CHECK_IN_PORT (this,d,p))

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

  if (node == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

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

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);
  p = &this->props[1];

  if (props == NULL) {
    reset_audiomixer_props (p);
    return SPA_RESULT_OK;
  }
  res = spa_props_copy_values (props, &p->props);

  return res;
}

static void
update_state (SpaAudioMixer *this, SpaNodeState state)
{
  this->node.state = state;
}

static SpaResult
spa_audiomixer_node_send_command (SpaNode        *node,
                                  SpaNodeCommand *command)
{
  SpaAudioMixer *this;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  switch (command->type) {
    case SPA_NODE_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_NODE_COMMAND_START:
      update_state (this, SPA_NODE_STATE_STREAMING);
      break;

    case SPA_NODE_COMMAND_PAUSE:
      update_state (this, SPA_NODE_STATE_PAUSED);
      break;

    case SPA_NODE_COMMAND_FLUSH:
    case SPA_NODE_COMMAND_DRAIN:
    case SPA_NODE_COMMAND_MARKER:
    case SPA_NODE_COMMAND_CLOCK_UPDATE:
      return SPA_RESULT_NOT_IMPLEMENTED;
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_set_event_callback (SpaNode              *node,
                                        SpaNodeEventCallback  event,
                                        void                 *user_data)
{
  SpaAudioMixer *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

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
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports)
    *n_input_ports = 0;
  if (max_input_ports)
    *max_input_ports = MAX_PORTS;
  if (n_output_ports)
    *n_output_ports = 1;
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

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (input_ids) {
    for (i = 0, idx = 0; i < MAX_PORTS && idx < n_input_ports; i++) {
      if (this->in_ports[i].valid)
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

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (!CHECK_FREE_IN_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->in_ports[port_id].valid = true;
  this->port_count++;

  this->in_ports[port_id].info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                                    SPA_PORT_INFO_FLAG_REMOVABLE |
                                    SPA_PORT_INFO_FLAG_OPTIONAL |
                                    SPA_PORT_INFO_FLAG_IN_PLACE;
  this->in_ports[port_id].status.flags = SPA_PORT_STATUS_FLAG_NEED_INPUT;

  this->out_ports[0].status.flags &= ~SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_remove_port (SpaNode        *node,
                                 SpaDirection    direction,
                                 uint32_t        port_id)
{
  SpaAudioMixer *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (!CHECK_IN_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->in_ports[port_id].valid = false;
  this->port_count--;
  if (this->in_ports[port_id].buffer) {
    this->in_ports[port_id].buffer = NULL;
    this->port_queued--;
  }
  if (this->port_count == this->port_queued)
    this->out_ports[0].status.flags |= SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;

  return SPA_RESULT_OK;
}


static SpaResult
spa_audiomixer_node_port_enum_formats (SpaNode         *node,
                                       SpaDirection     direction,
                                       uint32_t         port_id,
                                       SpaFormat      **format,
                                       const SpaFormat *filter,
                                       void           **state)
{
  SpaAudioMixer *this;
  SpaAudioMixerPort *port;
  int index;

  if (node == NULL || format == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[0];

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      spa_format_audio_init (SPA_MEDIA_TYPE_AUDIO,
                             SPA_MEDIA_SUBTYPE_RAW,
                             &port->format[0]);
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
                                     SpaDirection        direction,
                                     uint32_t            port_id,
                                     SpaPortFormatFlags  flags,
                                     const SpaFormat    *format)
{
  SpaAudioMixer *this;
  SpaAudioMixerPort *port;
  SpaResult res;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[0];

  if (format == NULL) {
    port->have_format = false;
    return SPA_RESULT_OK;
  }

  if ((res = spa_format_audio_parse (format, &port->format[1])) < 0)
    return res;

  port->have_format = true;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_get_format (SpaNode          *node,
                                     SpaDirection      direction,
                                     uint32_t          port_id,
                                     const SpaFormat **format)
{
  SpaAudioMixer *this;
  SpaAudioMixerPort *port;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[0];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = &port->format[1].format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_get_info (SpaNode            *node,
                                   SpaDirection        direction,
                                   uint32_t            port_id,
                                   const SpaPortInfo **info)
{
  SpaAudioMixer *this;
  SpaAudioMixerPort *port;

  if (node == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[0];
  *info = &port->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_get_props (SpaNode       *node,
                                    SpaDirection   direction,
                                    uint32_t       port_id,
                                    SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiomixer_node_port_set_props (SpaNode        *node,
                                    SpaDirection    direction,
                                    uint32_t        port_id,
                                    const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiomixer_node_port_use_buffers (SpaNode         *node,
                                      SpaDirection     direction,
                                      uint32_t         port_id,
                                      SpaBuffer      **buffers,
                                      uint32_t         n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiomixer_node_port_alloc_buffers (SpaNode         *node,
                                        SpaDirection     direction,
                                        uint32_t         port_id,
                                        SpaAllocParam  **params,
                                        uint32_t         n_params,
                                        SpaBuffer      **buffers,
                                        uint32_t        *n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiomixer_node_port_get_status (SpaNode              *node,
                                     SpaDirection          direction,
                                     uint32_t              port_id,
                                     const SpaPortStatus **status)
{
  SpaAudioMixer *this;
  SpaAudioMixerPort *port;

  if (node == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[0];
  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  *status = &port->status;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_push_input (SpaNode          *node,
                                     unsigned int      n_info,
                                     SpaPortInputInfo *info)
{
  SpaAudioMixer *this;
  unsigned int i;
  bool have_error = false;

  if (node == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (this->out_ports[0].status.flags & SPA_PORT_STATUS_FLAG_HAVE_OUTPUT)
    return SPA_RESULT_HAVE_ENOUGH_INPUT;

  for (i = 0; i < n_info; i++) {
    SpaBuffer *buffer;
    SpaAudioMixerPort *port;
    int idx = info[i].port_id;

    if (!CHECK_IN_PORT (this, SPA_DIRECTION_INPUT, idx)) {
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }
    port = &this->in_ports[idx];
    buffer = port->buffers[info[i].buffer_id];

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

      if (port->buffer != NULL) {
        info[i].status = SPA_RESULT_HAVE_ENOUGH_INPUT;
        have_error = true;
        continue;
      }
      port->buffer = buffer;
      port->buffer_queued = 0;
      port->buffer_index = 0;
      port->buffer_offset = 0;
      this->port_queued++;

      if (this->port_queued == this->port_count)
        this->out_ports[0].status.flags |= SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;
    }
    info[i].status = SPA_RESULT_OK;
  }
  if (have_error)
    return SPA_RESULT_ERROR;

  return SPA_RESULT_OK;
}


static void
pull_port (SpaAudioMixer *this, uint32_t port_id, SpaPortOutputInfo *info, size_t pull_size)
{
  SpaNodeEventNeedInput ni;

  ni.event.type = SPA_NODE_EVENT_TYPE_NEED_INPUT;
  ni.event.size = sizeof (ni);
  ni.port_id = port_id;
  this->event_cb (&this->node, &ni.event, this->user_data);
}

static void
add_port_data (SpaAudioMixer *this, SpaBuffer *out, SpaAudioMixerPort *port)
{
  int i, oi = 0;
  uint8_t *op, *ip;
  size_t os, is, chunk;
  SpaData *odatas = out->datas;
  SpaData *idatas = port->buffer->datas;

  op = ip = NULL;

  while (true) {
    if (op == NULL) {
      op = (uint8_t*)odatas[oi].data + odatas[oi].offset;
      os = odatas[oi].size;
    }
    if (ip == NULL) {
      ip = (uint8_t*)idatas[port->buffer_index].data + idatas[port->buffer_index].offset;
      is = idatas[port->buffer_index].size;
      ip += port->buffer_offset;
      is -= port->buffer_offset;
    }

    chunk = os < is ? os : is;

    for (i = 0; i < chunk; i++)
      op[i] += ip[i];

    if ((is -= chunk) == 0) {
      if (++port->buffer_index == port->buffer->n_datas) {
        port->buffer = NULL;
        port->status.flags = SPA_PORT_STATUS_FLAG_NEED_INPUT;
        this->out_ports[0].status.flags &= ~SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;
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
mix_data (SpaAudioMixer *this, SpaPortOutputInfo *info)
{
  int i, min_size, min_port, pull_size;
  SpaBuffer *buf;

  if (info->port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  pull_size = 0;

  min_size = 0;
  min_port = 0;
  for (i = 0; i < MAX_PORTS; i++) {
    if (!this->in_ports[i].valid)
      continue;

    if (this->in_ports[i].buffer == NULL) {
      if (pull_size && info->flags & SPA_PORT_OUTPUT_FLAG_PULL) {
        pull_port (this, i, info, pull_size);
      }
      if (this->in_ports[i].buffer == NULL)
        return SPA_RESULT_NEED_MORE_INPUT;
    }

    if (min_size == 0 || this->in_ports[i].buffer_queued < min_size) {
      min_size = this->in_ports[i].buffer_queued;
      min_port = i;
    }
  }
  if (min_port == 0)
    return SPA_RESULT_NEED_MORE_INPUT;

  buf = this->in_ports[min_port].buffer;
  info->buffer_id = buf->id;
  this->in_ports[min_port].buffer = NULL;
  this->in_ports[min_port].status.flags = SPA_PORT_STATUS_FLAG_NEED_INPUT;
  this->out_ports[0].status.flags &= ~SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;

  for (i = 0; i < MAX_PORTS; i++) {
    if (!this->in_ports[i].valid || this->in_ports[i].buffer == NULL)
      continue;

    add_port_data (this, buf, &this->in_ports[i]);
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_pull_output (SpaNode           *node,
                                      unsigned int       n_info,
                                      SpaPortOutputInfo *info)
{
  SpaAudioMixer *this;
  SpaAudioMixerPort *port;
  int i;
  bool have_error = false;

  if (node == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (!CHECK_OUT_PORT (this, SPA_DIRECTION_OUTPUT, info->port_id))
    return SPA_RESULT_INVALID_PORT;

  port = &this->out_ports[info->port_id];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

//  if (!(this->ports[0].status.flags & SPA_PORT_STATUS_FLAG_HAVE_OUTPUT))
//    return SPA_RESULT_NEED_MORE_INPUT;

  for (i = 0; i < n_info; i++) {
    if ((info[i].status = mix_data (this, &info[i])) < 0) {
      spa_log_error (this->log, "error mixing: %d\n", info[i].status);
      have_error = true;
      continue;
    }
  }
  if (have_error)
    return SPA_RESULT_ERROR;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_reuse_buffer (SpaNode         *node,
                                       uint32_t         port_id,
                                       uint32_t         buffer_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiomixer_node_port_send_command (SpaNode        *node,
                                       SpaDirection    direction,
                                       uint32_t        port_id,
                                       SpaNodeCommand *command)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static const SpaNode audiomixer_node = {
  sizeof (SpaNode),
  NULL,
  SPA_NODE_STATE_INIT,
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
  spa_audiomixer_node_port_reuse_buffer,
  spa_audiomixer_node_port_send_command,
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

  if (interface_id == this->uri.node)
    *interface = &this->node;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_clear (SpaHandle *handle)
{
  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_init (const SpaHandleFactory *factory,
                     SpaHandle              *handle,
                     const SpaDict          *info,
                     const SpaSupport       *support,
                     unsigned int            n_support)
{
  SpaAudioMixer *this;
  unsigned int i;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_audiomixer_get_interface;
  handle->clear = spa_audiomixer_clear;

  this = (SpaAudioMixer *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].uri, SPA_ID_MAP_URI) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].uri, SPA_LOG_URI) == 0)
      this->log = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "an id-map is needed");
    return SPA_RESULT_ERROR;
  }
  this->uri.node = spa_id_map_get_id (this->map, SPA_NODE_URI);

  this->node = audiomixer_node;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  reset_audiomixer_props (&this->props[1]);

  this->out_ports[0].valid = true;
  this->out_ports[0].info.flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS |
                                  SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                                  SPA_PORT_INFO_FLAG_NO_REF;
  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo audiomixer_interfaces[] =
{
  { SPA_NODE_URI, },
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
