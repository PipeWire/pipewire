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

#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/timerfd.h>

#include <spa/type-map.h>
#include <spa/clock.h>
#include <spa/log.h>
#include <spa/loop.h>
#include <spa/node.h>
#include <spa/param-alloc.h>
#include <spa/list.h>
#include <spa/audio/format-utils.h>
#include <spa/format-builder.h>
#include <lib/props.h>

#define SAMPLES_TO_TIME(this,s)   ((s) * SPA_NSEC_PER_SEC / (this)->current_format.info.raw.rate)
#define BYTES_TO_SAMPLES(this,b)  ((b)/(this)->bpf)
#define BYTES_TO_TIME(this,b)     SAMPLES_TO_TIME(this, BYTES_TO_SAMPLES (this, b))

typedef struct {
  uint32_t node;
  uint32_t clock;
  uint32_t format;
  uint32_t props;
  uint32_t prop_live;
  uint32_t prop_wave;
  uint32_t prop_freq;
  uint32_t prop_volume;
  uint32_t wave_sine;
  uint32_t wave_square;
  SpaTypeMeta meta;
  SpaTypeData data;
  SpaTypeMediaType media_type;
  SpaTypeMediaSubtype media_subtype;
  SpaTypeFormatAudio format_audio;
  SpaTypeAudioFormat audio_format;
  SpaTypeEventNode event_node;
  SpaTypeCommandNode command_node;
  SpaTypeParamAllocBuffers param_alloc_buffers;
  SpaTypeParamAllocMetaEnable param_alloc_meta_enable;
} Type;

static inline void
init_type (Type *type, SpaTypeMap *map)
{
  type->node = spa_type_map_get_id (map, SPA_TYPE__Node);
  type->clock = spa_type_map_get_id (map, SPA_TYPE__Clock);
  type->format = spa_type_map_get_id (map, SPA_TYPE__Format);
  type->props = spa_type_map_get_id (map, SPA_TYPE__Props);
  type->prop_live = spa_type_map_get_id (map, SPA_TYPE_PROPS__live);
  type->prop_wave = spa_type_map_get_id (map, SPA_TYPE_PROPS__waveType);
  type->prop_freq = spa_type_map_get_id (map, SPA_TYPE_PROPS__frequency);
  type->prop_volume = spa_type_map_get_id (map, SPA_TYPE_PROPS__volume);
  type->wave_sine = spa_type_map_get_id (map, SPA_TYPE_PROPS__waveType ":sine");
  type->wave_square = spa_type_map_get_id (map, SPA_TYPE_PROPS__waveType ":square");
  spa_type_meta_map (map, &type->meta);
  spa_type_data_map (map, &type->data);
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_format_audio_map (map, &type->format_audio);
  spa_type_audio_format_map (map, &type->audio_format);
  spa_type_event_node_map (map, &type->event_node);
  spa_type_command_node_map (map, &type->command_node);
  spa_type_param_alloc_buffers_map (map, &type->param_alloc_buffers);
  spa_type_param_alloc_meta_enable_map (map, &type->param_alloc_meta_enable);
}

typedef struct _SpaAudioTestSrc SpaAudioTestSrc;

typedef struct {
  bool live;
  uint32_t wave;
  double freq;
  double volume;
} SpaAudioTestSrcProps;

#define MAX_BUFFERS 16

typedef struct _ATSBuffer ATSBuffer;

struct _ATSBuffer {
  SpaBuffer *outbuf;
  bool outstanding;
  SpaMetaHeader *h;
  SpaMetaRingbuffer *rb;
  SpaList link;
};

typedef SpaResult (*RenderFunc) (SpaAudioTestSrc *this, void *samples, size_t n_samples);

struct _SpaAudioTestSrc {
  SpaHandle handle;
  SpaNode node;
  SpaClock clock;

  Type type;
  SpaTypeMap *map;
  SpaLog *log;
  SpaLoop *data_loop;

  uint8_t props_buffer[512];
  SpaAudioTestSrcProps props;

  SpaNodeCallbacks callbacks;
  void *user_data;

  SpaSource timer_source;
  struct itimerspec timerspec;

  SpaPortInfo info;
  uint32_t params[2];
  uint8_t params_buffer[1024];
  SpaPortIO *io;

