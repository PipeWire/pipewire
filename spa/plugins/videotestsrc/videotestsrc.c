/* Spa
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 * Copyright (C) 2016 Axis Communications AB
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
#include <spa/video/format.h>
#include <lib/props.h>

#define FRAMES_TO_TIME(this,f)    ((this->current_format.info.raw.framerate.denom * (f) * SPA_NSEC_PER_SEC) / \
                                   (this->current_format.info.raw.framerate.num))

typedef struct {
  uint32_t node;
  uint32_t clock;
} URI;

typedef struct _SpaVideoTestSrc SpaVideoTestSrc;

typedef struct {
  SpaProps props;
  bool live;
  uint32_t pattern;
} SpaVideoTestSrcProps;

#define MAX_BUFFERS 16

typedef struct _VTSBuffer VTSBuffer;

struct _VTSBuffer {
  SpaBuffer *outbuf;
  bool outstanding;
  SpaMetaHeader *h;
  void *ptr;
  size_t stride;
  SpaList link;
};

struct _SpaVideoTestSrc {
  SpaHandle handle;
  SpaNode node;
  SpaClock clock;

  URI uri;
  SpaIDMap *map;
  SpaLog *log;
  SpaLoop *data_loop;

  SpaVideoTestSrcProps props[2];

  SpaNodeEventCallback event_cb;
  void *user_data;

  SpaSource timer_source;
  bool timer_enabled;
  struct itimerspec timerspec;

  SpaPortInfo info;
  SpaAllocParam *params[2];
  SpaAllocParamBuffers param_buffers;
  SpaAllocParamMetaEnable param_meta;
  SpaPortOutput *output;

  bool have_format;
  SpaFormatVideo query_format;
  SpaFormatVideo current_format;
  size_t bpp;

  VTSBuffer buffers[MAX_BUFFERS];
  unsigned int n_buffers;

  bool started;
  uint64_t start_time;
  uint64_t elapsed_time;

  uint64_t frame_count;
  SpaList empty;
};

#define CHECK_PORT(this,d,p)  ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)

#define DEFAULT_LIVE true
#define DEFAULT_PATTERN 0

static const uint32_t pattern_val_smpte_snow = 0;
static const uint32_t pattern_val_snow = 1;

static const SpaPropRangeInfo pattern_range[] = {
  { "smpte-snow", { sizeof (uint32_t), &pattern_val_smpte_snow } },
  { "snow", { sizeof (uint32_t), &pattern_val_snow } },
};

enum {
  PROP_ID_LIVE,
  PROP_ID_PATTERN,
  PROP_ID_LAST,
};

static const SpaPropInfo prop_info[] =
{
  { PROP_ID_LIVE,              offsetof (SpaVideoTestSrcProps, live),
                               "live",
                               SPA_PROP_FLAG_READWRITE,
                               SPA_PROP_TYPE_BOOL, sizeof (bool),
                               SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                               NULL },
  { PROP_ID_PATTERN,           offsetof (SpaVideoTestSrcProps, pattern),
                               "pattern",
                               SPA_PROP_FLAG_READWRITE,
                               SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                               SPA_PROP_RANGE_TYPE_ENUM, SPA_N_ELEMENTS (pattern_range), pattern_range,
                               NULL },
};

static SpaResult
reset_videotestsrc_props (SpaVideoTestSrcProps *props)
{
  props->live = DEFAULT_LIVE;
  props->pattern = DEFAULT_PATTERN;

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_get_props (SpaNode       *node,
                                 SpaProps     **props)
{
  SpaVideoTestSrc *this;

  if (node == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));
  *props = &this->props[0].props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_set_props (SpaNode         *node,
                                 const SpaProps  *props)
{
  SpaVideoTestSrc *this;
  SpaVideoTestSrcProps *p;
  SpaResult res;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);
  p = &this->props[1];

  if (props == NULL) {
    res = reset_videotestsrc_props (p);
  } else {
    res = spa_props_copy_values (props, &p->props);
  }

  if (this->props[1].live)
    this->info.flags |= SPA_PORT_INFO_FLAG_LIVE;
  else
    this->info.flags &= ~SPA_PORT_INFO_FLAG_LIVE;

  return res;
}

static SpaResult
send_have_output (SpaVideoTestSrc *this)
{
  SpaNodeEventHaveOutput ho;

  if (this->event_cb) {
    ho.event.type = SPA_NODE_EVENT_TYPE_HAVE_OUTPUT;
    ho.event.size = sizeof (ho);
    ho.port_id = 0;
    this->event_cb (&this->node, &ho.event, this->user_data);
  }

  return SPA_RESULT_OK;
}

#include "draw.c"

static SpaResult
fill_buffer (SpaVideoTestSrc *this, VTSBuffer *b)
{
  return draw (this, b->ptr);
}

static SpaResult update_loop_enabled (SpaVideoTestSrc *this, bool enabled);

static void
videotestsrc_on_output (SpaSource *source)
{
  SpaVideoTestSrc *this = source->data;
  VTSBuffer *b;
  SpaPortOutput *output;

  if (spa_list_is_empty (&this->empty)) {
    update_loop_enabled (this, false);
    return;
  }
  b = spa_list_first (&this->empty, VTSBuffer, link);
  spa_list_remove (&b->link);

  fill_buffer (this, b);

  if (b->h) {
    b->h->seq = this->frame_count;
    b->h->pts = this->start_time + this->elapsed_time;
    b->h->dts_offset = 0;
  }

  this->frame_count++;
  this->elapsed_time = FRAMES_TO_TIME (this, this->frame_count);

  if (this->props[1].live) {
    uint64_t expirations, next_time;

    if (read (this->timer_source.fd, &expirations, sizeof (uint64_t)) < sizeof (uint64_t))
      perror ("read timerfd");

    next_time = this->start_time + this->elapsed_time;
    this->timerspec.it_value.tv_sec = next_time / SPA_NSEC_PER_SEC;
    this->timerspec.it_value.tv_nsec = next_time % SPA_NSEC_PER_SEC;
    timerfd_settime (this->timer_source.fd, TFD_TIMER_ABSTIME, &this->timerspec, NULL);
  }

  if ((output = this->output)) {
    b->outstanding = true;
    output->buffer_id = b->outbuf->id;
    output->status = SPA_RESULT_OK;
    send_have_output (this);
  }
}

static void
update_state (SpaVideoTestSrc *this, SpaNodeState state)
{
  this->node.state = state;
}

static SpaResult
update_loop_enabled (SpaVideoTestSrc *this, bool enabled)
{
  if (this->event_cb && this->timer_enabled != enabled) {
    this->timer_enabled = enabled;
    if (enabled)
      this->timer_source.mask = SPA_IO_IN;
    else
      this->timer_source.mask = 0;

    if (this->props[1].live) {
      if (enabled) {
        uint64_t next_time = this->start_time + this->elapsed_time;
        this->timerspec.it_value.tv_sec = next_time / SPA_NSEC_PER_SEC;
        this->timerspec.it_value.tv_nsec = next_time % SPA_NSEC_PER_SEC;
      }
      else {
        this->timerspec.it_value.tv_sec = 0;
        this->timerspec.it_value.tv_nsec = 0;
      }
      timerfd_settime (this->timer_source.fd, TFD_TIMER_ABSTIME, &this->timerspec, NULL);
    }
    spa_loop_update_source (this->data_loop, &this->timer_source);
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_send_command (SpaNode        *node,
                                    SpaNodeCommand *command)
{
  SpaVideoTestSrc *this;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

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
      if (this->props[1].live)
        this->start_time = SPA_TIMESPEC_TO_TIME (&now);
      else
        this->start_time = 0;
      this->frame_count = 0;
      this->elapsed_time = 0;

      this->started = true;
      update_loop_enabled (this, true);
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
      update_loop_enabled (this, false);
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
spa_videotestsrc_node_set_event_callback (SpaNode              *node,
                                          SpaNodeEventCallback  event_cb,
                                          void                 *user_data)
{
  SpaVideoTestSrc *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

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
spa_videotestsrc_node_get_n_ports (SpaNode       *node,
                                   unsigned int  *n_input_ports,
                                   unsigned int  *max_input_ports,
                                   unsigned int  *n_output_ports,
                                   unsigned int  *max_output_ports)
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
spa_videotestsrc_node_get_port_ids (SpaNode       *node,
                                    unsigned int   n_input_ports,
                                    uint32_t      *input_ids,
                                    unsigned int   n_output_ports,
                                    uint32_t      *output_ids)
{
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_output_ports > 0 && output_ids != NULL)
    output_ids[0] = 0;

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_add_port (SpaNode        *node,
                                SpaDirection    direction,
                                uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_node_remove_port (SpaNode        *node,
                                   SpaDirection    direction,
                                   uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_node_port_enum_formats (SpaNode          *node,
                                         SpaDirection      direction,
                                         uint32_t          port_id,
                                         SpaFormat       **format,
                                         const SpaFormat  *filter,
                                         void            **state)
{
  SpaVideoTestSrc *this;
  int index;

  if (node == NULL || format == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      if (filter)
        spa_format_video_parse (filter, &this->query_format);
      else
        spa_format_video_init (SPA_MEDIA_TYPE_VIDEO,
                               SPA_MEDIA_SUBTYPE_RAW,
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
clear_buffers (SpaVideoTestSrc *this)
{
  if (this->n_buffers > 0) {
    spa_log_info (this->log, "videotestsrc %p: clear buffers", this);
    this->n_buffers = 0;
    spa_list_init (&this->empty);
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_port_set_format (SpaNode            *node,
                                       SpaDirection        direction,
                                       uint32_t            port_id,
                                       SpaPortFormatFlags  flags,
                                       const SpaFormat    *format)
{
  SpaVideoTestSrc *this;
  SpaResult res;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (format == NULL) {
    this->have_format = false;
    clear_buffers (this);
  } else {
    if ((res = spa_format_video_parse (format, &this->current_format)) < 0)
      return res;

    this->have_format = true;
  }

  if (this->have_format) {
    SpaVideoInfoRaw *raw_info = &this->current_format.info.raw;

    switch (raw_info->format) {
      case SPA_VIDEO_FORMAT_RGB:
        this->bpp = 3;
        break;
      case SPA_VIDEO_FORMAT_UYVY:
        this->bpp = 2;
        break;
      default:
        return SPA_RESULT_NOT_IMPLEMENTED;
    }

    this->info.maxbuffering = -1;
    this->info.latency = 0;

    this->info.n_params = 2;
    this->info.params = this->params;
    this->params[0] = &this->param_buffers.param;
    this->param_buffers.param.type = SPA_ALLOC_PARAM_TYPE_BUFFERS;
    this->param_buffers.param.size = sizeof (this->param_buffers);
    this->param_buffers.stride = this->bpp * raw_info->size.width;
    this->param_buffers.minsize = this->param_buffers.stride * raw_info->size.height;
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
spa_videotestsrc_node_port_get_format (SpaNode          *node,
                                       SpaDirection      direction,
                                       uint32_t          port_id,
                                       const SpaFormat **format)
{
  SpaVideoTestSrc *this;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = &this->current_format.format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_port_get_info (SpaNode            *node,
                                     SpaDirection        direction,
                                     uint32_t            port_id,
                                     const SpaPortInfo **info)
{
  SpaVideoTestSrc *this;

  if (node == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  *info = &this->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_port_get_props (SpaNode       *node,
                                      SpaDirection   direction,
                                      uint32_t       port_id,
                                      SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_node_port_set_props (SpaNode        *node,
                                      SpaDirection    direction,
                                      uint32_t        port_id,
                                      const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_node_port_use_buffers (SpaNode         *node,
                                        SpaDirection     direction,
                                        uint32_t         port_id,
                                        SpaBuffer      **buffers,
                                        uint32_t         n_buffers)
{
  SpaVideoTestSrc *this;
  unsigned int i;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  clear_buffers (this);

  for (i = 0; i < n_buffers; i++) {
    VTSBuffer *b;
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
          spa_log_error (this->log, "videotestsrc %p: invalid memory on buffer %p", this, buffers[i]);
          continue;
        }
        b->ptr = SPA_MEMBER (d[0].data, d[0].chunk->offset, void);
        b->stride = d[0].chunk->stride;
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
spa_videotestsrc_node_port_alloc_buffers (SpaNode         *node,
                                          SpaDirection     direction,
                                          uint32_t         port_id,
                                          SpaAllocParam  **params,
                                          uint32_t         n_params,
                                          SpaBuffer      **buffers,
                                          uint32_t        *n_buffers)
{
  SpaVideoTestSrc *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  if (this->n_buffers == 0)
    return SPA_RESULT_NO_BUFFERS;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_node_port_set_input (SpaNode      *node,
                                      uint32_t      port_id,
                                      SpaPortInput *input)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_node_port_set_output (SpaNode       *node,
                                       uint32_t       port_id,
                                       SpaPortOutput *output)
{
  SpaVideoTestSrc *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  if (!CHECK_PORT (this, SPA_DIRECTION_OUTPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->output = output;

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_port_reuse_buffer (SpaNode         *node,
                                         uint32_t         port_id,
                                         uint32_t         buffer_id)
{
  SpaVideoTestSrc *this;
  VTSBuffer *b;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

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

  if (!this->props[1].live)
    update_loop_enabled (this, true);

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_port_send_command (SpaNode        *node,
                                         SpaDirection    direction,
                                         uint32_t        port_id,
                                         SpaNodeCommand *command)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_node_process_input (SpaNode *node)
{
  return SPA_RESULT_INVALID_PORT;
}

static SpaResult
spa_videotestsrc_node_process_output (SpaNode *node)
{
  return SPA_RESULT_OK;
}

static const SpaNode videotestsrc_node = {
  sizeof (SpaNode),
  NULL,
  SPA_NODE_STATE_INIT,
  spa_videotestsrc_node_get_props,
  spa_videotestsrc_node_set_props,
  spa_videotestsrc_node_send_command,
  spa_videotestsrc_node_set_event_callback,
  spa_videotestsrc_node_get_n_ports,
  spa_videotestsrc_node_get_port_ids,
  spa_videotestsrc_node_add_port,
  spa_videotestsrc_node_remove_port,
  spa_videotestsrc_node_port_enum_formats,
  spa_videotestsrc_node_port_set_format,
  spa_videotestsrc_node_port_get_format,
  spa_videotestsrc_node_port_get_info,
  spa_videotestsrc_node_port_get_props,
  spa_videotestsrc_node_port_set_props,
  spa_videotestsrc_node_port_use_buffers,
  spa_videotestsrc_node_port_alloc_buffers,
  spa_videotestsrc_node_port_set_input,
  spa_videotestsrc_node_port_set_output,
  spa_videotestsrc_node_port_reuse_buffer,
  spa_videotestsrc_node_port_send_command,
  spa_videotestsrc_node_process_input,
  spa_videotestsrc_node_process_output,
};

static SpaResult
spa_videotestsrc_clock_get_props (SpaClock  *clock,
                                  SpaProps **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_clock_set_props (SpaClock       *clock,
                                  const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_clock_get_time (SpaClock         *clock,
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

static const SpaClock videotestsrc_clock = {
  sizeof (SpaClock),
  NULL,
  SPA_CLOCK_STATE_STOPPED,
  spa_videotestsrc_clock_get_props,
  spa_videotestsrc_clock_set_props,
  spa_videotestsrc_clock_get_time,
};

static SpaResult
spa_videotestsrc_get_interface (SpaHandle         *handle,
                                uint32_t           interface_id,
                                void             **interface)
{
  SpaVideoTestSrc *this;

  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVideoTestSrc *) handle;

  if (interface_id == this->uri.node)
    *interface = &this->node;
  else if (interface_id == this->uri.clock)
    *interface = &this->clock;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static SpaResult
videotestsrc_clear (SpaHandle *handle)
{
  SpaVideoTestSrc *this;

  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVideoTestSrc *) handle;

  close (this->timer_source.fd);

  return SPA_RESULT_OK;
}

static SpaResult
videotestsrc_init (const SpaHandleFactory  *factory,
                   SpaHandle               *handle,
                   const SpaDict           *info,
                   const SpaSupport        *support,
                   unsigned int             n_support)
{
  SpaVideoTestSrc *this;
  unsigned int i;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_videotestsrc_get_interface;
  handle->clear = videotestsrc_clear;

  this = (SpaVideoTestSrc *) handle;

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

  this->node = videotestsrc_node;
  this->clock = videotestsrc_clock;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  reset_videotestsrc_props (&this->props[1]);

  spa_list_init (&this->empty);

  this->timer_source.func = videotestsrc_on_output;
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
  if (this->props[1].live)
    this->info.flags |= SPA_PORT_INFO_FLAG_LIVE;

  this->node.state = SPA_NODE_STATE_CONFIGURE;

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo videotestsrc_interfaces[] =
{
  { SPA_NODE_URI, },
  { SPA_CLOCK_URI, },
};

static SpaResult
videotestsrc_enum_interface_info (const SpaHandleFactory  *factory,
                                  const SpaInterfaceInfo **info,
                                  void			 **state)
{
  int index;

  if (factory == NULL || info == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      *info = &videotestsrc_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *(int*)state = ++index;
  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_videotestsrc_factory =
{ "videotestsrc",
  NULL,
  sizeof (SpaVideoTestSrc),
  videotestsrc_init,
  videotestsrc_enum_interface_info,
};
