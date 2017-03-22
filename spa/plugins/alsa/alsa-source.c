/* Spa ALSA Source
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
#include <spa/list.h>
#include <spa/audio/format.h>
#include <lib/props.h>

#include "alsa-utils.h"

#define CHECK_PORT(this,d,p)    ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)

typedef struct _SpaALSAState SpaALSASource;

static void
update_state (SpaALSASource *this, SpaNodeState state)
{
  this->node.state = state;
}

static const char default_device[] = "hw:0";
static const uint32_t default_period_size = 32;
static const uint32_t default_periods = 2;
static const bool default_period_event = 0;

static void
reset_alsa_props (SpaALSAProps *props)
{
  strncpy (props->device, default_device, 64);
  props->period_size = default_period_size;
  props->periods = default_periods;
  props->period_event = default_period_event;
}

enum {
  PROP_ID_NONE,
  PROP_ID_DEVICE,
  PROP_ID_DEVICE_NAME,
  PROP_ID_CARD_NAME,
  PROP_ID_PERIOD_SIZE,
  PROP_ID_PERIODS,
  PROP_ID_PERIOD_EVENT,
};

#define PROP(f,key,type,...)                                                    \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_READWRITE,type,1,__VA_ARGS__)
#define PROP_MM(f,key,type,...)                                                 \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_READWRITE |                     \
                              SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_U_MM(f,key,type,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_READWRITE |                     \
                              SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_EN(f,key,type,n,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_READWRITE |                     \
                              SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)
#define PROP_U_EN(f,key,type,n,...)                                             \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_READWRITE |                     \
                              SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)

static SpaResult
spa_alsa_source_node_get_props (SpaNode       *node,
                                SpaProps     **props)
{
  SpaALSASource *this;
  SpaPODBuilder b = { NULL,  };
  SpaPODFrame f[2];

  if (node == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  spa_pod_builder_init (&b, this->props_buffer, sizeof (this->props_buffer));

  spa_pod_builder_props (&b, &f[0],
        PROP    (&f[1], PROP_ID_DEVICE,      -SPA_POD_TYPE_STRING, this->props.device, sizeof (this->props.device)),
        PROP    (&f[1], PROP_ID_DEVICE_NAME, -SPA_POD_TYPE_STRING, this->props.device_name, sizeof (this->props.device_name)),
        PROP    (&f[1], PROP_ID_CARD_NAME,   -SPA_POD_TYPE_STRING, this->props.card_name, sizeof (this->props.card_name)),
        PROP_MM (&f[1], PROP_ID_PERIOD_SIZE,  SPA_POD_TYPE_INT,    this->props.period_size, 1, INT32_MAX),
        PROP_MM (&f[1], PROP_ID_PERIODS,      SPA_POD_TYPE_INT,    this->props.periods, 1, INT32_MAX),
        PROP    (&f[1], PROP_ID_PERIOD_EVENT, SPA_POD_TYPE_BOOL,   this->props.period_event));

  *props = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaProps);

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_set_props (SpaNode         *node,
                                const SpaProps  *props)
{
  SpaALSASource *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  if (props == NULL) {
    reset_alsa_props (&this->props);
    return SPA_RESULT_OK;
  } else {
    spa_props_query (props,
        PROP_ID_DEVICE,      -SPA_POD_TYPE_STRING, this->props.device, sizeof (this->props.device),
        PROP_ID_PERIOD_SIZE,  SPA_POD_TYPE_INT,    &this->props.period_size,
        PROP_ID_PERIODS,      SPA_POD_TYPE_INT,    &this->props.periods,
        PROP_ID_PERIOD_EVENT, SPA_POD_TYPE_BOOL,   &this->props.period_event,
        0);
  }

  return SPA_RESULT_OK;
}

static SpaResult
do_send_event (SpaLoop        *loop,
               bool            async,
               uint32_t        seq,
               size_t          size,
               void           *data,
               void           *user_data)
{
  SpaALSASource *this = user_data;

  this->event_cb (&this->node, data, this->user_data);

  return SPA_RESULT_OK;
}

static SpaResult
do_start (SpaLoop        *loop,
          bool            async,
          uint32_t        seq,
          size_t          size,
          void           *data,
          void           *user_data)
{
  SpaALSASource *this = user_data;
  SpaResult res;

  if (SPA_RESULT_IS_OK (res = spa_alsa_start (this, false))) {
    update_state (this, SPA_NODE_STATE_STREAMING);
  }

  if (async) {
    SpaNodeEventAsyncComplete ac = SPA_NODE_EVENT_ASYNC_COMPLETE_INIT (this->uri.node_events.AsyncComplete,
                                                                       seq, res);
    spa_loop_invoke (this->main_loop,
                     do_send_event,
                     SPA_ID_INVALID,
                     sizeof (ac),
                     &ac,
                     this);
  }
  return res;
}

static SpaResult
do_pause (SpaLoop        *loop,
          bool            async,
          uint32_t        seq,
          size_t          size,
          void           *data,
          void           *user_data)
{
  SpaALSASource *this = user_data;
  SpaResult res;

  if (SPA_RESULT_IS_OK (res = spa_alsa_pause (this, false))) {
    update_state (this, SPA_NODE_STATE_PAUSED);
  }

  if (async) {
    SpaNodeEventAsyncComplete ac = SPA_NODE_EVENT_ASYNC_COMPLETE_INIT (this->uri.node_events.AsyncComplete,
                                                                       seq, res);
    spa_loop_invoke (this->main_loop,
                     do_send_event,
                     SPA_ID_INVALID,
                     sizeof (ac),
                     &ac,
                     this);
  }
  return res;
}

static SpaResult
spa_alsa_source_node_send_command (SpaNode    *node,
                                   SpaCommand *command)
{
  SpaALSASource *this;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  if (SPA_COMMAND_TYPE (command) == this->uri.node_commands.Start) {
    if (!this->have_format)
      return SPA_RESULT_NO_FORMAT;

    if (this->n_buffers == 0)
      return SPA_RESULT_NO_BUFFERS;

    return spa_loop_invoke (this->data_loop,
                            do_start,
                            ++this->seq,
                            0,
                            NULL,
                            this);
  }
  else if (SPA_COMMAND_TYPE (command) == this->uri.node_commands.Pause) {
    if (!this->have_format)
      return SPA_RESULT_NO_FORMAT;

    if (this->n_buffers == 0)
      return SPA_RESULT_NO_BUFFERS;

    return spa_loop_invoke (this->data_loop,
                            do_pause,
                            ++this->seq,
                            0,
                            NULL,
                            this);
  }
  else
    return SPA_RESULT_NOT_IMPLEMENTED;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_set_event_callback (SpaNode              *node,
                                       SpaNodeEventCallback  event,
                                       void                 *user_data)
{
  SpaALSASource *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_get_n_ports (SpaNode       *node,
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
    *max_input_ports = 0;
  if (n_output_ports)
    *n_output_ports = 1;
  if (max_output_ports)
    *max_output_ports = 1;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_get_port_ids (SpaNode       *node,
                                   uint32_t       n_input_ports,
                                   uint32_t      *input_ids,
                                   uint32_t       n_output_ports,
                                   uint32_t      *output_ids)
{
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_output_ports > 0 && output_ids != NULL)
    output_ids[0] = 0;

  return SPA_RESULT_OK;
}


static SpaResult
spa_alsa_source_node_add_port (SpaNode        *node,
                               SpaDirection    direction,
                               uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_node_remove_port (SpaNode        *node,
                                  SpaDirection    direction,
                                  uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_node_port_enum_formats (SpaNode         *node,
                                        SpaDirection     direction,
                                        uint32_t         port_id,
                                        SpaFormat      **format,
                                        const SpaFormat *filter,
                                        uint32_t         index)
{
  SpaALSASource *this;
  SpaResult res;
  SpaFormat *fmt;
  uint8_t buffer[1024];
  SpaPODBuilder b = { NULL, };
  SpaPODFrame f[2];

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

next:
  spa_pod_builder_init (&b, buffer, sizeof (buffer));

  switch (index++) {
    case 0:
      spa_pod_builder_format (&b, &f[0], this->uri.format,
        this->uri.media_types.audio, this->uri.media_subtypes.raw,
        PROP_U_EN (&f[1], this->uri.prop_audio.format,   SPA_POD_TYPE_URI, 3, this->uri.audio_formats.S16,
                                                                              this->uri.audio_formats.S16,
                                                                              this->uri.audio_formats.S32),
        PROP_U_MM (&f[1], this->uri.prop_audio.rate,     SPA_POD_TYPE_INT, 44100, 1, INT32_MAX),
        PROP_U_MM (&f[1], this->uri.prop_audio.channels, SPA_POD_TYPE_INT, 2,     1, INT32_MAX));
      break;
    case 1:
      spa_pod_builder_format (&b, &f[0], this->uri.format,
        this->uri.media_types.audio, this->uri.media_subtypes_audio.aac,
        SPA_POD_TYPE_NONE);
    default:
      return SPA_RESULT_ENUM_END;
  }
  fmt = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaFormat);

  spa_pod_builder_init (&b, this->format_buffer, sizeof (this->format_buffer));

  if ((res = spa_format_filter (fmt, filter, &b)) != SPA_RESULT_OK)
    goto next;

  *format = SPA_MEMBER (this->format_buffer, 0, SpaFormat);

  return SPA_RESULT_OK;
}

static void
recycle_buffer (SpaALSASource *this, uint32_t buffer_id)
{
  SpaALSABuffer *b;

  b = &this->buffers[buffer_id];
  if (!b->outstanding)
    return;

  b->outstanding = false;
  spa_list_insert (this->free.prev, &b->link);
}

static SpaResult
spa_alsa_clear_buffers (SpaALSASource *this)
{
  if (this->n_buffers > 0) {
    spa_list_init (&this->free);
    spa_list_init (&this->ready);
    this->n_buffers = 0;
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_port_set_format (SpaNode            *node,
                                      SpaDirection        direction,
                                      uint32_t            port_id,
                                      SpaPortFormatFlags  flags,
                                      const SpaFormat    *format)
{
  SpaALSASource *this;
  SpaResult res;
  SpaPODBuilder b = { NULL };
  SpaPODFrame f[2];

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (format == NULL) {
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
  this->info.n_params = 2;
  this->info.params = this->params;

  spa_pod_builder_init (&b, this->params_buffer, sizeof (this->params_buffer));
  spa_pod_builder_object (&b, &f[0], 0, SPA_ALLOC_PARAM_TYPE_BUFFERS,
      PROP    (&f[1], SPA_ALLOC_PARAM_BUFFERS_SIZE,    SPA_POD_TYPE_INT, this->period_frames * this->frame_size),
      PROP    (&f[1], SPA_ALLOC_PARAM_BUFFERS_STRIDE,  SPA_POD_TYPE_INT, 0),
      PROP_MM (&f[1], SPA_ALLOC_PARAM_BUFFERS_BUFFERS, SPA_POD_TYPE_INT, 32, 1, 32),
      PROP    (&f[1], SPA_ALLOC_PARAM_BUFFERS_ALIGN,   SPA_POD_TYPE_INT, 16));
  this->params[0] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

  spa_pod_builder_object (&b, &f[0], 0, SPA_ALLOC_PARAM_TYPE_META_ENABLE,
      PROP    (&f[1], SPA_ALLOC_PARAM_META_ENABLE_TYPE, SPA_POD_TYPE_INT, SPA_META_TYPE_HEADER));
  this->params[1] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

  this->info.extra = NULL;

  this->have_format = true;

  update_state (this, SPA_NODE_STATE_READY);

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_port_get_format (SpaNode          *node,
                                      SpaDirection      direction,
                                      uint32_t          port_id,
                                      const SpaFormat **format)
{
  SpaALSASource *this;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = NULL;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_port_get_info (SpaNode            *node,
                                    SpaDirection        direction,
                                    uint32_t            port_id,
                                    const SpaPortInfo **info)
{
  SpaALSASource *this;

  if (node == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  *info = &this->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_port_get_props (SpaNode       *node,
                                     SpaDirection   direction,
                                     uint32_t       port_id,
                                     SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_node_port_set_props (SpaNode         *node,
                                     SpaDirection     direction,
                                     uint32_t         port_id,
                                     const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_node_port_use_buffers (SpaNode         *node,
                                       SpaDirection     direction,
                                       uint32_t         port_id,
                                       SpaBuffer      **buffers,
                                       uint32_t         n_buffers)
{
  SpaALSASource *this;
  SpaResult res;
  int i;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  if (this->n_buffers > 0) {
    if ((res = spa_alsa_clear_buffers (this)) < 0)
      return res;
  }
  for (i = 0; i < n_buffers; i++) {
    SpaALSABuffer *b = &this->buffers[i];
    b->outbuf = buffers[i];
    b->outstanding = false;

    b->h = spa_buffer_find_meta (b->outbuf, SPA_META_TYPE_HEADER);

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
    spa_list_insert (this->free.prev, &b->link);
  }
  this->n_buffers = n_buffers;

  if (this->n_buffers > 0)
    update_state (this, SPA_NODE_STATE_PAUSED);
  else
    update_state (this, SPA_NODE_STATE_READY);

  return SPA_RESULT_OK;
}


static SpaResult
spa_alsa_source_node_port_alloc_buffers (SpaNode         *node,
                                         SpaDirection     direction,
                                         uint32_t         port_id,
                                         SpaAllocParam  **params,
                                         uint32_t         n_params,
                                         SpaBuffer      **buffers,
                                         uint32_t        *n_buffers)
{
  SpaALSASource *this;

  if (node == NULL || buffers == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  if (this->n_buffers == 0)
    return SPA_RESULT_NO_FORMAT;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_node_port_set_input (SpaNode      *node,
                                     uint32_t      port_id,
                                     SpaPortInput *input)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_node_port_set_output (SpaNode       *node,
                                      uint32_t       port_id,
                                      SpaPortOutput *output)
{
  SpaALSASource *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  if (!CHECK_PORT (this, SPA_DIRECTION_OUTPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->io = output;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_port_reuse_buffer (SpaNode         *node,
                                        uint32_t         port_id,
                                        uint32_t         buffer_id)
{
  SpaALSASource *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  if (this->n_buffers == 0)
    return SPA_RESULT_NO_BUFFERS;

  if (buffer_id >= this->n_buffers)
    return SPA_RESULT_INVALID_BUFFER_ID;

  spa_log_trace (this->log, "recycle buffer %u", buffer_id);
  recycle_buffer (this, buffer_id);

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_port_send_command (SpaNode          *node,
                                        SpaDirection      direction,
                                        uint32_t          port_id,
                                        SpaCommand       *command)
{
  SpaALSASource *this;
  SpaResult res;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  if (SPA_COMMAND_TYPE (command) == this->uri.node_commands.Pause) {
    if (SPA_RESULT_IS_OK (res = spa_alsa_pause (this, false))) {
      update_state (this, SPA_NODE_STATE_PAUSED);
    }
  } else if (SPA_COMMAND_TYPE (command) == this->uri.node_commands.Start) {
    if (SPA_RESULT_IS_OK (res = spa_alsa_start (this, false))) {
      update_state (this, SPA_NODE_STATE_STREAMING);
    }
  } else
    res = SPA_RESULT_NOT_IMPLEMENTED;

  return res;
}

static SpaResult
spa_alsa_source_node_process_input (SpaNode *node)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_node_process_output (SpaNode *node)
{
  return SPA_RESULT_OK;
}

static const SpaNode alsasource_node = {
  sizeof (SpaNode),
  NULL,
  SPA_NODE_STATE_INIT,
  spa_alsa_source_node_get_props,
  spa_alsa_source_node_set_props,
  spa_alsa_source_node_send_command,
  spa_alsa_source_node_set_event_callback,
  spa_alsa_source_node_get_n_ports,
  spa_alsa_source_node_get_port_ids,
  spa_alsa_source_node_add_port,
  spa_alsa_source_node_remove_port,
  spa_alsa_source_node_port_enum_formats,
  spa_alsa_source_node_port_set_format,
  spa_alsa_source_node_port_get_format,
  spa_alsa_source_node_port_get_info,
  spa_alsa_source_node_port_get_props,
  spa_alsa_source_node_port_set_props,
  spa_alsa_source_node_port_use_buffers,
  spa_alsa_source_node_port_alloc_buffers,
  spa_alsa_source_node_port_set_input,
  spa_alsa_source_node_port_set_output,
  spa_alsa_source_node_port_reuse_buffer,
  spa_alsa_source_node_port_send_command,
  spa_alsa_source_node_process_input,
  spa_alsa_source_node_process_output,
};

static SpaResult
spa_alsa_source_clock_get_props (SpaClock  *clock,
                                 SpaProps **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_clock_set_props (SpaClock       *clock,
                                 const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_clock_get_time (SpaClock         *clock,
                                int32_t          *rate,
                                int64_t          *ticks,
                                int64_t          *monotonic_time)
{
  SpaALSASource *this;

  if (clock == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (clock, SpaALSASource, clock);

  if (rate)
    *rate = SPA_USEC_PER_SEC;
  if (ticks)
    *ticks = this->last_ticks;
  if (monotonic_time)
    *monotonic_time = this->last_monotonic;

  return SPA_RESULT_OK;
}

static const SpaClock alsasource_clock = {
  sizeof (SpaClock),
  NULL,
  SPA_CLOCK_STATE_STOPPED,
  spa_alsa_source_clock_get_props,
  spa_alsa_source_clock_set_props,
  spa_alsa_source_clock_get_time,
};

static SpaResult
spa_alsa_source_get_interface (SpaHandle               *handle,
                             uint32_t                 interface_id,
                             void                   **interface)
{
  SpaALSASource *this;

  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASource *) handle;

  if (interface_id == this->uri.node)
    *interface = &this->node;
  else if (interface_id == this->uri.clock)
    *interface = &this->clock;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static SpaResult
alsa_source_clear (SpaHandle *handle)
{
  return SPA_RESULT_OK;
}

static SpaResult
alsa_source_init (const SpaHandleFactory  *factory,
                  SpaHandle               *handle,
                  const SpaDict           *info,
                  const SpaSupport        *support,
                  uint32_t                 n_support)
{
  SpaALSASource *this;
  uint32_t i;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_alsa_source_get_interface;
  handle->clear = alsa_source_clear;

  this = (SpaALSASource *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].uri, SPA_ID_MAP_URI) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].uri, SPA_LOG_URI) == 0)
      this->log = support[i].data;
    else if (strcmp (support[i].uri, SPA_LOOP__DataLoop) == 0)
      this->data_loop = support[i].data;
    else if (strcmp (support[i].uri, SPA_LOOP__MainLoop) == 0)
      this->main_loop = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "an id-map is needed");
    return SPA_RESULT_ERROR;
  }
  this->uri.node = spa_id_map_get_id (this->map, SPA_NODE_URI);
  this->uri.clock = spa_id_map_get_id (this->map, SPA_CLOCK_URI);
  this->uri.format = spa_id_map_get_id (this->map, SPA_FORMAT_URI);

  spa_media_types_fill (&this->uri.media_types, this->map);
  spa_media_subtypes_map (this->map, &this->uri.media_subtypes);
  spa_media_subtypes_audio_map (this->map, &this->uri.media_subtypes_audio);
  spa_prop_audio_map (this->map, &this->uri.prop_audio);
  spa_audio_formats_map (this->map, &this->uri.audio_formats);
  spa_node_events_map (this->map, &this->uri.node_events);
  spa_node_commands_map (this->map, &this->uri.node_commands);

  this->node = alsasource_node;
  this->clock = alsasource_clock;
  this->stream = SND_PCM_STREAM_CAPTURE;
  reset_alsa_props (&this->props);

  spa_list_init (&this->free);
  spa_list_init (&this->ready);

  for (i = 0; info && i < info->n_items; i++) {
    if (!strcmp (info->items[i].key, "alsa.card")) {
      snprintf (this->props.device, 63, "hw:%s", info->items[i].value);
    }
  }

  update_state (this, SPA_NODE_STATE_CONFIGURE);

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo alsa_source_interfaces[] =
{
  { SPA_NODE_URI, },
  { SPA_CLOCK_URI, },
};

static SpaResult
alsa_source_enum_interface_info (const SpaHandleFactory  *factory,
                                 const SpaInterfaceInfo **info,
                                 uint32_t                 index)
{
  if (factory == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (index < 0 || index >= SPA_N_ELEMENTS (alsa_source_interfaces))
    return SPA_RESULT_ENUM_END;

  *info = &alsa_source_interfaces[index];

  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_alsa_source_factory =
{ "alsa-source",
  NULL,
  sizeof (SpaALSASource),
  alsa_source_init,
  alsa_source_enum_interface_info,
};
