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

#include <spa/type-map.h>
#include <spa/clock.h>
#include <spa/log.h>
#include <spa/loop.h>
#include <spa/node.h>
#include <spa/list.h>
#include <spa/video/format-utils.h>
#include <spa/format-builder.h>
#include <lib/props.h>

#define FRAMES_TO_TIME(this,f)    ((this->current_format.info.raw.framerate.denom * (f) * SPA_NSEC_PER_SEC) / \
                                   (this->current_format.info.raw.framerate.num))

typedef struct {
  uint32_t node;
  uint32_t clock;
  uint32_t format;
  uint32_t props;
  uint32_t prop_live;
  uint32_t prop_pattern;
  uint32_t pattern_smpte_snow;
  uint32_t pattern_snow;
  SpaTypeMeta meta;
  SpaTypeData data;
  SpaTypeMediaType media_type;
  SpaTypeMediaSubtype media_subtype;
  SpaTypeFormatVideo format_video;
  SpaTypeVideoFormat video_format;
  SpaTypeEventNode event_node;
  SpaTypeCommandNode command_node;
  SpaTypeAllocParamBuffers alloc_param_buffers;
  SpaTypeAllocParamMetaEnable alloc_param_meta_enable;
} Type;

static inline void
init_type (Type *type, SpaTypeMap *map)
{
  type->node = spa_type_map_get_id (map, SPA_TYPE__Node);
  type->clock = spa_type_map_get_id (map, SPA_TYPE__Clock);
  type->format = spa_type_map_get_id (map, SPA_TYPE__Format);
  type->props = spa_type_map_get_id (map, SPA_TYPE__Props);
  type->prop_live = spa_type_map_get_id (map, SPA_TYPE_PROPS__live);
  type->prop_pattern = spa_type_map_get_id (map, SPA_TYPE_PROPS__patternType);
  type->pattern_smpte_snow = spa_type_map_get_id (map, SPA_TYPE_PROPS__patternType ":smpte-snow");
  type->pattern_snow = spa_type_map_get_id (map, SPA_TYPE_PROPS__patternType ":snow");
  spa_type_meta_map (map, &type->meta);
  spa_type_data_map (map, &type->data);
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_format_video_map (map, &type->format_video);
  spa_type_video_format_map (map, &type->video_format);
  spa_type_event_node_map (map, &type->event_node);
  spa_type_command_node_map (map, &type->command_node);
  spa_type_alloc_param_buffers_map (map, &type->alloc_param_buffers);
  spa_type_alloc_param_meta_enable_map (map, &type->alloc_param_meta_enable);
}

typedef struct _SpaVideoTestSrc SpaVideoTestSrc;

typedef struct {
  bool live;
  uint32_t pattern;
} SpaVideoTestSrcProps;

#define MAX_BUFFERS 16
#define MAX_PORTS 1

typedef struct _VTSBuffer VTSBuffer;

struct _VTSBuffer {
  SpaBuffer *outbuf;
  bool outstanding;
  SpaMetaHeader *h;
  SpaList link;
};

struct _SpaVideoTestSrc {
  SpaHandle handle;
  SpaNode node;
  SpaClock clock;

  Type type;
  SpaTypeMap *map;
  SpaLog *log;
  SpaLoop *data_loop;
  bool async;

  uint8_t props_buffer[512];
  SpaVideoTestSrcProps props;

  SpaEventNodeCallback event_cb;
  void *user_data;

  SpaSource timer_source;
  struct itimerspec timerspec;

  SpaPortInfo info;
  SpaAllocParam *params[2];
  uint8_t params_buffer[1024];
  SpaPortIO *io;

  bool have_format;
  SpaVideoInfo current_format;
  uint8_t format_buffer[1024];
  size_t bpp;
  int stride;

  VTSBuffer buffers[MAX_BUFFERS];
  uint32_t  n_buffers;

  bool started;
  uint64_t start_time;
  uint64_t elapsed_time;

  uint64_t frame_count;
  SpaList empty;
};

