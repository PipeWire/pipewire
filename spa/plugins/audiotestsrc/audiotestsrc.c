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

#include <spa/id-map.h>
#include <spa/log.h>
#include <spa/loop.h>
#include <spa/node.h>
#include <spa/list.h>
#include <spa/audio/format.h>
#include <spa/format-builder.h>
#include <lib/props.h>

#define SAMPLES_TO_TIME(this,s)   ((s) * SPA_NSEC_PER_SEC / (this)->current_format.info.raw.rate)
#define BYTES_TO_SAMPLES(this,b)  ((b)/(this)->bpf)
#define BYTES_TO_TIME(this,b)     SAMPLES_TO_TIME(this, BYTES_TO_SAMPLES (this, b))

typedef struct {
  uint32_t node;
  uint32_t clock;
} URI;

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
  void *ptr;
  size_t size;
  SpaList link;
};

struct _SpaAudioTestSrc {
  SpaHandle handle;
  SpaNode node;
  SpaClock clock;

  URI uri;
  SpaIDMap *map;
  SpaLog *log;
  SpaLoop *data_loop;

  uint8_t props_buffer[512];
  SpaAudioTestSrcProps props;

  SpaNodeEventCallback event_cb;
  void *user_data;

  SpaSource timer_source;
  struct itimerspec timerspec;

  SpaPortInfo info;
  SpaAllocParam *params[2];
  SpaAllocParamBuffers param_buffers;
  SpaAllocParamMetaEnable param_meta;
  SpaPortOutput *output;

