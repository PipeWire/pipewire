/* Spa ALSA Sink
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

#include <stddef.h>

#include <asoundlib.h>

#include <spa/node.h>
#include <spa/audio/format.h>
#include <lib/props.h>

#include "alsa-utils.h"

#define CHECK_PORT(this,d,p)    ((d) == SPA_DIRECTION_INPUT && (p) == 0)

typedef struct _SpaALSAState SpaALSASink;

static const char default_device[] = "default";
static const uint32_t default_buffer_time = 10000;
static const uint32_t default_period_time = 1000;
static const bool default_period_event = 0;

static void
reset_alsa_sink_props (SpaALSAProps *props)
{
  strncpy (props->device, default_device, 64);
  props->buffer_time = default_buffer_time;
  props->period_time = default_period_time;
  props->period_event = default_period_event;
}

static void
update_state (SpaALSASink *this, SpaNodeState state)
{
  spa_log_info (this->log, "update state %d", state);
  this->node.state = state;
}

static const uint32_t min_uint32 = 1;
static const uint32_t max_uint32 = UINT32_MAX;

static const SpaPropRangeInfo uint32_range[] = {
  { "min", { 4, &min_uint32 } },
  { "max", { 4, &max_uint32 } },
};

enum {
  PROP_ID_DEVICE,
  PROP_ID_DEVICE_NAME,
  PROP_ID_CARD_NAME,
  PROP_ID_BUFFER_TIME,
  PROP_ID_PERIOD_TIME,
  PROP_ID_PERIOD_EVENT,
  PROP_ID_LAST,
};

static const SpaPropInfo prop_info[] =
{
  { PROP_ID_DEVICE,             offsetof (SpaALSAProps, device),
                                "device",
                                SPA_PROP_FLAG_READWRITE,
                                SPA_PROP_TYPE_STRING, 63,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
  { PROP_ID_DEVICE_NAME,        offsetof (SpaALSAProps, device_name),
                                "device-name",
                                SPA_PROP_FLAG_READABLE,
                                SPA_PROP_TYPE_STRING, 127,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
  { PROP_ID_CARD_NAME,          offsetof (SpaALSAProps, card_name),
                                "card-name",
                                SPA_PROP_FLAG_READABLE,
                                SPA_PROP_TYPE_STRING, 127,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
  { PROP_ID_BUFFER_TIME,        offsetof (SpaALSAProps, buffer_time),
                                "buffer-time",
                                SPA_PROP_FLAG_READWRITE,
                                SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                SPA_PROP_RANGE_TYPE_MIN_MAX, 2, uint32_range,
                                NULL },
  { PROP_ID_PERIOD_TIME,        offsetof (SpaALSAProps, period_time),
                                "period-time",
                                SPA_PROP_FLAG_READWRITE,
                                SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                SPA_PROP_RANGE_TYPE_MIN_MAX, 2, uint32_range,
                                NULL },
  { PROP_ID_PERIOD_EVENT,       offsetof (SpaALSAProps, period_event),
                                "period-event",
                                SPA_PROP_FLAG_READWRITE,
                                SPA_PROP_TYPE_BOOL, sizeof (bool),
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
};

static SpaResult
spa_alsa_sink_node_get_props (SpaNode       *node,
                              SpaProps     **props)
{
  SpaALSASink *this;

  if (node == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASink, node);

  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));
  *props = &this->props[0].props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_set_props (SpaNode         *node,
                              const SpaProps  *props)
{
  SpaALSASink *this;
  SpaALSAProps *p;
  SpaResult res;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASink, node);
  p = &this->props[1];

  if (props == NULL) {
    reset_alsa_sink_props (p);
    return SPA_RESULT_OK;
  }

  res = spa_props_copy_values (props, &p->props);

  return res;
}

static SpaResult
do_send_event (SpaPoll        *poll,
               bool            async,
               uint32_t        seq,
               size_t          size,
               void           *data,
               void           *user_data)
{
  SpaALSASink *this = user_data;

  this->event_cb (&this->node, data, this->user_data);

  return SPA_RESULT_OK;
}

static SpaResult
do_command (SpaPoll        *poll,
            bool            async,
            uint32_t        seq,
            size_t          size,
            void           *data,
            void           *user_data)
{
  SpaALSASink *this = user_data;
  SpaResult res;
  SpaNodeCommand *cmd = data;
  SpaNodeEventAsyncComplete ac;

  switch (cmd->type) {
    case SPA_NODE_COMMAND_START:
    case SPA_NODE_COMMAND_PAUSE:
      res = spa_node_port_send_command (&this->node,
                                        SPA_DIRECTION_INPUT,
                                        0,
                                        cmd);
      break;
    default:
      res = SPA_RESULT_NOT_IMPLEMENTED;
      break;
  }

  if (async) {
    ac.event.type = SPA_NODE_EVENT_TYPE_ASYNC_COMPLETE;
    ac.event.size = sizeof (SpaNodeEventAsyncComplete);
    ac.seq = seq;
    ac.res = res;
    spa_poll_invoke (this->main_loop,
                     do_send_event,
                     SPA_ID_INVALID,
                     sizeof (ac),
                     &ac,
                     this);
  }
  return res;
}

static SpaResult
spa_alsa_sink_node_send_command (SpaNode        *node,
                                 SpaNodeCommand *command)
{
  SpaALSASink *this;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASink, node);

  switch (command->type) {
    case SPA_NODE_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_NODE_COMMAND_START:
    case SPA_NODE_COMMAND_PAUSE:
    {
      if (!this->have_format)
        return SPA_RESULT_NO_FORMAT;

      if (this->n_buffers == 0)
        return SPA_RESULT_NO_BUFFERS;

      return spa_poll_invoke (this->data_loop,
                              do_command,
                              ++this->seq,
                              command->size,
                              command,
                              this);

    }
    case SPA_NODE_COMMAND_FLUSH:
    case SPA_NODE_COMMAND_DRAIN:
    case SPA_NODE_COMMAND_MARKER:
    case SPA_NODE_COMMAND_CLOCK_UPDATE:
      return SPA_RESULT_NOT_IMPLEMENTED;
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_set_event_callback (SpaNode              *node,
                                       SpaNodeEventCallback  event,
                                       void                 *user_data)
{
  SpaALSASink *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASink, node);

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_get_n_ports (SpaNode       *node,
                                unsigned int  *n_input_ports,
                                unsigned int  *max_input_ports,
                                unsigned int  *n_output_ports,
                                unsigned int  *max_output_ports)
{
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports)
    *n_input_ports = 1;
  if (max_input_ports)
    *max_input_ports = 1;
  if (n_output_ports)
    *n_output_ports = 0;
  if (max_output_ports)
    *max_output_ports = 0;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_get_port_ids (SpaNode       *node,
                                 unsigned int   n_input_ports,
                                 uint32_t      *input_ids,
                                 unsigned int   n_output_ports,
                                 uint32_t      *output_ids)
{
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports > 0 && input_ids != NULL)
    input_ids[0] = 0;

  return SPA_RESULT_OK;
}


static SpaResult
spa_alsa_sink_node_add_port (SpaNode        *node,
                             SpaDirection    direction,
                             uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_sink_node_remove_port (SpaNode        *node,
                                SpaDirection    direction,
                                uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_sink_node_port_enum_formats (SpaNode         *node,
                                      SpaDirection     direction,
                                      uint32_t         port_id,
                                      SpaFormat      **format,
                                      const SpaFormat *filter,
                                      void           **state)
{
  SpaALSASink *this;
  int index;

  if (node == NULL || format == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASink, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      spa_format_audio_init (SPA_MEDIA_TYPE_AUDIO,
                             SPA_MEDIA_SUBTYPE_RAW,
                             &this->query_format);
      break;
    case 1:
      spa_format_audio_init (SPA_MEDIA_TYPE_AUDIO,
                             SPA_MEDIA_SUBTYPE_AAC,
                             &this->query_format);
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = &this->query_format.format;
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_clear_buffers (SpaALSASink *this)
{
  if (this->n_buffers > 0) {
    SPA_QUEUE_INIT (&this->ready);
    this->n_buffers = 0;
    this->ringbuffer = NULL;
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_port_set_format (SpaNode            *node,
                                    SpaDirection        direction,
                                    uint32_t            port_id,
                                    SpaPortFormatFlags  flags,
                                    const SpaFormat    *format)
{
  SpaALSASink *this;
  SpaResult res;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASink, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (format == NULL) {
    spa_log_info (this->log, "clear format");
    spa_alsa_pause (this, false);
    spa_alsa_clear_buffers (this);
    spa_alsa_close (this);
    this->have_format = false;
    update_state (this, SPA_NODE_STATE_CONFIGURE);
    return SPA_RESULT_OK;
  }

  if ((res = spa_format_audio_parse (format, &this->current_format)) < 0)
    return res;

  if (spa_alsa_set_format (this, &this->current_format, flags) < 0)
    return SPA_RESULT_ERROR;

  this->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                     SPA_PORT_INFO_FLAG_LIVE;
  this->info.maxbuffering = this->buffer_frames * this->frame_size;
  this->info.latency = (this->period_frames * SPA_NSEC_PER_SEC) / this->rate;
  this->info.n_params = 3;
  this->info.params = this->params;
  this->params[0] = &this->param_buffers.param;
  this->param_buffers.param.type = SPA_ALLOC_PARAM_TYPE_BUFFERS;
  this->param_buffers.param.size = sizeof (this->param_buffers);
  this->param_buffers.minsize = this->period_frames * this->frame_size;
  this->param_buffers.stride = 0;
  this->param_buffers.min_buffers = 1;
  this->param_buffers.max_buffers = 32;
  this->param_buffers.align = 16;
  this->params[1] = &this->param_meta.param;
  this->param_meta.param.type = SPA_ALLOC_PARAM_TYPE_META_ENABLE;
  this->param_meta.param.size = sizeof (this->param_meta);
  this->param_meta.type = SPA_META_TYPE_HEADER;
  this->params[2] = &this->param_meta_rb.param;
  this->param_meta_rb.param.type = SPA_ALLOC_PARAM_TYPE_META_ENABLE;
  this->param_meta_rb.param.size = sizeof (this->param_meta_rb);
  this->param_meta_rb.type = SPA_META_TYPE_RINGBUFFER;
  this->param_meta_rb.minsize = this->period_frames * this->frame_size * 32;
  this->param_meta_rb.stride = 0;
  this->param_meta_rb.blocks = 1;
  this->param_meta_rb.align = 16;
  this->info.extra = NULL;

  this->have_format = true;
  update_state (this, SPA_NODE_STATE_READY);

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_port_get_format (SpaNode          *node,
                                    SpaDirection      direction,
                                    uint32_t          port_id,
                                    const SpaFormat **format)
{
  SpaALSASink *this;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASink, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = &this->current_format.format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_port_get_info (SpaNode            *node,
                                  SpaDirection        direction,
                                  uint32_t            port_id,
                                  const SpaPortInfo **info)
{
  SpaALSASink *this;

  if (node == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASink, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  *info = &this->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_port_get_props (SpaNode       *node,
                                   SpaDirection   direction,
                                   uint32_t       port_id,
                                   SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_sink_node_port_set_props (SpaNode         *node,
                                   SpaDirection     direction,
                                   uint32_t         port_id,
                                   const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_sink_node_port_use_buffers (SpaNode         *node,
                                     SpaDirection     direction,
                                     uint32_t         port_id,
                                     SpaBuffer      **buffers,
                                     uint32_t         n_buffers)
{
  SpaALSASink *this;
  int i;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  this = SPA_CONTAINER_OF (node, SpaALSASink, node);

  spa_log_info (this->log, "use buffers %d", n_buffers);

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  if (n_buffers == 0) {
    spa_alsa_pause (this, false);
    spa_alsa_clear_buffers (this);
    update_state (this, SPA_NODE_STATE_READY);
    return SPA_RESULT_OK;
  }

  for (i = 0; i < n_buffers; i++) {
    SpaALSABuffer *b = &this->buffers[i];
    b->outbuf = buffers[i];
    b->outstanding = true;

    b->h = spa_buffer_find_meta (b->outbuf, SPA_META_TYPE_HEADER);
    b->rb = spa_buffer_find_meta (b->outbuf, SPA_META_TYPE_RINGBUFFER);
    if (b->rb)
      this->ringbuffer = b;

    switch (buffers[i]->datas[0].type) {
      case SPA_DATA_TYPE_MEMFD:
      case SPA_DATA_TYPE_DMABUF:
      case SPA_DATA_TYPE_MEMPTR:
        if (buffers[i]->datas[0].data == NULL) {
          spa_log_error (this->log, "alsa-source: need mapped memory");
          continue;
        }
        break;
      default:
        break;
    }
  }
  this->n_buffers = n_buffers;

  update_state (this, SPA_NODE_STATE_PAUSED);

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_port_alloc_buffers (SpaNode         *node,
                                       SpaDirection     direction,
                                       uint32_t         port_id,
                                       SpaAllocParam  **params,
                                       uint32_t         n_params,
                                       SpaBuffer      **buffers,
                                       uint32_t        *n_buffers)
{
  SpaALSASink *this;

  if (node == NULL || buffers == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  this = SPA_CONTAINER_OF (node, SpaALSASink, node);

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_sink_node_port_get_status (SpaNode              *node,
                                    SpaDirection          direction,
                                    uint32_t              port_id,
                                    const SpaPortStatus **status)
{
  SpaALSASink *this;

  if (node == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASink, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  *status = &this->status;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_port_push_input (SpaNode          *node,
                                    unsigned int      n_info,
                                    SpaPortInputInfo *info)
{
  SpaALSASink *this;
  unsigned int i;
  bool have_error = false;

  if (node == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASink, node);

  for (i = 0; i < n_info; i++) {
    SpaALSABuffer *b;

    if (info[i].port_id != 0) {
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }
    if (!this->have_format) {
      info[i].status = SPA_RESULT_NO_FORMAT;
      have_error = true;
      continue;
    }

    if (info[i].buffer_id >= this->n_buffers) {
      info[i].status = SPA_RESULT_INVALID_BUFFER_ID;
      have_error = true;
      continue;
    }

    b = &this->buffers[info[i].buffer_id];
    if (!b->outstanding) {
      info[i].status = SPA_RESULT_INVALID_BUFFER_ID;
      have_error = true;
      continue;
    }
    if (this->ringbuffer) {
      this->ringbuffer->outstanding = true;
      this->ringbuffer = b;
    } else {
      b->next = NULL;
      SPA_QUEUE_PUSH_TAIL (&this->ready, SpaALSABuffer, next, b);
    }
    b->outstanding = false;
    info[i].status = SPA_RESULT_OK;
  }
  if (have_error)
    return SPA_RESULT_ERROR;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_port_pull_output (SpaNode           *node,
                                     unsigned int       n_info,
                                     SpaPortOutputInfo *info)
{
  return SPA_RESULT_INVALID_PORT;
}

static SpaResult
spa_alsa_sink_node_port_reuse_buffer (SpaNode         *node,
                                      uint32_t         port_id,
                                      uint32_t         buffer_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_sink_node_port_send_command (SpaNode          *node,
                                      SpaDirection      direction,
                                      uint32_t          port_id,
                                      SpaNodeCommand   *command)
{
  SpaALSASink *this;
  SpaResult res;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASink, node);

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  switch (command->type) {
    case SPA_NODE_COMMAND_PAUSE:
    {
      if (SPA_RESULT_IS_OK (res = spa_alsa_pause (this, false))) {
        update_state (this, SPA_NODE_STATE_PAUSED);
      }
      break;
    }
    case SPA_NODE_COMMAND_START:
    {
      if (SPA_RESULT_IS_OK (res = spa_alsa_start (this, false))) {
        update_state (this, SPA_NODE_STATE_STREAMING);
      }
      break;
    }
    default:
      res = SPA_RESULT_NOT_IMPLEMENTED;
      break;
  }
  return res;
}


static const SpaNode alsasink_node = {
  sizeof (SpaNode),
  NULL,
  SPA_NODE_STATE_INIT,
  spa_alsa_sink_node_get_props,
  spa_alsa_sink_node_set_props,
  spa_alsa_sink_node_send_command,
  spa_alsa_sink_node_set_event_callback,
  spa_alsa_sink_node_get_n_ports,
  spa_alsa_sink_node_get_port_ids,
  spa_alsa_sink_node_add_port,
  spa_alsa_sink_node_remove_port,
  spa_alsa_sink_node_port_enum_formats,
  spa_alsa_sink_node_port_set_format,
  spa_alsa_sink_node_port_get_format,
  spa_alsa_sink_node_port_get_info,
  spa_alsa_sink_node_port_get_props,
  spa_alsa_sink_node_port_set_props,
  spa_alsa_sink_node_port_use_buffers,
  spa_alsa_sink_node_port_alloc_buffers,
  spa_alsa_sink_node_port_get_status,
  spa_alsa_sink_node_port_push_input,
  spa_alsa_sink_node_port_pull_output,
  spa_alsa_sink_node_port_reuse_buffer,
  spa_alsa_sink_node_port_send_command,
};

static SpaResult
spa_alsa_sink_get_interface (SpaHandle               *handle,
                             uint32_t                 interface_id,
                             void                   **interface)
{
  SpaALSASink *this;

  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASink *) handle;

  if (interface_id == this->uri.node)
    *interface = &this->node;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static SpaResult
alsa_sink_clear (SpaHandle *handle)
{
  return SPA_RESULT_OK;
}

static SpaResult
alsa_sink_init (const SpaHandleFactory  *factory,
                SpaHandle               *handle,
                const SpaDict           *info,
                const SpaSupport        *support,
                unsigned int             n_support)
{
  SpaALSASink *this;
  unsigned int i;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_alsa_sink_get_interface;
  handle->clear = alsa_sink_clear;

  this = (SpaALSASink *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].uri, SPA_ID_MAP_URI) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].uri, SPA_LOG_URI) == 0)
      this->log = support[i].data;
    else if (strcmp (support[i].uri, SPA_POLL__DataLoop) == 0)
      this->data_loop = support[i].data;
    else if (strcmp (support[i].uri, SPA_POLL__MainLoop) == 0)
      this->main_loop = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "an id-map is needed");
    return SPA_RESULT_ERROR;
  }
  this->uri.node = spa_id_map_get_id (this->map, SPA_NODE_URI);

  this->node = alsasink_node;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  this->stream = SND_PCM_STREAM_PLAYBACK;
  reset_alsa_sink_props (&this->props[1]);

  this->status.flags = SPA_PORT_STATUS_FLAG_NONE;

  for (i = 0; info && i < info->n_items; i++) {
    if (!strcmp (info->items[i].key, "alsa.card")) {
      snprintf (this->props[1].device, 63, "hw:%s", info->items[i].value);
      this->props[1].props.unset_mask &= ~1;
    }
  }

  update_state (this, SPA_NODE_STATE_CONFIGURE);

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo alsa_sink_interfaces[] =
{
  { SPA_NODE_URI, },
};

static SpaResult
alsa_sink_enum_interface_info (const SpaHandleFactory  *factory,
                               const SpaInterfaceInfo **info,
                               void                   **state)
{
  int index;

  if (factory == NULL || info == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      *info = &alsa_sink_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_alsa_sink_factory =
{ "alsa-sink",
  NULL,
  sizeof (SpaALSASink),
  alsa_sink_init,
  alsa_sink_enum_interface_info,
};
