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
#include <spa/list.h>
#include <spa/type-map.h>
#include <spa/node.h>
#include <spa/audio/format-utils.h>
#include <lib/props.h>

#define MAX_BUFFERS     64
#define MAX_PORTS       128

typedef struct _SpaAudioMixer SpaAudioMixer;

typedef struct {
  SpaBuffer     *outbuf;
  bool           outstanding;
  SpaMetaHeader *h;
  SpaList        link;
} MixerBuffer;

typedef struct {
  bool valid;
  bool have_format;
  SpaAudioInfo format;
  SpaPortInfo info;

  SpaPortIO   *io;
  MixerBuffer  buffers[MAX_BUFFERS];
  uint32_t     n_buffers;
  SpaList      queue;
  size_t       queued_offset;
  size_t       queued_bytes;
} SpaAudioMixerPort;

typedef struct {
  uint32_t node;
  SpaTypeMediaType media_type;
  SpaTypeMediaSubtype media_subtype;
  SpaTypeFormatAudio format_audio;
  SpaTypeCommandNode command_node;
} Type;

static inline void
init_type (Type *type, SpaTypeMap *map)
{
  type->node = spa_type_map_get_id (map, SPA_TYPE__Node);
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_format_audio_map (map, &type->format_audio);
  spa_type_command_node_map (map, &type->command_node);
}

struct _SpaAudioMixer {
  SpaHandle  handle;
  SpaNode  node;

  Type type;
  SpaTypeMap *map;
  SpaLog *log;

  SpaEventNodeCallback event_cb;
  void *user_data;