#define CHECK_PORT_NUM(this,d,p)  ((d) == SPA_DIRECTION_OUTPUT && (p) < MAX_PORTS)
#define CHECK_PORT(this,d,p)      (CHECK_PORT_NUM(this,d,p) && this->io)

#define DEFAULT_LIVE true
#define DEFAULT_PATTERN pattern_smpte_snow

static void
reset_videotestsrc_props (SpaVideoTestSrc *this, SpaVideoTestSrcProps *props)
{
  props->live = DEFAULT_LIVE;
  props->pattern = this->type. DEFAULT_PATTERN;
}

#define PROP(f,key,type,...)                                                    \
          SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
#define PROP_MM(f,key,type,...)                                                 \
          SPA_POD_PROP (f,key,SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_U_MM(f,key,type,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_EN(f,key,type,n,...)                                               \
          SPA_POD_PROP (f,key, SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)
#define PROP_U_EN(f,key,type,n,...)                                             \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)

static SpaResult
spa_videotestsrc_node_get_props (SpaNode       *node,
                                 SpaProps     **props)
{
  SpaVideoTestSrc *this;
  SpaPODBuilder b = { NULL,  };
  SpaPODFrame f[2];

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (props != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  spa_pod_builder_init (&b, this->props_buffer, sizeof (this->props_buffer));
  spa_pod_builder_props (&b, &f[0], this->type.props,
    PROP    (&f[1], this->type.prop_live,      SPA_POD_TYPE_BOOL, this->props.live),
    PROP_EN (&f[1], this->type.prop_pattern,   SPA_POD_TYPE_ID,  3,
                                                        this->props.pattern,
                                                        this->type.pattern_smpte_snow,
                                                        this->type.pattern_snow));

  *props = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaProps);

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_set_props (SpaNode         *node,
                                 const SpaProps  *props)
{
  SpaVideoTestSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  if (props == NULL) {
    reset_videotestsrc_props (this, &this->props);
  } else {
    spa_props_query (props,
        this->type.prop_live,     SPA_POD_TYPE_BOOL,   &this->props.live,
        this->type.prop_pattern,  SPA_POD_TYPE_ID,     &this->props.pattern,
        0);
  }

  if (this->props.live)
    this->info.flags |= SPA_PORT_INFO_FLAG_LIVE;
  else
    this->info.flags &= ~SPA_PORT_INFO_FLAG_LIVE;

  return SPA_RESULT_OK;
}

static SpaResult
send_have_output (SpaVideoTestSrc *this)
{
  if (this->event_cb) {
    SpaEvent event = SPA_EVENT_INIT (this->type.event_node.HaveOutput);
    this->event_cb (&this->node, &event, this->user_data);
  }
  return SPA_RESULT_OK;
}

#include "draw.c"

static SpaResult
fill_buffer (SpaVideoTestSrc *this, VTSBuffer *b)
{
  return draw (this, b->outbuf->datas[0].data);
}

static void
set_timer (SpaVideoTestSrc *this, bool enabled)
{
  if (this->async || this->props.live) {
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
read_timer (SpaVideoTestSrc *this)
{
  uint64_t expirations;

  if (this->async || this->props.live) {
    if (read (this->timer_source.fd, &expirations, sizeof (uint64_t)) < sizeof (uint64_t))
      perror ("read timerfd");
  }
}

static SpaResult
videotestsrc_make_buffer (SpaVideoTestSrc *this)
{
  VTSBuffer *b;
  SpaPortIO *io = this->io;
  int n_bytes;

  read_timer (this);

  if (spa_list_is_empty (&this->empty)) {
    set_timer (this, false);
    spa_log_error (this->log, "videotestsrc %p: out of buffers", this);
    return SPA_RESULT_OUT_OF_BUFFERS;
  }
  b = spa_list_first (&this->empty, VTSBuffer, link);
  spa_list_remove (&b->link);
  b->outstanding = true;

  n_bytes = b->outbuf->datas[0].maxsize;

  spa_log_trace (this->log, "videotestsrc %p: dequeue buffer %d", this, b->outbuf->id);

  fill_buffer (this, b);

  b->outbuf->datas[0].chunk->offset = 0;
  b->outbuf->datas[0].chunk->size = n_bytes;
  b->outbuf->datas[0].chunk->stride = this->stride;

  if (b->h) {
    b->h->seq = this->frame_count;
    b->h->pts = this->start_time + this->elapsed_time;
    b->h->dts_offset = 0;
  }

  this->frame_count++;
  this->elapsed_time = FRAMES_TO_TIME (this, this->frame_count);
  set_timer (this, true);

  io->buffer_id = b->outbuf->id;
  io->status = SPA_RESULT_HAVE_BUFFER;

  return SPA_RESULT_HAVE_BUFFER;
}

static void
videotestsrc_on_output (SpaSource *source)
{
  SpaVideoTestSrc *this = source->data;
  SpaResult res;

  res = videotestsrc_make_buffer (this);

  if (res == SPA_RESULT_HAVE_BUFFER)
    send_have_output (this);
}

static SpaResult
spa_videotestsrc_node_send_command (SpaNode    *node,
                                    SpaCommand *command)
{
  SpaVideoTestSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (command != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

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
    this->frame_count = 0;
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
spa_videotestsrc_node_set_event_callback (SpaNode              *node,
                                          SpaEventNodeCallback  event_cb,
                                          void                 *user_data)
{
  SpaVideoTestSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  this->event_cb = event_cb;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_get_n_ports (SpaNode       *node,
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
spa_videotestsrc_node_get_port_ids (SpaNode       *node,
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
                                         uint32_t          index)
{
  SpaVideoTestSrc *this;
  SpaResult res;
  SpaFormat *fmt;
  uint8_t buffer[1024];
  SpaPODBuilder b = { NULL, };
  SpaPODFrame f[2];
  uint32_t count, match;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  count = match = filter ? 0 : index;

next:
  spa_pod_builder_init (&b, buffer, sizeof (buffer));

  switch (count++) {
    case 0:
      spa_pod_builder_format (&b, &f[0], this->type.format,
         this->type.media_type.video, this->type.media_subtype.raw,
         PROP_U_EN (&f[1], this->type.format_video.format,    SPA_POD_TYPE_ID,  3,
                                                             this->type.video_format.RGB,
                                                             this->type.video_format.RGB,
                                                             this->type.video_format.UYVY),
         PROP_U_MM (&f[1], this->type.format_video.size,      SPA_POD_TYPE_RECTANGLE,
                                                             320, 240,
                                                             1, 1,
                                                             INT32_MAX, INT32_MAX),
         PROP_U_MM (&f[1], this->type.format_video.framerate, SPA_POD_TYPE_FRACTION,
                                                             25, 1,
                                                             0, 1,
                                                             INT32_MAX, 1));
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
clear_buffers (SpaVideoTestSrc *this)
{
  if (this->n_buffers > 0) {
    spa_log_info (this->log, "videotestsrc %p: clear buffers", this);
    this->n_buffers = 0;
    spa_list_init (&this->empty);
    this->started = false;
    set_timer (this, false);
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

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (format == NULL) {
    this->have_format = false;
    clear_buffers (this);
  } else {
    SpaVideoInfo info = { SPA_FORMAT_MEDIA_TYPE (format),
                          SPA_FORMAT_MEDIA_SUBTYPE (format), };

    if (info.media_type != this->type.media_type.video &&
        info.media_subtype != this->type.media_subtype.raw)
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    if (!spa_format_video_raw_parse (format, &info.info.raw, &this->type.format_video))
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    if (info.info.raw.format == this->type.video_format.RGB) {
      this->bpp = 3;
    }
    else if (info.info.raw.format == this->type.video_format.UYVY) {
      this->bpp = 2;
    }
    else
      return SPA_RESULT_NOT_IMPLEMENTED;

    this->current_format = info;
    this->have_format = true;
  }

  if (this->have_format) {
    SpaVideoInfoRaw *raw_info = &this->current_format.info.raw;
    SpaPODBuilder b = { NULL };
    SpaPODFrame f[2];

    this->info.latency = 0;
    this->info.maxbuffering = -1;

    this->info.n_params = 2;
    this->info.params = this->params;
    this->stride = SPA_ROUND_UP_N (this->bpp * raw_info->size.width, 4);

    spa_pod_builder_init (&b, this->params_buffer, sizeof (this->params_buffer));
    spa_pod_builder_object (&b, &f[0], 0, this->type.alloc_param_buffers.Buffers,
      PROP      (&f[1], this->type.alloc_param_buffers.size,    SPA_POD_TYPE_INT, this->stride * raw_info->size.height),
      PROP      (&f[1], this->type.alloc_param_buffers.stride,  SPA_POD_TYPE_INT, this->stride),
      PROP_U_MM (&f[1], this->type.alloc_param_buffers.buffers, SPA_POD_TYPE_INT, 32, 2, 32),
      PROP      (&f[1], this->type.alloc_param_buffers.align,   SPA_POD_TYPE_INT, 16));
    this->params[0] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

    spa_pod_builder_object (&b, &f[0], 0, this->type.alloc_param_meta_enable.MetaEnable,
      PROP      (&f[1], this->type.alloc_param_meta_enable.type, SPA_POD_TYPE_ID, this->type.meta.Header),
      PROP      (&f[1], this->type.alloc_param_meta_enable.size, SPA_POD_TYPE_INT, sizeof (SpaMetaHeader)));
    this->params[1] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

    this->info.extra = NULL;
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_port_get_format (SpaNode          *node,
                                       SpaDirection      direction,
                                       uint32_t          port_id,
                                       const SpaFormat **format)
{
  SpaVideoTestSrc *this;
  SpaPODBuilder b = { NULL, };
  SpaPODFrame f[2];

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  spa_pod_builder_init (&b, this->format_buffer, sizeof (this->format_buffer));
  spa_pod_builder_format (&b, &f[0], this->type.format,
     this->type.media_type.video, this->type.media_subtype.raw,
     PROP (&f[1], this->type.format_video.format,     SPA_POD_TYPE_ID,        this->current_format.info.raw.format),
     PROP (&f[1], this->type.format_video.size,      -SPA_POD_TYPE_RECTANGLE, &this->current_format.info.raw.size),
     PROP (&f[1], this->type.format_video.framerate, -SPA_POD_TYPE_FRACTION,  &this->current_format.info.raw.framerate));
  *format = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaFormat);

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_port_get_info (SpaNode            *node,
                                     SpaDirection        direction,
                                     uint32_t            port_id,
                                     const SpaPortInfo **info)
{
  SpaVideoTestSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

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
  uint32_t i;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  clear_buffers (this);

  for (i = 0; i < n_buffers; i++) {
    VTSBuffer *b;
    SpaData *d = buffers[i]->datas;

    b = &this->buffers[i];
    b->outbuf = buffers[i];
    b->outstanding = false;
    b->h = spa_buffer_find_meta (buffers[i], this->type.meta.Header);

    if ((d[0].type == this->type.data.MemPtr ||
         d[0].type == this->type.data.MemFd ||
         d[0].type == this->type.data.DmaBuf) &&
        d[0].data == NULL) {
      spa_log_error (this->log, "videotestsrc %p: invalid memory on buffer %p", this, buffers[i]);
    }
    spa_list_insert (this->empty.prev, &b->link);
  }
  this->n_buffers = n_buffers;

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

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_node_port_set_io (SpaNode       *node,
                                   SpaDirection   direction,
                                   uint32_t       port_id,
                                   SpaPortIO     *io)
{
  SpaVideoTestSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  spa_return_val_if_fail (CHECK_PORT_NUM (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  this->io = io;

  return SPA_RESULT_OK;
}

static inline void
reuse_buffer (SpaVideoTestSrc *this, uint32_t id)
{
  VTSBuffer *b = &this->buffers[id];
  spa_return_if_fail (b->outstanding);

  spa_log_trace (this->log, "videotestsrc %p: reuse buffer %d", this, id);

  b->outstanding = false;
  spa_list_insert (this->empty.prev, &b->link);

  if (!this->props.live)
    set_timer (this, true);
}

static SpaResult
spa_videotestsrc_node_port_reuse_buffer (SpaNode         *node,
                                         uint32_t         port_id,
                                         uint32_t         buffer_id)
{
  SpaVideoTestSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  spa_return_val_if_fail (port_id == 0, SPA_RESULT_INVALID_PORT);
  spa_return_val_if_fail (this->n_buffers > 0, SPA_RESULT_NO_BUFFERS);
  spa_return_val_if_fail (buffer_id < this->n_buffers, SPA_RESULT_INVALID_BUFFER_ID);

  reuse_buffer (this, buffer_id);

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_port_send_command (SpaNode        *node,
                                         SpaDirection    direction,
                                         uint32_t        port_id,
                                         SpaCommand     *command)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_node_process_input (SpaNode *node)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_node_process_output (SpaNode *node)
{
  SpaVideoTestSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaVideoTestSrc, node);

  if (this->io && this->io->buffer_id != SPA_ID_INVALID) {
    reuse_buffer (this, this->io->buffer_id);
    this->io->buffer_id = SPA_ID_INVALID;
  }

  if (!this->async)
    return videotestsrc_make_buffer (this);
  else
    return SPA_RESULT_OK;
}

static const SpaNode videotestsrc_node = {
  sizeof (SpaNode),
  NULL,
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
  spa_videotestsrc_node_port_set_io,
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

  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (interface != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = (SpaVideoTestSrc *) handle;

  if (interface_id == this->type.node)
    *interface = &this->node;
  else if (interface_id == this->type.clock)
    *interface = &this->clock;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static SpaResult
videotestsrc_clear (SpaHandle *handle)
{
  SpaVideoTestSrc *this;

  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = (SpaVideoTestSrc *) handle;

  if (this->data_loop)
    spa_loop_remove_source (this->data_loop, &this->timer_source);
  close (this->timer_source.fd);

  return SPA_RESULT_OK;
}

static SpaResult
videotestsrc_init (const SpaHandleFactory  *factory,
                   SpaHandle               *handle,
                   const SpaDict           *info,
                   const SpaSupport        *support,
                   uint32_t                 n_support)
{
  SpaVideoTestSrc *this;
  uint32_t i;
  const char *str;

  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  handle->get_interface = spa_videotestsrc_get_interface;
  handle->clear = videotestsrc_clear;

  this = (SpaVideoTestSrc *) handle;

  if (info && (str = spa_dict_lookup (info, "asynchronous")))
    this->async = atoi (str) == 1;
  else
    this->async = false;

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
  if (this->data_loop == NULL && this->async) {
    spa_log_error (this->log, "a data_loop is needed");
    return SPA_RESULT_ERROR;
  }
  init_type (&this->type, this->map);

  this->node = videotestsrc_node;
  this->clock = videotestsrc_clock;
  reset_videotestsrc_props (this, &this->props);

  spa_list_init (&this->empty);

  this->timer_source.func = videotestsrc_on_output;
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

  spa_log_info (this->log, "videotestsrc %p: initialized, async=%d", this, this->async);

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo videotestsrc_interfaces[] =
{
  { SPA_TYPE__Node, },
  { SPA_TYPE__Clock, },
};

static SpaResult
videotestsrc_enum_interface_info (const SpaHandleFactory  *factory,
                                  const SpaInterfaceInfo **info,
                                  uint32_t                 index)
{
  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  switch (index) {
    case 0:
      *info = &videotestsrc_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_videotestsrc_factory =
{ "videotestsrc",
  NULL,
  sizeof (SpaVideoTestSrc),
  videotestsrc_init,
  videotestsrc_enum_interface_info,
};
