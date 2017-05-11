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

static const char default_device[] = "hw:0";
static const uint32_t default_min_latency = 1024;

static void
reset_alsa_props (SpaALSAProps *props)
{
  strncpy (props->device, default_device, 64);
  props->min_latency = default_min_latency;
}

static SpaResult
spa_alsa_source_node_get_props (SpaNode       *node,
                                SpaProps     **props)
{
  SpaALSASource *this;
  SpaPODBuilder b = { NULL,  };
  SpaPODFrame f[2];

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (props != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  spa_pod_builder_init (&b, this->props_buffer, sizeof (this->props_buffer));

  spa_pod_builder_props (&b, &f[0], this->type.props,
    PROP    (&f[1], this->type.prop_device,      -SPA_POD_TYPE_STRING, this->props.device, sizeof (this->props.device)),
    PROP    (&f[1], this->type.prop_device_name, -SPA_POD_TYPE_STRING, this->props.device_name, sizeof (this->props.device_name)),
    PROP    (&f[1], this->type.prop_card_name,   -SPA_POD_TYPE_STRING, this->props.card_name, sizeof (this->props.card_name)),
    PROP_MM (&f[1], this->type.prop_min_latency,  SPA_POD_TYPE_INT,    this->props.min_latency, 1, INT32_MAX));

  *props = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaProps);

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_set_props (SpaNode         *node,
                                const SpaProps  *props)
{
  SpaALSASource *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  if (props == NULL) {
    reset_alsa_props (&this->props);
    return SPA_RESULT_OK;
  } else {
    spa_props_query (props,
        this->type.prop_device,      -SPA_POD_TYPE_STRING, this->props.device, sizeof (this->props.device),
        this->type.prop_min_latency,  SPA_POD_TYPE_INT,    &this->props.min_latency,
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

  this->callbacks.event (&this->node, data, this->user_data);

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

  res = spa_alsa_start (this, false);

  if (async) {
    SpaEventNodeAsyncComplete ac = SPA_EVENT_NODE_ASYNC_COMPLETE_INIT (this->type.event_node.AsyncComplete,
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

  res = spa_alsa_pause (this, false);

  if (async) {
    SpaEventNodeAsyncComplete ac = SPA_EVENT_NODE_ASYNC_COMPLETE_INIT (this->type.event_node.AsyncComplete,
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

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (command != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  if (SPA_COMMAND_TYPE (command) == this->type.command_node.Start) {
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
  else if (SPA_COMMAND_TYPE (command) == this->type.command_node.Pause) {
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
spa_alsa_source_node_set_callbacks (SpaNode                *node,
                                    const SpaNodeCallbacks *callbacks,
                                    size_t                  callbacks_size,
                                    void                   *user_data)
{
  SpaALSASource *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  this->callbacks = *callbacks;
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
  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

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
  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

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

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  return spa_alsa_enum_format (this, format, filter, index);
}

static void
recycle_buffer (SpaALSASource *this, uint32_t buffer_id)
{
  SpaALSABuffer *b;

  spa_log_trace (this->log, "alsa-source %p: recycle buffer %u", this, buffer_id);

  b = &this->buffers[buffer_id];
  spa_return_if_fail (b->outstanding);

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
spa_alsa_source_node_port_set_format (SpaNode         *node,
                                      SpaDirection     direction,
                                      uint32_t         port_id,
                                      uint32_t         flags,
                                      const SpaFormat *format)
{
  SpaALSASource *this;
  SpaPODBuilder b = { NULL };
  SpaPODFrame f[2];

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (format == NULL) {
    spa_alsa_pause (this, false);
    spa_alsa_clear_buffers (this);
    spa_alsa_close (this);
    this->have_format = false;
  } else {
    SpaAudioInfo info = { SPA_FORMAT_MEDIA_TYPE (format),
                          SPA_FORMAT_MEDIA_SUBTYPE (format), };

    if (info.media_type != this->type.media_type.audio ||
        info.media_subtype != this->type.media_subtype.raw)
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    if (!spa_format_audio_raw_parse (format, &info.info.raw, &this->type.format_audio))
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    if (spa_alsa_set_format (this, &info, flags) < 0)
      return SPA_RESULT_ERROR;

    this->current_format = info;
    this->have_format = true;
  }

  if (this->have_format) {
    this->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                       SPA_PORT_INFO_FLAG_LIVE;
    this->info.maxbuffering = this->buffer_frames * this->frame_size;
    this->info.latency = (this->period_frames * SPA_NSEC_PER_SEC) / this->rate;
    this->info.n_params = 2;
    this->info.params = this->params;

    spa_pod_builder_init (&b, this->params_buffer, sizeof (this->params_buffer));
    spa_pod_builder_object (&b, &f[0], 0, this->type.alloc_param_buffers.Buffers,
        PROP    (&f[1], this->type.alloc_param_buffers.size,    SPA_POD_TYPE_INT,
                                                        this->props.min_latency * this->frame_size),
        PROP    (&f[1], this->type.alloc_param_buffers.stride,  SPA_POD_TYPE_INT, 0),
        PROP_MM (&f[1], this->type.alloc_param_buffers.buffers, SPA_POD_TYPE_INT, 32, 1, 32),
        PROP    (&f[1], this->type.alloc_param_buffers.align,   SPA_POD_TYPE_INT, 16));
    this->params[0] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

    spa_pod_builder_object (&b, &f[0], 0, this->type.alloc_param_meta_enable.MetaEnable,
        PROP    (&f[1], this->type.alloc_param_meta_enable.type, SPA_POD_TYPE_ID, this->type.meta.Header),
        PROP    (&f[1], this->type.alloc_param_meta_enable.size, SPA_POD_TYPE_INT, sizeof (SpaMetaHeader)));
    this->params[1] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

    this->info.extra = NULL;
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_port_get_format (SpaNode          *node,
                                      SpaDirection      direction,
                                      uint32_t          port_id,
                                      const SpaFormat **format)
{
  SpaALSASource *this;
  SpaPODBuilder b = { NULL, };
  SpaPODFrame f[2];

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  spa_pod_builder_init (&b, this->format_buffer, sizeof (this->format_buffer));
  spa_pod_builder_format (&b, &f[0], this->type.format,
         this->type.media_type.audio, this->type.media_subtype.raw,
         PROP (&f[1], this->type.format_audio.format,   SPA_POD_TYPE_ID,  this->current_format.info.raw.format),
         PROP (&f[1], this->type.format_audio.rate,     SPA_POD_TYPE_INT, this->current_format.info.raw.rate),
         PROP (&f[1], this->type.format_audio.channels, SPA_POD_TYPE_INT, this->current_format.info.raw.channels));

  *format = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaFormat);

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_port_get_info (SpaNode            *node,
                                    SpaDirection        direction,
                                    uint32_t            port_id,
                                    const SpaPortInfo **info)
{
  SpaALSASource *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

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

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  if (this->n_buffers > 0) {
    spa_alsa_pause (this, false);
    if ((res = spa_alsa_clear_buffers (this)) < 0)
      return res;
  }
  for (i = 0; i < n_buffers; i++) {
    SpaALSABuffer *b = &this->buffers[i];
    SpaData *d = buffers[i]->datas;

    b->outbuf = buffers[i];
    b->outstanding = false;

    b->h = spa_buffer_find_meta (b->outbuf, this->type.meta.Header);

    if (!((d[0].type == this->type.data.MemFd ||
           d[0].type == this->type.data.DmaBuf ||
           d[0].type == this->type.data.MemPtr) &&
          d[0].data != NULL)) {
      spa_log_error (this->log, "alsa-source: need mapped memory");
      return SPA_RESULT_ERROR;
    }
    spa_list_insert (this->free.prev, &b->link);
  }
  this->n_buffers = n_buffers;

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

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (buffers != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (this->n_buffers == 0)
    return SPA_RESULT_NO_FORMAT;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_node_port_set_io (SpaNode       *node,
                                  SpaDirection   direction,
                                  uint32_t       port_id,
                                  SpaPortIO     *io)
{
  SpaALSASource *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  this->io = io;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_port_reuse_buffer (SpaNode         *node,
                                        uint32_t         port_id,
                                        uint32_t         buffer_id)
{
  SpaALSASource *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  spa_return_val_if_fail (port_id == 0, SPA_RESULT_INVALID_PORT);

  if (this->n_buffers == 0)
    return SPA_RESULT_NO_BUFFERS;

  if (buffer_id >= this->n_buffers)
    return SPA_RESULT_INVALID_BUFFER_ID;

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

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (SPA_COMMAND_TYPE (command) == this->type.command_node.Pause) {
    res = spa_alsa_pause (this, false);
  } else if (SPA_COMMAND_TYPE (command) == this->type.command_node.Start) {
    res = spa_alsa_start (this, false);
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
  SpaALSASource *this;
  SpaPortIO *io;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaALSASource, node);
  io = this->io;
  spa_return_val_if_fail (io != NULL, SPA_RESULT_WRONG_STATE);

  if (io->status == SPA_RESULT_HAVE_BUFFER)
    return SPA_RESULT_HAVE_BUFFER;

  if (io->buffer_id != SPA_ID_INVALID) {
    recycle_buffer (this, io->buffer_id);
    io->buffer_id = SPA_ID_INVALID;
  }
  return SPA_RESULT_OK;
}

static const SpaNode alsasource_node = {
  sizeof (SpaNode),
  NULL,
  spa_alsa_source_node_get_props,
  spa_alsa_source_node_set_props,
  spa_alsa_source_node_send_command,
  spa_alsa_source_node_set_callbacks,
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
  spa_alsa_source_node_port_set_io,
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

  spa_return_val_if_fail (clock != NULL, SPA_RESULT_INVALID_ARGUMENTS);

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

  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (interface != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = (SpaALSASource *) handle;

  if (interface_id == this->type.node)
    *interface = &this->node;
  else if (interface_id == this->type.clock)
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

  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  handle->get_interface = spa_alsa_source_get_interface;
  handle->clear = alsa_source_clear;

  this = (SpaALSASource *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].type, SPA_TYPE__TypeMap) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE__Log) == 0)
      this->log = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE_LOOP__DataLoop) == 0)
      this->data_loop = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE_LOOP__MainLoop) == 0)
      this->main_loop = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "an id-map is needed");
    return SPA_RESULT_ERROR;
  }
  if (this->data_loop == NULL) {
    spa_log_error (this->log, "a data loop is needed");
    return SPA_RESULT_ERROR;
  }
  if (this->main_loop == NULL) {
    spa_log_error (this->log, "a main loop is needed");
    return SPA_RESULT_ERROR;
  }
  init_type (&this->type, this->map);

  this->node = alsasource_node;
  this->clock = alsasource_clock;
  this->stream = SND_PCM_STREAM_CAPTURE;
  reset_alsa_props (&this->props);

  spa_list_init (&this->free);
  spa_list_init (&this->ready);

  for (i = 0; info && i < info->n_items; i++) {
    if (!strcmp (info->items[i].key, "alsa.card")) {
      snprintf (this->props.device, 63, "%s", info->items[i].value);
    }
  }
  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo alsa_source_interfaces[] =
{
  { SPA_TYPE__Node, },
  { SPA_TYPE__Clock, },
};

static SpaResult
alsa_source_enum_interface_info (const SpaHandleFactory  *factory,
                                 const SpaInterfaceInfo **info,
                                 uint32_t                 index)
{
  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

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
