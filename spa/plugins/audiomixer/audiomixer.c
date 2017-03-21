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
#include <spa/audio/format-utils.h>
#include <lib/props.h>

#define MAX_PORTS       128

typedef struct _SpaAudioMixer SpaAudioMixer;

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
  SpaAudioInfo format;
  SpaPortInfo info;
  size_t buffer_index;
  size_t buffer_offset;
  size_t buffer_queued;
  MixerBuffer mix;

  SpaBuffer **buffers;
  uint32_t    n_buffers;
  SpaBuffer *buffer;
  void *io;
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

static SpaResult
spa_audiomixer_node_get_props (SpaNode       *node,
                               SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiomixer_node_set_props (SpaNode         *node,
                               const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
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

  switch (SPA_NODE_COMMAND_TYPE (command)) {
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
                                 uint32_t      *n_input_ports,
                                 uint32_t      *max_input_ports,
                                 uint32_t      *n_output_ports,
                                 uint32_t      *max_output_ports)
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
                                  uint32_t       n_input_ports,
                                  uint32_t      *input_ids,
                                  uint32_t       n_output_ports,
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
  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_remove_port (SpaNode        *node,
                                 SpaDirection    direction,
                                 uint32_t        port_id)
{
  SpaAudioMixer *this;
  SpaPortInput *input;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (!CHECK_IN_PORT (this, SPA_DIRECTION_INPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->in_ports[port_id].valid = false;
  this->port_count--;

  input = this->in_ports[port_id].io;
  if (input && input->buffer_id) {
    this->port_queued--;
  }

  return SPA_RESULT_OK;
}


static SpaResult
spa_audiomixer_node_port_enum_formats (SpaNode         *node,
                                       SpaDirection     direction,
                                       uint32_t         port_id,
                                       SpaFormat      **format,
                                       const SpaFormat *filter,
                                       uint32_t         index)
{
  SpaAudioMixer *this;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  switch (index) {
    case 0:
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = NULL;

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

  if ((res = spa_format_audio_parse (format, &port->format)) < 0)
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

  *format = NULL;

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
spa_audiomixer_node_port_set_input (SpaNode      *node,
                                    uint32_t      port_id,
                                    SpaPortInput *input)
{
  SpaAudioMixer *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (!CHECK_PORT (this, SPA_DIRECTION_INPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->in_ports[port_id].io = input;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_set_output (SpaNode       *node,
                                     uint32_t       port_id,
                                     SpaPortOutput *output)
{
  SpaAudioMixer *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (!CHECK_PORT (this, SPA_DIRECTION_OUTPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->out_ports[port_id].io = output;

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

static SpaResult
spa_audiomixer_node_process_input (SpaNode *node)
{
  SpaAudioMixer *this;
  uint32_t i;
  bool have_error = false;
  SpaPortOutput *output;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if ((output = this->out_ports[0].io) == NULL)
    return SPA_RESULT_OK;

  this->port_queued = 0;

  for (i = 0; i < MAX_PORTS; i++) {
    SpaAudioMixerPort *port = &this->in_ports[i];
    SpaPortInput *input;

    if ((input = port->io) == NULL)
      continue;

    if (input->buffer_id >= port->n_buffers) {
      input->status = SPA_RESULT_INVALID_BUFFER_ID;
      have_error = true;
      continue;
    }
    if (port->buffer == NULL) {
      port->buffer = port->buffers[input->buffer_id];
      input->buffer_id = SPA_ID_INVALID;
    }
    input->status = SPA_RESULT_OK;
    this->port_queued++;
  }
  if (have_error)
    return SPA_RESULT_ERROR;

  return this->port_queued == this->port_count ?  SPA_RESULT_HAVE_OUTPUT : SPA_RESULT_OK;
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
      op = (uint8_t*)odatas[oi].data + odatas[oi].chunk->offset;
      os = odatas[oi].chunk->size;
    }
    if (ip == NULL) {
      ip = (uint8_t*)idatas[port->buffer_index].data + idatas[port->buffer_index].chunk->offset;
      is = idatas[port->buffer_index].chunk->size;
      ip += port->buffer_offset;
      is -= port->buffer_offset;
    }

    chunk = os < is ? os : is;

    for (i = 0; i < chunk; i++)
      op[i] += ip[i];

    if ((is -= chunk) == 0) {
      if (++port->buffer_index == port->buffer->n_datas) {
        port->buffer = NULL;
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
mix_data (SpaAudioMixer *this, SpaPortOutput *output)
{
  int i, min_size, min_port;
  SpaBuffer *buf;

  min_size = 0;
  min_port = 0;
  for (i = 0; i < MAX_PORTS; i++) {
    if (!this->in_ports[i].valid)
      continue;

    if (this->in_ports[i].buffer == NULL)
      return SPA_RESULT_NEED_INPUT;

    if (min_size == 0 || this->in_ports[i].buffer_queued < min_size) {
      min_size = this->in_ports[i].buffer_queued;
      min_port = i;
    }
  }
  if (min_port == 0)
    return SPA_RESULT_NEED_INPUT;

  buf = this->in_ports[min_port].buffer;
  output->buffer_id = buf->id;
  this->in_ports[min_port].buffer = NULL;

  for (i = 0; i < MAX_PORTS; i++) {
    if (!this->in_ports[i].valid || this->in_ports[i].buffer == NULL)
      continue;

    add_port_data (this, buf, &this->in_ports[i]);
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_process_output (SpaNode *node)
{
  SpaAudioMixer *this;
  SpaAudioMixerPort *port;
  SpaPortOutput *output;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  port = &this->out_ports[0];
  if ((output = port->io) == NULL)
    return SPA_RESULT_ERROR;

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  if ((output->status = mix_data (this, output)) < 0)
    return SPA_RESULT_ERROR;

  return SPA_RESULT_OK;
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
  spa_audiomixer_node_port_set_input,
  spa_audiomixer_node_port_set_output,
  spa_audiomixer_node_port_reuse_buffer,
  spa_audiomixer_node_port_send_command,
  spa_audiomixer_node_process_input,
  spa_audiomixer_node_process_output,
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
                     uint32_t                n_support)
{
  SpaAudioMixer *this;
  uint32_t i;

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
                                uint32_t                 index)
{
  if (factory == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  switch (index) {
    case 0:
      *info = &audiomixer_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_audiomixer_factory =
{ "audiomixer",
  NULL,
  sizeof (SpaAudioMixer),
  spa_audiomixer_init,
  audiomixer_enum_interface_info,
};