  bool have_format;
  SpaAudioInfo current_format;
  uint8_t format_buffer[1024];
  size_t bpf;
  RenderFunc render_func;
  double accumulator;

  ATSBuffer buffers[MAX_BUFFERS];
  uint32_t  n_buffers;

  bool started;
  uint64_t start_time;
  uint64_t elapsed_time;

  uint64_t sample_count;
  SpaList empty;
};

#define CHECK_PORT(this,d,p)  ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)

#define DEFAULT_LIVE true
#define DEFAULT_WAVE wave_sine
#define DEFAULT_FREQ 440.0
#define DEFAULT_VOLUME 1.0

static void
reset_audiotestsrc_props (SpaAudioTestSrc *this, SpaAudioTestSrcProps *props)
{
  props->live = DEFAULT_LIVE;
  props->wave = this->type. DEFAULT_WAVE;
  props->freq = DEFAULT_FREQ;
  props->volume = DEFAULT_VOLUME;
}

#define PROP(f,key,type,...)                                                    \
          SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
#define PROP_MM(f,key,type,...)                                                 \
          SPA_POD_PROP (f,key,SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_U_MM(f,key,type,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_EN(f,key,type,n,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)
#define PROP_U_EN(f,key,type,n,...)                                             \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)

static SpaResult
spa_audiotestsrc_node_get_props (SpaNode       *node,
                                 SpaProps     **props)
{
  SpaAudioTestSrc *this;
  SpaPODBuilder b = { NULL,  };
  SpaPODFrame f[2];

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (props != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  spa_pod_builder_init (&b, this->props_buffer, sizeof (this->props_buffer));
  spa_pod_builder_props (&b, &f[0], this->type.props,
    PROP    (&f[1], this->type.prop_live,   SPA_POD_TYPE_BOOL,   this->props.live),
    PROP_EN (&f[1], this->type.prop_wave,   SPA_POD_TYPE_ID,  3, this->props.wave,
                                                                this->type.wave_sine,
                                                                this->type.wave_square),
    PROP_MM (&f[1], this->type.prop_freq,   SPA_POD_TYPE_DOUBLE, this->props.freq,
                                                            0.0, 50000000.0),
    PROP_MM (&f[1], this->type.prop_volume, SPA_POD_TYPE_DOUBLE, this->props.volume,
                                                            0.0, 10.0));

  *props = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaProps);

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_set_props (SpaNode         *node,
                                 const SpaProps  *props)
{
  SpaAudioTestSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  if (props == NULL) {
    reset_audiotestsrc_props (this, &this->props);
  } else {
    spa_props_query (props,
        this->type.prop_live,   SPA_POD_TYPE_BOOL,   &this->props.live,
        this->type.prop_wave,   SPA_POD_TYPE_ID,     &this->props.wave,
        this->type.prop_freq,   SPA_POD_TYPE_DOUBLE, &this->props.freq,
        this->type.prop_volume, SPA_POD_TYPE_DOUBLE, &this->props.volume,
        0);
  }

  if (this->props.live)
    this->info.flags |= SPA_PORT_INFO_FLAG_LIVE;
  else
    this->info.flags &= ~SPA_PORT_INFO_FLAG_LIVE;

  return SPA_RESULT_OK;
}

static inline SpaResult
send_have_output (SpaAudioTestSrc *this)
{
  if (this->callbacks.have_output)
    this->callbacks.have_output (&this->node, this->user_data);
  return SPA_RESULT_OK;
}

#include "render.c"

static void
set_timer (SpaAudioTestSrc *this, bool enabled)
{
  if (this->callbacks.have_output || this->props.live) {
    if (enabled) {
      if (this->props.live) {
        uint64_t next_time = this->start_time + this->elapsed_time;
        this->timerspec.it_value.tv_sec = next_time / SPA_NSEC_PER_SEC;
        this->timerspec.it_value.tv_nsec = next_time % SPA_NSEC_PER_SEC;
      } else {
        this->timerspec.it_value.tv_sec = 0;
        this->timerspec.it_value.tv_nsec = 1;
      }
    } else {
      this->timerspec.it_value.tv_sec = 0;
      this->timerspec.it_value.tv_nsec = 0;
    }
    timerfd_settime (this->timer_source.fd, TFD_TIMER_ABSTIME, &this->timerspec, NULL);
  }
}

static void
read_timer (SpaAudioTestSrc *this)
{
  uint64_t expirations;

  if (this->callbacks.have_output || this->props.live) {
    if (read (this->timer_source.fd, &expirations, sizeof (uint64_t)) < sizeof (uint64_t))
      perror ("read timerfd");
  }
}

static SpaResult
audiotestsrc_make_buffer (SpaAudioTestSrc *this)
{
  ATSBuffer *b;
  SpaPortIO *io = this->io;
  int n_bytes, n_samples;

  read_timer (this);

  if (spa_list_is_empty (&this->empty)) {
    set_timer (this, false);
    return SPA_RESULT_OUT_OF_BUFFERS;
  }
  b = spa_list_first (&this->empty, ATSBuffer, link);
  spa_list_remove (&b->link);
  b->outstanding = true;

  n_bytes = b->outbuf->datas[0].maxsize;
  if (io->range.min_size != 0) {
    n_bytes = SPA_MIN (n_bytes, io->range.min_size);
    if (io->range.max_size < n_bytes)
      n_bytes = io->range.max_size;
  }

  spa_log_trace (this->log, "audiotestsrc %p: dequeue buffer %d %d %d", this, b->outbuf->id,
      b->outbuf->datas[0].maxsize, n_bytes);

  if (b->rb) {
    int32_t filled, avail;
    uint32_t index, offset;

    filled = spa_ringbuffer_get_write_index (&b->rb->ringbuffer, &index);
    avail = b->rb->ringbuffer.size - filled;
    n_bytes = SPA_MIN (avail, n_bytes);

    n_samples = n_bytes / this->bpf;

    offset = index & b->rb->ringbuffer.mask;

    if (offset + n_bytes > b->rb->ringbuffer.size) {
      uint32_t l0 = b->rb->ringbuffer.size - offset;
      this->render_func (this, SPA_MEMBER (b->outbuf->datas[0].data, offset, void), l0 / this->bpf);
      this->render_func (this, b->outbuf->datas[0].data, (n_bytes - l0) / this->bpf);
    } else {
      this->render_func (this, SPA_MEMBER (b->outbuf->datas[0].data, offset, void), n_samples);
    }
    spa_ringbuffer_write_update (&b->rb->ringbuffer, index + n_bytes);
  } else {
    n_samples = n_bytes / this->bpf;
    this->render_func (this, b->outbuf->datas[0].data, n_samples);
    b->outbuf->datas[0].chunk->size = n_bytes;
    b->outbuf->datas[0].chunk->offset = 0;
    b->outbuf->datas[0].chunk->stride = 0;
  }

  if (b->h) {
    b->h->seq = this->sample_count;
    b->h->pts = this->start_time + this->elapsed_time;
    b->h->dts_offset = 0;
  }

  this->sample_count += n_samples;
  this->elapsed_time = SAMPLES_TO_TIME (this, this->sample_count);
  set_timer (this, true);

  io->buffer_id = b->outbuf->id;
  io->status = SPA_RESULT_HAVE_BUFFER;

  return SPA_RESULT_HAVE_BUFFER;
}

static void
audiotestsrc_on_output (SpaSource *source)
{
  SpaAudioTestSrc *this = source->data;
  SpaResult res;

  res = audiotestsrc_make_buffer (this);

  if (res == SPA_RESULT_HAVE_BUFFER)
    send_have_output (this);
}

static SpaResult
spa_audiotestsrc_node_send_command (SpaNode    *node,
                                    SpaCommand *command)
{
  SpaAudioTestSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (command != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  if (SPA_COMMAND_TYPE (command) == this->type.command_node.Start) {
    struct timespec now;

    if (!this->have_format)
      return SPA_RESULT_NO_FORMAT;

    if (this->n_buffers == 0)
      return SPA_RESULT_NO_BUFFERS;

    if (this->started)
      return SPA_RESULT_OK;

    clock_gettime (CLOCK_MONOTONIC, &now);
    if (this->props.live)
      this->start_time = SPA_TIMESPEC_TO_TIME (&now);
    else
      this->start_time = 0;
    this->sample_count = 0;
    this->elapsed_time = 0;

    this->started = true;
    set_timer (this, true);
  }
  else if (SPA_COMMAND_TYPE (command) == this->type.command_node.Pause) {
    if (!this->have_format)
      return SPA_RESULT_NO_FORMAT;

    if (this->n_buffers == 0)
      return SPA_RESULT_NO_BUFFERS;

    if (!this->started)
      return SPA_RESULT_OK;

    this->started = false;
    set_timer (this, false);
  }
  else
    return SPA_RESULT_NOT_IMPLEMENTED;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_set_callbacks (SpaNode                *node,
                                     const SpaNodeCallbacks *callbacks,
                                     size_t                  callbacks_size,
                                     void                   *user_data)
{
  SpaAudioTestSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  if (this->data_loop == NULL && callbacks->have_output) {
    spa_log_error (this->log, "a data_loop is needed for async operation");
    return SPA_RESULT_ERROR;
  }

  this->callbacks = *callbacks;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_get_n_ports (SpaNode       *node,
                                   uint32_t      *n_input_ports,
                                   uint32_t      *max_input_ports,
                                   uint32_t      *n_output_ports,
                                   uint32_t      *max_output_ports)
{
  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  if (n_input_ports)
    *n_input_ports = 0;
  if (n_output_ports)
    *n_output_ports = 1;
  if (max_input_ports)
    *max_input_ports = 0;
  if (max_output_ports)
    *max_output_ports = 1;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_get_port_ids (SpaNode       *node,
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
spa_audiotestsrc_node_add_port (SpaNode        *node,
                                SpaDirection    direction,
                                uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_remove_port (SpaNode        *node,
                                   SpaDirection    direction,
                                   uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_port_enum_formats (SpaNode          *node,
                                         SpaDirection      direction,
                                         uint32_t          port_id,
                                         SpaFormat       **format,
                                         const SpaFormat  *filter,
                                         uint32_t          index)
{
  SpaAudioTestSrc *this;
  SpaResult res;
  SpaFormat *fmt;
  uint8_t buffer[256];
  SpaPODBuilder b = { NULL, };
  SpaPODFrame f[2];
  uint32_t count, match;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  count = match = filter ? 0 : index;

next:
  spa_pod_builder_init (&b, buffer, sizeof (buffer));

  switch (count++) {
    case 0:
      spa_pod_builder_format (&b, &f[0], this->type.format,
          this->type.media_type.audio, this->type.media_subtype.raw,
          PROP_U_EN (&f[1], this->type.format_audio.format,   SPA_POD_TYPE_ID,  5, this->type.audio_format.S16,
                                                                                this->type.audio_format.S16,
                                                                                this->type.audio_format.S32,
                                                                                this->type.audio_format.F32,
                                                                                this->type.audio_format.F64),
          PROP_U_MM (&f[1], this->type.format_audio.rate,     SPA_POD_TYPE_INT, 44100, 1, INT32_MAX),
          PROP_U_MM (&f[1], this->type.format_audio.channels, SPA_POD_TYPE_INT, 2,     1, INT32_MAX));
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  fmt = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaFormat);

  spa_pod_builder_init (&b, this->format_buffer, sizeof (this->format_buffer));

  if ((res = spa_format_filter (fmt, filter, &b)) != SPA_RESULT_OK || match++ != index)
    goto next;

  *format = SPA_POD_BUILDER_DEREF (&b, 0, SpaFormat);

  return SPA_RESULT_OK;
}

static SpaResult
clear_buffers (SpaAudioTestSrc *this)
{
  if (this->n_buffers > 0) {
    spa_log_info (this->log, "audiotestsrc %p: clear buffers", this);
    this->n_buffers = 0;
    spa_list_init (&this->empty);
    this->started = false;
    set_timer (this, false);
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_set_format (SpaNode         *node,
                                       SpaDirection     direction,
                                       uint32_t         port_id,
                                       uint32_t         flags,
                                       const SpaFormat *format)
{
  SpaAudioTestSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (format == NULL) {
    this->have_format = false;
    clear_buffers (this);
  } else {
    SpaAudioInfo info = { SPA_FORMAT_MEDIA_TYPE (format),
                          SPA_FORMAT_MEDIA_SUBTYPE (format), };
    int idx;
    int sizes[4] = { 2, 4, 4, 8 };

    if (info.media_type != this->type.media_type.audio ||
        info.media_subtype != this->type.media_subtype.raw)
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    if (!spa_format_audio_raw_parse (format, &info.info.raw, &this->type.format_audio))
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    if (info.info.raw.format == this->type.audio_format.S16)
      idx = 0;
    else if (info.info.raw.format == this->type.audio_format.S32)
      idx = 1;
    else if (info.info.raw.format == this->type.audio_format.F32)
      idx = 2;
    else if (info.info.raw.format == this->type.audio_format.F64)
      idx = 3;
    else
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    this->bpf = sizes[idx] * info.info.raw.channels;
    this->current_format = info;
    this->have_format = true;
    this->render_func =  sine_funcs[idx];
  }

  if (this->have_format) {
    this->info.direction = direction;
    this->info.port_id = port_id;
    this->info.rate = this->current_format.info.raw.rate;
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_get_format (SpaNode          *node,
                                       SpaDirection      direction,
                                       uint32_t          port_id,
                                       const SpaFormat **format)
{
  SpaAudioTestSrc *this;
  SpaPODBuilder b = { NULL, };
  SpaPODFrame f[2];

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

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
spa_audiotestsrc_node_port_get_info (SpaNode            *node,
                                     SpaDirection        direction,
                                     uint32_t            port_id,
                                     const SpaPortInfo **info)
{
  SpaAudioTestSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  *info = &this->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_enum_params (SpaNode       *node,
                                        SpaDirection   direction,
                                        uint32_t       port_id,
                                        uint32_t       index,
                                        SpaParam     **param)
{
  SpaAudioTestSrc *this;
  SpaPODBuilder b = { NULL, };
  SpaPODFrame f[2];

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (param != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  spa_pod_builder_init (&b, this->params_buffer, sizeof (this->params_buffer));

  switch (index) {
  case 0:
    spa_pod_builder_object (&b, &f[0], 0, this->type.param_alloc_buffers.Buffers,
      PROP      (&f[1], this->type.param_alloc_buffers.size,    SPA_POD_TYPE_INT, 1024 * this->bpf),
      PROP      (&f[1], this->type.param_alloc_buffers.stride,  SPA_POD_TYPE_INT, this->bpf),
      PROP_U_MM (&f[1], this->type.param_alloc_buffers.buffers, SPA_POD_TYPE_INT, 32, 2, 32),
      PROP      (&f[1], this->type.param_alloc_buffers.align,   SPA_POD_TYPE_INT, 16));
    break;

  case 1:
    spa_pod_builder_object (&b, &f[0], 0, this->type.param_alloc_meta_enable.MetaEnable,
      PROP      (&f[1], this->type.param_alloc_meta_enable.type, SPA_POD_TYPE_ID, this->type.meta.Header),
      PROP      (&f[1], this->type.param_alloc_meta_enable.size, SPA_POD_TYPE_INT, sizeof (SpaMetaHeader)));
    break;

  default:
    return SPA_RESULT_NOT_IMPLEMENTED;
  }

  *param = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaParam);

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_set_param (SpaNode        *node,
                                      SpaDirection    direction,
                                      uint32_t        port_id,
                                      const SpaParam *param)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_port_use_buffers (SpaNode         *node,
                                        SpaDirection     direction,
                                        uint32_t         port_id,
                                        SpaBuffer      **buffers,
                                        uint32_t         n_buffers)
{
  SpaAudioTestSrc *this;
  uint32_t i;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  clear_buffers (this);

  for (i = 0; i < n_buffers; i++) {
    ATSBuffer *b;
    SpaData *d = buffers[i]->datas;

    b = &this->buffers[i];
    b->outbuf = buffers[i];
    b->outstanding = false;
    b->h = spa_buffer_find_meta (buffers[i], this->type.meta.Header);
    b->rb = spa_buffer_find_meta (buffers[i], this->type.meta.Ringbuffer);

    if ((d[0].type == this->type.data.MemPtr ||
         d[0].type == this->type.data.MemFd ||
         d[0].type == this->type.data.DmaBuf) &&
        d[0].data == NULL) {
      spa_log_error (this->log, "audiotestsrc %p: invalid memory on buffer %p", this, buffers[i]);
    }
    spa_list_insert (this->empty.prev, &b->link);
  }
  this->n_buffers = n_buffers;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_alloc_buffers (SpaNode         *node,
                                          SpaDirection     direction,
                                          uint32_t         port_id,
                                          SpaParam       **params,
                                          uint32_t         n_params,
                                          SpaBuffer      **buffers,
                                          uint32_t        *n_buffers)
{
  SpaAudioTestSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_port_set_io (SpaNode       *node,
                                   SpaDirection   direction,
                                   uint32_t       port_id,
                                   SpaPortIO     *io)
{
  SpaAudioTestSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  this->io = io;

  return SPA_RESULT_OK;
}

static inline void
reuse_buffer (SpaAudioTestSrc *this, uint32_t id)
{
  ATSBuffer *b = &this->buffers[id];
  spa_return_if_fail (b->outstanding);

  spa_log_trace (this->log, "audiotestsrc %p: reuse buffer %d", this, id);

  b->outstanding = false;
  spa_list_insert (this->empty.prev, &b->link);

  if (!this->props.live)
    set_timer (this, true);
}

static SpaResult
spa_audiotestsrc_node_port_reuse_buffer (SpaNode         *node,
                                         uint32_t         port_id,
                                         uint32_t         buffer_id)
{
  SpaAudioTestSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  spa_return_val_if_fail (port_id == 0, SPA_RESULT_INVALID_PORT);
  spa_return_val_if_fail (this->n_buffers > 0, SPA_RESULT_NO_BUFFERS);
  spa_return_val_if_fail (buffer_id < this->n_buffers, SPA_RESULT_INVALID_BUFFER_ID);

  reuse_buffer (this, buffer_id);

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_send_command (SpaNode        *node,
                                         SpaDirection    direction,
                                         uint32_t        port_id,
                                         SpaCommand     *command)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_process_input (SpaNode *node)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_process_output (SpaNode *node)
{
  SpaAudioTestSrc *this;
  SpaPortIO *io;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);
  io = this->io;
  spa_return_val_if_fail (io != NULL, SPA_RESULT_WRONG_STATE);

  if (io->status == SPA_RESULT_HAVE_BUFFER)
    return SPA_RESULT_HAVE_BUFFER;

  if (io->buffer_id != SPA_ID_INVALID) {
    reuse_buffer (this, this->io->buffer_id);
    this->io->buffer_id = SPA_ID_INVALID;
  }

  if (!this->callbacks.have_output && (io->status == SPA_RESULT_NEED_BUFFER))
    return audiotestsrc_make_buffer (this);
  else
    return SPA_RESULT_OK;
}

static const SpaNode audiotestsrc_node = {
  sizeof (SpaNode),
  NULL,
  spa_audiotestsrc_node_get_props,
  spa_audiotestsrc_node_set_props,
  spa_audiotestsrc_node_send_command,
  spa_audiotestsrc_node_set_callbacks,
  spa_audiotestsrc_node_get_n_ports,
  spa_audiotestsrc_node_get_port_ids,
  spa_audiotestsrc_node_add_port,
  spa_audiotestsrc_node_remove_port,
  spa_audiotestsrc_node_port_enum_formats,
  spa_audiotestsrc_node_port_set_format,
  spa_audiotestsrc_node_port_get_format,
  spa_audiotestsrc_node_port_get_info,
  spa_audiotestsrc_node_port_enum_params,
  spa_audiotestsrc_node_port_set_param,
  spa_audiotestsrc_node_port_use_buffers,
  spa_audiotestsrc_node_port_alloc_buffers,
  spa_audiotestsrc_node_port_set_io,
  spa_audiotestsrc_node_port_reuse_buffer,
  spa_audiotestsrc_node_port_send_command,
  spa_audiotestsrc_node_process_input,
  spa_audiotestsrc_node_process_output,
};

static SpaResult
spa_audiotestsrc_clock_get_props (SpaClock  *clock,
                                  SpaProps **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_clock_set_props (SpaClock       *clock,
                                  const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_clock_get_time (SpaClock         *clock,
                                 int32_t          *rate,
                                 int64_t          *ticks,
                                 int64_t          *monotonic_time)
{
  struct timespec now;
  uint64_t tnow;

  spa_return_val_if_fail (clock != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  if (rate)
    *rate = SPA_NSEC_PER_SEC;

  clock_gettime (CLOCK_MONOTONIC, &now);
  tnow = SPA_TIMESPEC_TO_TIME (&now);

  if (ticks)
    *ticks = tnow;
  if (monotonic_time)
    *monotonic_time = tnow;

  return SPA_RESULT_OK;
}

static const SpaClock audiotestsrc_clock = {
  sizeof (SpaClock),
  NULL,
  SPA_CLOCK_STATE_STOPPED,
  spa_audiotestsrc_clock_get_props,
  spa_audiotestsrc_clock_set_props,
  spa_audiotestsrc_clock_get_time,
};

static SpaResult
spa_audiotestsrc_get_interface (SpaHandle         *handle,
                                uint32_t           interface_id,
                                void             **interface)
{
  SpaAudioTestSrc *this;

  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (interface != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = (SpaAudioTestSrc *) handle;

  if (interface_id == this->type.node)
    *interface = &this->node;
  else if (interface_id == this->type.clock)
    *interface = &this->clock;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static SpaResult
audiotestsrc_clear (SpaHandle *handle)
{
  SpaAudioTestSrc *this;

  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = (SpaAudioTestSrc *) handle;

  if (this->data_loop)
    spa_loop_remove_source (this->data_loop, &this->timer_source);
  close (this->timer_source.fd);

  return SPA_RESULT_OK;
}

static SpaResult
audiotestsrc_init (const SpaHandleFactory  *factory,
                   SpaHandle               *handle,
                   const SpaDict           *info,
                   const SpaSupport        *support,
                   uint32_t                 n_support)
{
  SpaAudioTestSrc *this;
  uint32_t i;

  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  handle->get_interface = spa_audiotestsrc_get_interface;
  handle->clear = audiotestsrc_clear;

  this = (SpaAudioTestSrc *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].type, SPA_TYPE__TypeMap) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE__Log) == 0)
      this->log = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE_LOOP__DataLoop) == 0)
      this->data_loop = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "a type-map is needed");
    return SPA_RESULT_ERROR;
  }
  init_type (&this->type, this->map);

  this->node = audiotestsrc_node;
  this->clock = audiotestsrc_clock;
  reset_audiotestsrc_props (this, &this->props);

  spa_list_init (&this->empty);

  this->timer_source.func = audiotestsrc_on_output;
  this->timer_source.data = this;
  this->timer_source.fd = timerfd_create (CLOCK_MONOTONIC, TFD_CLOEXEC);
  this->timer_source.mask = SPA_IO_IN;
  this->timer_source.rmask = 0;
  this->timerspec.it_value.tv_sec = 0;
  this->timerspec.it_value.tv_nsec = 0;
  this->timerspec.it_interval.tv_sec = 0;
  this->timerspec.it_interval.tv_nsec = 0;

  if (this->data_loop)
    spa_loop_add_source (this->data_loop, &this->timer_source);

  this->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                     SPA_PORT_INFO_FLAG_NO_REF;
  if (this->props.live)
    this->info.flags |= SPA_PORT_INFO_FLAG_LIVE;

  spa_log_info (this->log, "audiotestsrc %p: initialized", this);

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo audiotestsrc_interfaces[] =
{
  { SPA_TYPE__Node, },
  { SPA_TYPE__Clock, },
};

static SpaResult
audiotestsrc_enum_interface_info (const SpaHandleFactory  *factory,
                                  const SpaInterfaceInfo **info,
                                  uint32_t                 index)
{
  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  switch (index) {
    case 0:
      *info = &audiotestsrc_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_audiotestsrc_factory =
{ "audiotestsrc",
  NULL,
  sizeof (SpaAudioTestSrc),
  audiotestsrc_init,
  audiotestsrc_enum_interface_info,
};