  bool have_format;
  SpaAudioInfo current_format;
  uint8_t format_buffer[1024];
  size_t bpf;

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
#define DEFAULT_WAVE 0
#define DEFAULT_FREQ 440.0
#define DEFAULT_VOLUME 1.0

static const uint32_t wave_val_sine = 0;
static const uint32_t wave_val_square = 1;

enum {
  PROP_ID_NONE,
  PROP_ID_LIVE,
  PROP_ID_WAVE,
  PROP_ID_FREQ,
  PROP_ID_VOLUME,
};

static void
reset_audiotestsrc_props (SpaAudioTestSrcProps *props)
{
  props->live = DEFAULT_LIVE;
  props->wave = DEFAULT_WAVE;
  props->freq = DEFAULT_FREQ;
  props->volume = DEFAULT_VOLUME;
}

static SpaResult
spa_audiotestsrc_node_get_props (SpaNode       *node,
                                 SpaProps     **props)
{
  SpaAudioTestSrc *this;
  SpaPODBuilder b = { NULL,  };
  SpaPODFrame f;

  if (node == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  spa_pod_builder_init (&b, this->props_buffer, sizeof (this->props_buffer));

  *props = SPA_MEMBER (b.data, spa_pod_builder_props (&b,
        SPA_POD_TYPE_PROP, &f,
           PROP_ID_LIVE,      SPA_POD_PROP_FLAG_READWRITE |
                              SPA_POD_PROP_RANGE_NONE,
                              SPA_POD_TYPE_BOOL, 1,
                                  this->props.live,
       -SPA_POD_TYPE_PROP, &f,
        SPA_POD_TYPE_PROP, &f,
           PROP_ID_WAVE,      SPA_POD_PROP_FLAG_READWRITE |
                              SPA_POD_PROP_RANGE_ENUM,
                              SPA_POD_TYPE_INT, 3,
                                  this->props.wave,
                                  wave_val_sine,
                                  wave_val_square,
       -SPA_POD_TYPE_PROP, &f,
        SPA_POD_TYPE_PROP, &f,
           PROP_ID_FREQ,      SPA_POD_PROP_FLAG_READWRITE |
                              SPA_POD_PROP_RANGE_MIN_MAX,
                              SPA_POD_TYPE_DOUBLE, 3,
                                  this->props.freq,
                                  0.0,
                                  50000000.0,
       -SPA_POD_TYPE_PROP, &f,
        SPA_POD_TYPE_PROP, &f,
           PROP_ID_VOLUME,    SPA_POD_PROP_FLAG_READWRITE |
                              SPA_POD_PROP_RANGE_MIN_MAX,
                              SPA_POD_TYPE_DOUBLE, 3,
                                  this->props.volume,
                                  0.0,
                                  10.0,
       -SPA_POD_TYPE_PROP, &f, 0), SpaProps);

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_set_props (SpaNode         *node,
                                 const SpaProps  *props)
{
  SpaAudioTestSrc *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  if (props == NULL) {
    reset_audiotestsrc_props (&this->props);
  } else {
    SpaPODProp *pr;

    SPA_POD_OBJECT_BODY_FOREACH (&props->body, props->pod.size, pr) {
      switch (pr->body.key) {
        case PROP_ID_LIVE:
          this->props.live = ((SpaPODBool*)&pr->body.value)->value;
          break;
        case PROP_ID_WAVE:
          this->props.wave = ((SpaPODInt*)&pr->body.value)->value;
          break;
        case PROP_ID_FREQ:
          this->props.freq = ((SpaPODDouble*)&pr->body.value)->value;
          break;
        case PROP_ID_VOLUME:
          this->props.volume = ((SpaPODDouble*)&pr->body.value)->value;
          break;
      }
    }
  }

  if (this->props.live)
    this->info.flags |= SPA_PORT_INFO_FLAG_LIVE;
  else
    this->info.flags &= ~SPA_PORT_INFO_FLAG_LIVE;

  return SPA_RESULT_OK;
}

static SpaResult
send_have_output (SpaAudioTestSrc *this)
{
  SpaNodeEvent event;

  if (this->event_cb) {
    event.type = SPA_NODE_EVENT_TYPE_HAVE_OUTPUT;
    event.size = sizeof (event);
    this->event_cb (&this->node, &event, this->user_data);
  }

  return SPA_RESULT_OK;
}

static SpaResult
fill_buffer (SpaAudioTestSrc *this, ATSBuffer *b)
{
  uint8_t *p = b->ptr;
  size_t i;

  for (i = 0; i < b->size; i++) {
    p[i] = rand();
  }

  return SPA_RESULT_OK;
}

static void
set_timer (SpaAudioTestSrc *this, bool enabled)
{
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

static void
audiotestsrc_on_output (SpaSource *source)
{
  SpaAudioTestSrc *this = source->data;
  ATSBuffer *b;
  SpaPortOutput *output;
  uint64_t expirations;

  if (read (this->timer_source.fd, &expirations, sizeof (uint64_t)) < sizeof (uint64_t))
    perror ("read timerfd");

  if (spa_list_is_empty (&this->empty)) {
    set_timer (this, false);
    return;
  }
  b = spa_list_first (&this->empty, ATSBuffer, link);
  spa_list_remove (&b->link);

  fill_buffer (this, b);

  if (b->h) {
    b->h->seq = this->sample_count;
    b->h->pts = this->start_time + this->elapsed_time;
    b->h->dts_offset = 0;
  }

  this->sample_count += b->size / this->bpf;
  this->elapsed_time = SAMPLES_TO_TIME (this, this->sample_count);
  set_timer (this, true);

  if ((output = this->output)) {
    b->outstanding = true;
    output->buffer_id = b->outbuf->id;
    output->status = SPA_RESULT_OK;
    send_have_output (this);
  }
}

static void
update_state (SpaAudioTestSrc *this, SpaNodeState state)
{
  this->node.state = state;
}

static SpaResult
spa_audiotestsrc_node_send_command (SpaNode        *node,
                                    SpaNodeCommand *command)
{
  SpaAudioTestSrc *this;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  switch (command->type) {
    case SPA_NODE_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_NODE_COMMAND_START:
    {
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
      update_state (this, SPA_NODE_STATE_STREAMING);
      break;
    }
    case SPA_NODE_COMMAND_PAUSE:
    {
      if (!this->have_format)
        return SPA_RESULT_NO_FORMAT;

      if (this->n_buffers == 0)
        return SPA_RESULT_NO_BUFFERS;

      if (!this->started)
        return SPA_RESULT_OK;

      this->started = false;
      set_timer (this, false);
      update_state (this, SPA_NODE_STATE_PAUSED);
      break;
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
spa_audiotestsrc_node_set_event_callback (SpaNode              *node,
                                          SpaNodeEventCallback  event_cb,
                                          void                 *user_data)
{
  SpaAudioTestSrc *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  if (event_cb == NULL && this->event_cb)
    spa_loop_remove_source (this->data_loop, &this->timer_source);

  this->event_cb = event_cb;
  this->user_data = user_data;

  if (this->event_cb) {
    spa_loop_add_source (this->data_loop, &this->timer_source);
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_get_n_ports (SpaNode       *node,
                                   uint32_t      *n_input_ports,
                                   uint32_t      *max_input_ports,
                                   uint32_t      *n_output_ports,
                                   uint32_t      *max_output_ports)
{
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

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
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

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

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

next:
  spa_pod_builder_init (&b, buffer, sizeof (buffer));

  switch (index++) {
    case 0:
      fmt = SPA_MEMBER (buffer, spa_pod_builder_format (&b,
         SPA_MEDIA_TYPE_AUDIO, SPA_MEDIA_SUBTYPE_RAW,
         SPA_POD_TYPE_PROP, &f[0],
           SPA_PROP_ID_AUDIO_FORMAT,    SPA_POD_PROP_FLAG_UNSET | SPA_POD_PROP_FLAG_READWRITE |
                                        SPA_POD_PROP_RANGE_ENUM,
                                        SPA_POD_TYPE_INT, 3,
                                                SPA_AUDIO_FORMAT_S16,
                                                SPA_AUDIO_FORMAT_S16,
                                                SPA_AUDIO_FORMAT_S32,
        -SPA_POD_TYPE_PROP, &f[0],
         SPA_POD_TYPE_PROP, &f[0],
           SPA_PROP_ID_AUDIO_RATE,      SPA_POD_PROP_FLAG_UNSET | SPA_POD_PROP_FLAG_READWRITE |
                                        SPA_POD_PROP_RANGE_MIN_MAX,
                                        SPA_POD_TYPE_INT, 3,
                                                44100,
                                                1, INT32_MAX,
        -SPA_POD_TYPE_PROP, &f[0],
         SPA_POD_TYPE_PROP, &f[0],
           SPA_PROP_ID_AUDIO_CHANNELS,  SPA_POD_PROP_FLAG_UNSET | SPA_POD_PROP_FLAG_READWRITE |
                                        SPA_POD_PROP_RANGE_MIN_MAX,
                                        SPA_POD_TYPE_INT, 3,
                                                2,
                                                1, INT32_MAX,
        -SPA_POD_TYPE_PROP, &f[0], 0), SpaFormat);
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }

  spa_pod_builder_init (&b, this->format_buffer, sizeof (this->format_buffer));

  if ((res = spa_format_filter (fmt, filter, &b)) != SPA_RESULT_OK)
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
spa_audiotestsrc_node_port_set_format (SpaNode            *node,
                                       SpaDirection        direction,
                                       uint32_t            port_id,
                                       SpaPortFormatFlags  flags,
                                       const SpaFormat    *format)
{
  SpaAudioTestSrc *this;
  SpaResult res;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (format == NULL) {
    this->have_format = false;
    clear_buffers (this);
  } else {
    if ((res = spa_format_audio_parse (format, &this->current_format)) < 0)
      return res;

    this->bpf = (2 * this->current_format.info.raw.channels);
    this->have_format = true;
  }

  if (this->have_format) {
    this->info.maxbuffering = -1;
    this->info.latency = BYTES_TO_TIME (this, 1024);

    this->info.n_params = 2;
    this->info.params = this->params;
    this->params[0] = &this->param_buffers.param;
    this->param_buffers.param.type = SPA_ALLOC_PARAM_TYPE_BUFFERS;
    this->param_buffers.param.size = sizeof (this->param_buffers);
    this->param_buffers.minsize = 1024;
    this->param_buffers.stride = 1024;
    this->param_buffers.min_buffers = 2;
    this->param_buffers.max_buffers = 32;
    this->param_buffers.align = 16;
    this->params[1] = &this->param_meta.param;
    this->param_meta.param.type = SPA_ALLOC_PARAM_TYPE_META_ENABLE;
    this->param_meta.param.size = sizeof (this->param_meta);
    this->param_meta.type = SPA_META_TYPE_HEADER;
    this->info.extra = NULL;
    update_state (this, SPA_NODE_STATE_READY);
  }
  else
    update_state (this, SPA_NODE_STATE_CONFIGURE);

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
  SpaPODFrame f;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  spa_pod_builder_init (&b, this->format_buffer, sizeof (this->format_buffer));

  *format = SPA_MEMBER (b.data, spa_pod_builder_format (&b,
         SPA_MEDIA_TYPE_AUDIO, SPA_MEDIA_SUBTYPE_RAW,
         SPA_POD_TYPE_PROP, &f,
           SPA_PROP_ID_AUDIO_FORMAT,    SPA_POD_PROP_FLAG_READWRITE,
                                        SPA_POD_TYPE_INT, 1,
                                                this->current_format.info.raw.format,
        -SPA_POD_TYPE_PROP, &f,
         SPA_POD_TYPE_PROP, &f,
           SPA_PROP_ID_AUDIO_RATE,      SPA_POD_PROP_FLAG_READWRITE,
                                        SPA_POD_TYPE_INT, 1,
                                                this->current_format.info.raw.rate,
        -SPA_POD_TYPE_PROP, &f,
         SPA_POD_TYPE_PROP, &f,
           SPA_PROP_ID_AUDIO_CHANNELS,  SPA_POD_PROP_FLAG_READWRITE,
                                        SPA_POD_TYPE_INT, 1,
                                                this->current_format.info.raw.channels,
        -SPA_POD_TYPE_PROP, &f, 0), SpaFormat);

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_get_info (SpaNode            *node,
                                     SpaDirection        direction,
                                     uint32_t            port_id,
                                     const SpaPortInfo **info)
{
  SpaAudioTestSrc *this;

  if (node == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  *info = &this->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_get_props (SpaNode       *node,
                                      SpaDirection   direction,
                                      uint32_t       port_id,
                                      SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_port_set_props (SpaNode        *node,
                                      SpaDirection    direction,
                                      uint32_t        port_id,
                                      const SpaProps *props)
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

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  clear_buffers (this);

  for (i = 0; i < n_buffers; i++) {
    ATSBuffer *b;
    SpaData *d = buffers[i]->datas;

    b = &this->buffers[i];
    b->outbuf = buffers[i];
    b->outstanding = true;
    b->h = spa_buffer_find_meta (buffers[i], SPA_META_TYPE_HEADER);

    switch (d[0].type) {
      case SPA_DATA_TYPE_MEMPTR:
      case SPA_DATA_TYPE_MEMFD:
      case SPA_DATA_TYPE_DMABUF:
        if (d[0].data == NULL) {
          spa_log_error (this->log, "audiotestsrc %p: invalid memory on buffer %p", this, buffers[i]);
          continue;
        }
        b->ptr = d[0].data;
        b->size = d[0].maxsize;
        break;
      default:
        break;
    }
    spa_list_insert (this->empty.prev, &b->link);
  }
  this->n_buffers = n_buffers;

  if (this->n_buffers > 0) {
    update_state (this, SPA_NODE_STATE_PAUSED);
  } else {
    update_state (this, SPA_NODE_STATE_READY);
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_alloc_buffers (SpaNode         *node,
                                          SpaDirection     direction,
                                          uint32_t         port_id,
                                          SpaAllocParam  **params,
                                          uint32_t         n_params,
                                          SpaBuffer      **buffers,
                                          uint32_t        *n_buffers)
{
  SpaAudioTestSrc *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_port_set_input (SpaNode      *node,
                                      uint32_t      port_id,
                                      SpaPortInput *input)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_port_set_output (SpaNode       *node,
                                       uint32_t       port_id,
                                       SpaPortOutput *output)
{
  SpaAudioTestSrc *this;

  if (node == NULL || output == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  if (!CHECK_PORT (this, SPA_DIRECTION_OUTPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->output = output;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_reuse_buffer (SpaNode         *node,
                                         uint32_t         port_id,
                                         uint32_t         buffer_id)
{
  SpaAudioTestSrc *this;
  ATSBuffer *b;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaAudioTestSrc, node);

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  if (this->n_buffers == 0)
    return SPA_RESULT_NO_BUFFERS;

  if (buffer_id >= this->n_buffers)
    return SPA_RESULT_INVALID_BUFFER_ID;

  b = &this->buffers[buffer_id];
  if (!b->outstanding)
    return SPA_RESULT_OK;

  b->outstanding = false;
  spa_list_insert (this->empty.prev, &b->link);

  if (!this->props.live)
    set_timer (this, true);

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_send_command (SpaNode        *node,
                                         SpaDirection    direction,
                                         uint32_t        port_id,
                                         SpaNodeCommand *command)
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
  return SPA_RESULT_OK;
}

static const SpaNode audiotestsrc_node = {
  sizeof (SpaNode),
  NULL,
  SPA_NODE_STATE_INIT,
  spa_audiotestsrc_node_get_props,
  spa_audiotestsrc_node_set_props,
  spa_audiotestsrc_node_send_command,
  spa_audiotestsrc_node_set_event_callback,
  spa_audiotestsrc_node_get_n_ports,
  spa_audiotestsrc_node_get_port_ids,
  spa_audiotestsrc_node_add_port,
  spa_audiotestsrc_node_remove_port,
  spa_audiotestsrc_node_port_enum_formats,
  spa_audiotestsrc_node_port_set_format,
  spa_audiotestsrc_node_port_get_format,
  spa_audiotestsrc_node_port_get_info,
  spa_audiotestsrc_node_port_get_props,
  spa_audiotestsrc_node_port_set_props,
  spa_audiotestsrc_node_port_use_buffers,
  spa_audiotestsrc_node_port_alloc_buffers,
  spa_audiotestsrc_node_port_set_input,
  spa_audiotestsrc_node_port_set_output,
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

  if (clock == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

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

  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) handle;

  if (interface_id == this->uri.node)
    *interface = &this->node;
  else if (interface_id == this->uri.clock)
    *interface = &this->clock;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static SpaResult
audiotestsrc_clear (SpaHandle *handle)
{
  SpaAudioTestSrc *this;

  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) handle;

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

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_audiotestsrc_get_interface;
  handle->clear = audiotestsrc_clear;

  this = (SpaAudioTestSrc *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].uri, SPA_ID_MAP_URI) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].uri, SPA_LOG_URI) == 0)
      this->log = support[i].data;
    else if (strcmp (support[i].uri, SPA_LOOP__DataLoop) == 0)
      this->data_loop = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "an id-map is needed");
    return SPA_RESULT_ERROR;
  }
  if (this->data_loop == NULL) {
    spa_log_error (this->log, "a data_loop is needed");
    return SPA_RESULT_ERROR;
  }
  this->uri.node = spa_id_map_get_id (this->map, SPA_NODE_URI);
  this->uri.clock = spa_id_map_get_id (this->map, SPA_CLOCK_URI);

  this->node = audiotestsrc_node;
  this->clock = audiotestsrc_clock;
  reset_audiotestsrc_props (&this->props);

  spa_list_init (&this->empty);

  this->timer_source.func = audiotestsrc_on_output;
  this->timer_source.data = this;
  this->timer_source.fd = timerfd_create (CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  this->timer_source.mask = SPA_IO_IN;
  this->timer_source.rmask = 0;
  this->timerspec.it_value.tv_sec = 0;
  this->timerspec.it_value.tv_nsec = 0;
  this->timerspec.it_interval.tv_sec = 0;
  this->timerspec.it_interval.tv_nsec = 0;

  this->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                     SPA_PORT_INFO_FLAG_NO_REF;
  if (this->props.live)
    this->info.flags |= SPA_PORT_INFO_FLAG_LIVE;

  this->node.state = SPA_NODE_STATE_CONFIGURE;

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo audiotestsrc_interfaces[] =
{
  { SPA_NODE_URI, },
  { SPA_CLOCK_URI, },
};

static SpaResult
audiotestsrc_enum_interface_info (const SpaHandleFactory  *factory,
                                  const SpaInterfaceInfo **info,
                                  uint32_t                 index)
{
  if (factory == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

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
