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
  SpaAudioMixerPortProps props[2];
  SpaPortInfo info;
  SpaPortStatus status;
  SpaBuffer *buffer;
  size_t buffer_index;
  size_t buffer_offset;
  size_t buffer_queued;
  MixerBuffer mix;
} SpaAudioMixerPort;

struct _SpaAudioMixer {
  SpaHandle  handle;

  SpaAudioMixerProps props[2];

  SpaEventCallback event_cb;
  void *user_data;

  bool have_format;
  SpaAudioRawFormat query_format;
  SpaAudioRawFormat current_format;

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
spa_audiomixer_node_get_props (SpaHandle     *handle,
                               SpaProps     **props)
{
  SpaAudioMixer *this = (SpaAudioMixer *) handle;

  if (handle == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));
  *props = &this->props[0].props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_set_props (SpaHandle       *handle,
                               const SpaProps  *props)
{
  SpaAudioMixer *this = (SpaAudioMixer *) handle;
  SpaAudioMixerProps *p = &this->props[1];
  SpaResult res;

  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (props == NULL) {
    reset_audiomixer_props (p);
    return SPA_RESULT_OK;
  }
  res = spa_props_copy (props, &p->props);

  return res;
}

static SpaResult
spa_audiomixer_node_send_command (SpaHandle     *handle,
                                  SpaCommand    *command)
{
  SpaAudioMixer *this = (SpaAudioMixer *) handle;

  if (handle == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

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

        this->event_cb (handle, &event, this->user_data);
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

        this->event_cb (handle, &event, this->user_data);
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
spa_audiomixer_node_set_event_callback (SpaHandle        *handle,
                                        SpaEventCallback  event,
                                        void             *user_data)
{
  SpaAudioMixer *this = (SpaAudioMixer *) handle;

  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_get_n_ports (SpaHandle     *handle,
                                 unsigned int  *n_input_ports,
                                 unsigned int  *max_input_ports,
                                 unsigned int  *n_output_ports,
                                 unsigned int  *max_output_ports)
{
  if (handle == NULL)
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
spa_audiomixer_node_get_port_ids (SpaHandle     *handle,
                                  unsigned int   n_input_ports,
                                  uint32_t      *input_ids,
                                  unsigned int   n_output_ports,
                                  uint32_t      *output_ids)
{
  SpaAudioMixer *this = (SpaAudioMixer *) handle;
  int i, idx;

  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

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
spa_audiomixer_node_add_port (SpaHandle      *handle,
                              SpaDirection    direction,
                              uint32_t       *port_id)
{
  SpaAudioMixer *this = (SpaAudioMixer *) handle;
  int i;

  if (handle == NULL || port_id == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (direction != SPA_DIRECTION_INPUT)
    return SPA_RESULT_INVALID_DIRECTION;

  for (i = 1; i < MAX_PORTS; i++)
    if (!this->ports[i].valid)
      break;
  if (i == MAX_PORTS)
    return SPA_RESULT_TOO_MANY_PORTS;

  this->ports[i].valid = true;
  *port_id = i;
  this->port_count++;

  this->ports[i].info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFER |
                              SPA_PORT_INFO_FLAG_REMOVABLE |
                              SPA_PORT_INFO_FLAG_OPTIONAL |
                              SPA_PORT_INFO_FLAG_IN_PLACE;
  this->ports[i].status.flags = SPA_PORT_STATUS_FLAG_NEED_INPUT;

  this->ports[0].status.flags &= ~SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_remove_port (SpaHandle      *handle,
                                 uint32_t        port_id)
{
  SpaAudioMixer *this = (SpaAudioMixer *) handle;

  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

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
spa_audiomixer_node_port_enum_formats (SpaHandle       *handle,
                                       uint32_t         port_id,
                                       unsigned int     index,
                                       SpaFormat      **format)
{
  SpaAudioMixer *this = (SpaAudioMixer *) handle;

  if (handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id > MAX_PORTS)
    return SPA_RESULT_INVALID_PORT;

  switch (index) {
    case 0:
      spa_audio_raw_format_init (&this->query_format);
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = &this->query_format.format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_set_format (SpaHandle       *handle,
                                     uint32_t         port_id,
                                     bool             test_only,
                                     const SpaFormat *format)
{
  SpaAudioMixer *this = (SpaAudioMixer *) handle;
  SpaResult res;

  if (handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id > MAX_PORTS)
    return SPA_RESULT_INVALID_PORT;

  if (format == NULL) {
    this->have_format = false;
    return SPA_RESULT_OK;
  }

  if ((res = spa_audio_raw_format_parse (format, &this->current_format)) < 0)
    return res;

  this->have_format = true;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_get_format (SpaHandle        *handle,
                                     uint32_t          port_id,
                                     const SpaFormat **format)
{
  SpaAudioMixer *this = (SpaAudioMixer *) handle;

  if (handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id >= MAX_PORTS || !this->ports[port_id].valid)
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = &this->current_format.format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_get_info (SpaHandle          *handle,
                                   uint32_t            port_id,
                                   const SpaPortInfo **info)
{
  SpaAudioMixer *this = (SpaAudioMixer *) handle;

  if (handle == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id >= MAX_PORTS || !this->ports[port_id].valid)
    return SPA_RESULT_INVALID_PORT;

  *info = &this->ports[port_id].info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_get_props (SpaHandle *handle,
                                    uint32_t   port_id,
                                    SpaProps **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiomixer_node_port_set_props (SpaHandle      *handle,
                                    uint32_t        port_id,
                                    const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiomixer_node_port_get_status (SpaHandle            *handle,
                                     uint32_t              port_id,
                                     const SpaPortStatus **status)
{
  SpaAudioMixer *this = (SpaAudioMixer *) handle;

  if (handle == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id >= MAX_PORTS || !this->ports[port_id].valid)
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  *status = &this->ports[port_id].status;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_use_buffers (SpaHandle       *handle,
                                      uint32_t         port_id,
                                      SpaBuffer      **buffers,
                                      uint32_t         n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiomixer_node_port_alloc_buffers (SpaHandle       *handle,
                                        uint32_t         port_id,
                                        SpaBuffer      **buffers,
                                        uint32_t        *n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiomixer_node_port_push_input (SpaHandle      *handle,
                                     unsigned int    n_info,
                                     SpaInputInfo   *info)
{
  SpaAudioMixer *this = (SpaAudioMixer *) handle;
  SpaBuffer *buffer;
  SpaEvent *event;
  unsigned int i;
  bool have_error = false;

  if (handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (this->ports[0].status.flags & SPA_PORT_STATUS_FLAG_HAVE_OUTPUT)
    return SPA_RESULT_HAVE_ENOUGH_INPUT;

  for (i = 0; i < n_info; i++) {
    int idx = info[i].port_id;

    if (idx >= MAX_PORTS || !this->ports[idx].valid) {
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }

    event = info[i].event;
    buffer = info[i].buffer;

    if (buffer == NULL && event == NULL) {
      info[i].status = SPA_RESULT_INVALID_ARGUMENTS;
      have_error = true;
      continue;
    }

    if (buffer) {
      if (!this->have_format) {
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
    if (event) {
      info[i].status = SPA_RESULT_NOT_IMPLEMENTED;
      have_error = true;
      continue;
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
  buffer->data[0].data = buffer->samples;
  buffer->data[0].size = pull_size;

  this->event_cb (&this->handle, &event, this->user_data);
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
      op = out->datas[oi].data;
      os = out->datas[oi].size;
    }
    if (ip == NULL) {
      ip = port->buffer->datas[port->buffer_index].data;
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

  if (info->port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  if (info->buffer) {
    pull_size = info->buffer->size;
  } else {
    pull_size = 0;
  }

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

  if (info->buffer) {
    if (info->buffer->size < min_size)
      min_size = info->buffer->size;
    else
      info->buffer->size = min_size;
  } else {
    info->buffer = this->ports[min_port].buffer;
    this->ports[min_port].buffer = NULL;
    this->ports[min_port].status.flags = SPA_PORT_STATUS_FLAG_NEED_INPUT;
    this->ports[0].status.flags &= ~SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;
  }

  for (i = 1; i < MAX_PORTS; i++) {
    if (!this->ports[i].valid || this->ports[i].buffer == NULL)
      continue;

    add_port_data (this, info->buffer, &this->ports[i]);
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_audiomixer_node_port_pull_output (SpaHandle      *handle,
                                      unsigned int    n_info,
                                      SpaOutputInfo  *info)
{
  SpaAudioMixer *this = (SpaAudioMixer *) handle;
  int i;
  bool have_error = false;

  if (handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (info->port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
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

static const SpaNode audiomixer_node = {
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
};

static SpaResult
spa_audiomixer_get_interface (SpaHandle   *handle,
                              uint32_t     interface_id,
                              const void **interface)
{
  if (handle == NULL || interface == 0)
    return SPA_RESULT_INVALID_ARGUMENTS;

  switch (interface_id) {
    case SPA_INTERFACE_ID_NODE:
      *interface = &audiomixer_node;
      break;
    default:
      return SPA_RESULT_UNKNOWN_INTERFACE;
  }
  return SPA_RESULT_OK;
}

SpaHandle *
spa_audiomixer_new (void)
{
  SpaHandle *handle;
  SpaAudioMixer *this;

  handle = calloc (1, sizeof (SpaAudioMixer));
  handle->get_interface = spa_audiomixer_get_interface;

  this = (SpaAudioMixer *) handle;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  this->props[1].props.set_prop = spa_props_generic_set_prop;
  this->props[1].props.get_prop = spa_props_generic_get_prop;
  reset_audiomixer_props (&this->props[1]);

  this->ports[0].valid = true;
  this->ports[0].info.flags = SPA_PORT_INFO_FLAG_CAN_GIVE_BUFFER |
                              SPA_PORT_INFO_FLAG_CAN_USE_BUFFER |
                              SPA_PORT_INFO_FLAG_NO_REF;
  return handle;
}