  int port_count;
  int port_queued;
  SpaAudioMixerPort in_ports[MAX_PORTS];
  SpaAudioMixerPort out_ports[1];

#define STATE_ERROR     0
#define STATE_IN        1
#define STATE_OUT       2
  int state;
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
spa_audiomixer_node_send_command (SpaNode    *node,
                                  SpaCommand *command)
{
  SpaAudioMixer *this;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (SPA_COMMAND_TYPE (command) == this->type.command_node.Start) {
    update_state (this, SPA_NODE_STATE_STREAMING);
  }
  else if (SPA_COMMAND_TYPE (command) == this->type.command_node.Pause) {
    update_state (this, SPA_NODE_STATE_PAUSED);
  }
  else
    return SPA_RESULT_NOT_IMPLEMENTED;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_set_event_callback (SpaNode              *node,
                                        SpaEventNodeCallback  event,
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
  spa_list_init (&this->in_ports[port_id].queue);

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
  SpaPortIO *io;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (!CHECK_IN_PORT (this, SPA_DIRECTION_INPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->in_ports[port_id].valid = false;
  this->port_count--;

  io = this->in_ports[port_id].io;
  if (io && io->buffer_id) {
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
clear_buffers (SpaAudioMixer *this, SpaAudioMixerPort *port)
{
  if (port->n_buffers > 0) {
    spa_log_info (this->log, "audio-mixer %p: clear buffers", this);
    port->n_buffers = 0;
    spa_list_init (&port->queue);
  }
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

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[0];

  if (format == NULL) {
    port->have_format = false;
    clear_buffers (this, port);
  } else {
    SpaAudioInfo info = { SPA_FORMAT_MEDIA_TYPE (format),
                          SPA_FORMAT_MEDIA_SUBTYPE (format), };

    if (info.media_type != this->type.media_type.audio ||
        info.media_subtype != this->type.media_subtype.raw)
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    if (!spa_format_audio_raw_parse (format, &info.info.raw, &this->type.format_audio))
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    port->format = info;
    port->have_format = true;
  }

  if (port->have_format) {
    update_state (this, SPA_NODE_STATE_READY);
  }
  else
    update_state (this, SPA_NODE_STATE_CONFIGURE);

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
  SpaAudioMixer *this;
  SpaAudioMixerPort *port;
  uint32_t i;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  spa_return_val_if_fail (port->have_format, SPA_RESULT_NO_FORMAT);

  clear_buffers (this, port);

  for (i = 0; i < n_buffers; i++) {
    MixerBuffer *b;
    SpaData *d = buffers[i]->datas;

    b = &port->buffers[i];
    b->outbuf = buffers[i];
    b->outstanding = direction == SPA_DIRECTION_INPUT ? true : false;
    b->h = spa_buffer_find_meta (buffers[i], SPA_META_TYPE_HEADER);

    switch (d[0].type) {
      case SPA_DATA_TYPE_MEMPTR:
      case SPA_DATA_TYPE_MEMFD:
      case SPA_DATA_TYPE_DMABUF:
        if (d[0].data == NULL) {
          spa_log_error (this->log, "volume %p: invalid memory on buffer %p", this, buffers[i]);
          continue;
        }
        break;
      default:
        break;
    }
    spa_list_insert (port->queue.prev, &b->link);
  }
  port->n_buffers = n_buffers;

  if (port->n_buffers > 0) {
    update_state (this, SPA_NODE_STATE_PAUSED);
  } else {
    update_state (this, SPA_NODE_STATE_READY);
  }
  return SPA_RESULT_OK;
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
spa_audiomixer_node_port_set_io (SpaNode      *node,
                                 SpaDirection  direction,
                                 uint32_t      port_id,
                                 SpaPortIO    *io)
{
  SpaAudioMixer *this;
  SpaAudioMixerPort *port;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];
  port->io = io;

  return SPA_RESULT_OK;
}

static void
recycle_buffer (SpaAudioMixer *this, uint32_t id)
{
  SpaAudioMixerPort *port = &this->out_ports[0];

  MixerBuffer *b = &port->buffers[id];
  if (b->outstanding) {
    spa_list_insert (port->queue.prev, &b->link);
    b->outstanding = false;
    spa_log_trace (this->log, "audiomixer %p: recycle buffer %d", this, id);
  }
}

static SpaResult
spa_audiomixer_node_port_reuse_buffer (SpaNode         *node,
                                       uint32_t         port_id,
                                       uint32_t         buffer_id)
{
  SpaAudioMixer *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  spa_return_val_if_fail (CHECK_PORT (this, SPA_DIRECTION_OUTPUT, port_id), SPA_RESULT_INVALID_PORT);

  recycle_buffer (this, buffer_id);

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiomixer_node_port_send_command (SpaNode        *node,
                                       SpaDirection    direction,
                                       uint32_t        port_id,
                                       SpaCommand     *command)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static void
add_port_data (SpaAudioMixer *this, MixerBuffer *out, SpaAudioMixerPort *port, int layer)
{
  int i;
  uint16_t *op, *ip;
  size_t os, is, chunk;
  MixerBuffer *b = spa_list_first (&port->queue, MixerBuffer, link);

  op = SPA_MEMBER (out->outbuf->datas[0].data, out->outbuf->datas[0].chunk->offset, void);
  os = out->outbuf->datas[0].chunk->size;
  ip = SPA_MEMBER (b->outbuf->datas[0].data, port->queued_offset + b->outbuf->datas[0].chunk->offset, void);
  is = b->outbuf->datas[0].chunk->size - port->queued_offset;

  chunk = SPA_MIN (os, is);

  if (layer == 0) {
    for (i = 0; i < chunk / 2; i++)
      op[i] = ip[i];
  }
  else {
    for (i = 0; i < chunk / 2; i++)
      op[i] += ip[i];
  }

  port->queued_offset += chunk;
  port->queued_bytes -= chunk;

  if (chunk == is) {
    spa_log_trace (this->log, "audiomixer %p: return buffer %d on port %p", this, b->outbuf->id, port);
    port->io->buffer_id = b->outbuf->id;
    spa_list_remove (&b->link);
    b->outstanding = true;
    port->queued_offset = 0;
  }
}

static SpaResult
spa_audiomixer_node_process_input (SpaNode *node)
{
  SpaAudioMixer *this;
  uint32_t i;
  SpaAudioMixerPort *outport;
  size_t min_queued = -1;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  if (this->state == STATE_OUT)
    return SPA_RESULT_HAVE_OUTPUT;

  outport = &this->out_ports[0];
  spa_return_val_if_fail (outport->io != NULL, SPA_RESULT_ERROR);

  for (i = 0; i < MAX_PORTS; i++) {
    SpaAudioMixerPort *port = &this->in_ports[i];
    SpaPortIO *input;

    if (!port->valid || (input = port->io) == NULL)
      continue;

    if (input->buffer_id != SPA_ID_INVALID) {
      MixerBuffer *b = &port->buffers[input->buffer_id];

      if (spa_list_is_empty (&port->queue)) {
        port->queued_bytes = 0;
        port->queued_offset = 0;
      }
      spa_log_trace (this->log, "audiomixer %p: queue buffer %d on port %p", this, b->outbuf->id, port);
      spa_list_insert (port->queue.prev, &b->link);
      b->outstanding = false;
      port->queued_bytes += b->outbuf->datas[0].chunk->size;
      input->buffer_id = SPA_ID_INVALID;
    }
    if (min_queued == -1 || port->queued_bytes < min_queued)
      min_queued = port->queued_bytes;

    input->status = SPA_RESULT_OK;
  }

  if (min_queued != -1) {
    MixerBuffer *outbuf;
    SpaPortIO *output;
    int j;

    if (spa_list_is_empty (&outport->queue))
      return SPA_RESULT_OUT_OF_BUFFERS;

    outbuf = spa_list_first (&outport->queue, MixerBuffer, link);
    spa_list_remove (&outbuf->link);
    spa_log_trace (this->log, "audiomixer %p: dequeue output buffer %d", this, outbuf->outbuf->id);
    outbuf->outstanding = true;
    outbuf->outbuf->datas[0].chunk->offset = 0;
    outbuf->outbuf->datas[0].chunk->size = min_queued;
    outbuf->outbuf->datas[0].chunk->stride = 0;

    for (j = 0, i = 0; i < MAX_PORTS; i++) {
      SpaAudioMixerPort *port = &this->in_ports[i];

      if (!port->valid || port->io == NULL ||
          spa_list_is_empty (&port->queue))
        continue;

      add_port_data (this, outbuf, port, j++);
    }
    output = outport->io;
    output->buffer_id = outbuf->outbuf->id;
    output->state = SPA_RESULT_OK;
    this->state = STATE_OUT;
  }
  return this->state == STATE_IN ? SPA_RESULT_NEED_INPUT : SPA_RESULT_HAVE_OUTPUT;
}

static SpaResult
spa_audiomixer_node_process_output (SpaNode *node)
{
  SpaAudioMixer *this;
  SpaAudioMixerPort *port;
  SpaPortIO *output;
  int i;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioMixer, node);

  port = &this->out_ports[0];
  spa_return_val_if_fail (port->io != NULL, SPA_RESULT_ERROR);
  output = port->io;

  spa_return_val_if_fail (port->have_format, SPA_RESULT_NO_FORMAT);

  for (i = 0; i < MAX_PORTS; i++) {
    SpaPortIO *input;

    if (!this->in_ports[i].valid || (input = this->in_ports[i].io) == NULL)
      continue;

    input->flags = output->flags;
    input->range = output->range;
  }
  if (output->buffer_id != SPA_ID_INVALID) {
    recycle_buffer (this, output->buffer_id);
    output->buffer_id = SPA_ID_INVALID;
  }
  this->state = STATE_IN;

  return SPA_RESULT_NEED_INPUT;
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
  spa_audiomixer_node_port_set_io,
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

  if (interface_id == this->type.node)
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
    if (strcmp (support[i].type, SPA_TYPE__TypeMap) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE__Log) == 0)
      this->log = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "an id-map is needed");
    return SPA_RESULT_ERROR;
  }
  init_type (&this->type, this->map);

  this->node = audiomixer_node;

  this->out_ports[0].valid = true;
  this->out_ports[0].info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                                  SPA_PORT_INFO_FLAG_NO_REF;
  spa_list_init (&this->out_ports[0].queue);

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo audiomixer_interfaces[] =
{
  { SPA_TYPE__Node, },
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
