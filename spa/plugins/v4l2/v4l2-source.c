/* Spa V4l2 Source
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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <linux/videodev2.h>

#include <spa/node.h>
#include <spa/video/format-utils.h>
#include <spa/clock.h>
#include <spa/list.h>
#include <spa/log.h>
#include <spa/loop.h>
#include <spa/param-alloc.h>
#include <spa/type-map.h>
#include <spa/format-builder.h>
#include <lib/debug.h>
#include <lib/props.h>

#define NAME "v4l2-source"

static const char default_device[] = "/dev/video0";

struct props {
  char device[64];
  char device_name[128];
  int  device_fd;
};

static void
reset_props (struct props *props)
{
  strncpy (props->device, default_device, 64);
}

#define MAX_BUFFERS     64

struct buffer {
  struct spa_buffer *outbuf;
  struct spa_meta_header *h;
  bool outstanding;
  bool allocated;
  struct v4l2_buffer v4l2_buffer;
};

struct type {
  uint32_t node;
  uint32_t clock;
  uint32_t format;
  uint32_t props;
  uint32_t prop_device;
  uint32_t prop_device_name;
  uint32_t prop_device_fd;
  struct spa_type_media_type media_type;
  struct spa_type_media_subtype media_subtype;
  struct spa_type_media_subtype_video media_subtype_video;
  struct spa_type_format_video format_video;
  struct spa_type_video_format video_format;
  struct spa_type_event_node event_node;
  struct spa_type_command_node command_node;
  struct spa_type_param_alloc_buffers param_alloc_buffers;
  struct spa_type_param_alloc_meta_enable param_alloc_meta_enable;
  struct spa_type_meta meta;
  struct spa_type_data data;
};

static inline void
init_type (struct type *type, struct spa_type_map *map)
{
  type->node = spa_type_map_get_id (map, SPA_TYPE__Node);
  type->clock = spa_type_map_get_id (map, SPA_TYPE__Clock);
  type->format = spa_type_map_get_id (map, SPA_TYPE__Format);
  type->props = spa_type_map_get_id (map, SPA_TYPE__Props);
  type->prop_device = spa_type_map_get_id (map, SPA_TYPE_PROPS__device);
  type->prop_device_name = spa_type_map_get_id (map, SPA_TYPE_PROPS__deviceName);
  type->prop_device_fd = spa_type_map_get_id (map, SPA_TYPE_PROPS__deviceFd);
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_media_subtype_video_map (map, &type->media_subtype_video);
  spa_type_format_video_map (map, &type->format_video);
  spa_type_video_format_map (map, &type->video_format);
  spa_type_event_node_map (map, &type->event_node);
  spa_type_command_node_map (map, &type->command_node);
  spa_type_param_alloc_buffers_map (map, &type->param_alloc_buffers);
  spa_type_param_alloc_meta_enable_map (map, &type->param_alloc_meta_enable);
  spa_type_meta_map (map, &type->meta);
  spa_type_data_map (map, &type->data);
}

struct port {
  struct spa_log *log;
  struct spa_loop *main_loop;
  struct spa_loop *data_loop;

  bool export_buf;
  bool started;

  bool next_fmtdesc;
  struct v4l2_fmtdesc fmtdesc;
  bool next_frmsize;
  struct v4l2_frmsizeenum frmsize;
  struct v4l2_frmivalenum frmival;

  bool have_format;
  struct spa_video_info current_format;
  uint8_t format_buffer[1024];

  int fd;
  bool opened;
  struct v4l2_capability cap;
  struct v4l2_format fmt;
  enum v4l2_buf_type type;
  enum v4l2_memory memtype;

  struct buffer buffers[MAX_BUFFERS];
  uint32_t n_buffers;

  bool source_enabled;
  struct spa_source source;

  struct spa_port_info info;
  uint8_t params_buffer[1024];
  struct spa_port_io *io;

  int64_t last_ticks;
  int64_t last_monotonic;
};

struct impl {
  struct spa_handle handle;
  struct spa_node node;
  struct spa_clock clock;

  struct spa_type_map *map;
  struct spa_log *log;
  struct type type;

  uint32_t seq;

  uint8_t props_buffer[512];
  struct props props;

  struct spa_node_callbacks callbacks;
  void *user_data;

  struct port out_ports[1];
};

#define CHECK_PORT(this,direction,port_id)  ((direction) == SPA_DIRECTION_OUTPUT && (port_id) == 0)


#define PROP(f,key,type,...)                                                    \
          SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
#define PROP_R(f,key,type,...)                                                  \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_READONLY,type,1,__VA_ARGS__)
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
#include "v4l2-utils.c"

static int
impl_node_get_props (struct spa_node       *node,
                                struct spa_props     **props)
{
  struct impl *this;
  struct spa_pod_builder b = { NULL,  };
  struct spa_pod_frame f[2];

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (props != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, struct impl, node);

  spa_pod_builder_init (&b, this->props_buffer, sizeof (this->props_buffer));
  spa_pod_builder_props (&b, &f[0], this->type.props,
      PROP   (&f[1], this->type.prop_device,      -SPA_POD_TYPE_STRING, this->props.device, sizeof (this->props.device)),
      PROP_R (&f[1], this->type.prop_device_name, -SPA_POD_TYPE_STRING, this->props.device_name, sizeof (this->props.device_name)),
      PROP_R (&f[1], this->type.prop_device_fd,    SPA_POD_TYPE_INT,    this->props.device_fd));
  *props = SPA_POD_BUILDER_DEREF (&b, f[0].ref, struct spa_props);

  return SPA_RESULT_OK;
}

static int
impl_node_set_props (struct spa_node         *node,
                                const struct spa_props  *props)
{
  struct impl *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, struct impl, node);

  if (props == NULL) {
    reset_props (&this->props);
    return SPA_RESULT_OK;
  } else {
    spa_props_query (props,
        this->type.prop_device, -SPA_POD_TYPE_STRING, this->props.device, sizeof (this->props.device),
        0);
  }
  return SPA_RESULT_OK;
}

static int
do_pause_done (struct spa_loop *loop,
               bool             async,
               uint32_t         seq,
               size_t           size,
               void            *data,
               void            *user_data)
{
  struct impl *this = user_data;
  struct spa_event_node_async_complete *ac = data;

  if (SPA_RESULT_IS_OK (ac->body.res.value))
    ac->body.res.value = spa_v4l2_stream_off (this);

  this->callbacks.event (&this->node, (struct spa_event *)ac, this->user_data);

  return SPA_RESULT_OK;
}

static int
do_pause (struct spa_loop *loop,
          bool             async,
          uint32_t         seq,
          size_t           size,
          void            *data,
          void            *user_data)
{
  struct impl *this = user_data;
  int res;
  struct spa_command *cmd = data;

  res = spa_node_port_send_command (&this->node,
                                    SPA_DIRECTION_OUTPUT,
                                    0,
                                    cmd);

  if (async) {
    spa_loop_invoke (this->out_ports[0].main_loop,
                     do_pause_done,
                     seq,
                     sizeof (struct spa_event_node_async_complete),
                     &SPA_EVENT_NODE_ASYNC_COMPLETE_INIT (this->type.event_node.AsyncComplete,
                                                          seq, res),
                     this);
  }
  return res;
}

static int
do_start_done (struct spa_loop *loop,
               bool             async,
               uint32_t         seq,
               size_t           size,
               void            *data,
               void            *user_data)
{
  struct impl *this = user_data;
  struct spa_event_node_async_complete *ac = data;

  this->callbacks.event (&this->node, (struct spa_event *)ac, this->user_data);

  return SPA_RESULT_OK;
}

static int
do_start (struct spa_loop *loop,
          bool             async,
          uint32_t         seq,
          size_t           size,
          void            *data,
          void            *user_data)
{
  struct impl *this = user_data;
  int res;
  struct spa_command *cmd = data;

  res = spa_node_port_send_command (&this->node,
                                    SPA_DIRECTION_OUTPUT,
                                    0,
                                    cmd);

  if (async) {
    spa_loop_invoke (this->out_ports[0].main_loop,
                     do_start_done,
                     seq,
                     sizeof (struct spa_event_node_async_complete),
                     &SPA_EVENT_NODE_ASYNC_COMPLETE_INIT (this->type.event_node.AsyncComplete,
                                                          seq, res),
                     this);
  }

  return SPA_RESULT_OK;
}

static int
impl_node_send_command (struct spa_node    *node,
                                   struct spa_command *command)
{
  struct impl *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (command != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, struct impl, node);

  if (SPA_COMMAND_TYPE (command) == this->type.command_node.Start) {
    struct port *state = &this->out_ports[0];
    int res;

    if (!state->have_format)
      return SPA_RESULT_NO_FORMAT;

    if (state->n_buffers == 0)
      return SPA_RESULT_NO_BUFFERS;

    if ((res = spa_v4l2_stream_on (this)) < 0)
      return res;

    return spa_loop_invoke (this->out_ports[0].data_loop,
                            do_start,
                            ++this->seq,
                            SPA_POD_SIZE (command),
                            command,
                            this);
  }
  else if (SPA_COMMAND_TYPE (command) == this->type.command_node.Pause) {
    struct port *state = &this->out_ports[0];

    if (!state->have_format)
      return SPA_RESULT_NO_FORMAT;

    if (state->n_buffers == 0)
      return SPA_RESULT_NO_BUFFERS;

    return spa_loop_invoke (this->out_ports[0].data_loop,
                            do_pause,
                            ++this->seq,
                            SPA_POD_SIZE (command),
                            command,
                            this);
  }
  else if (SPA_COMMAND_TYPE (command) == this->type.command_node.ClockUpdate) {
    return SPA_RESULT_OK;
  }
  else
    return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_set_callbacks (struct spa_node                *node,
                                    const struct spa_node_callbacks *callbacks,
                                    size_t                  callbacks_size,
                                    void                   *user_data)
{
  struct impl *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, struct impl, node);

  this->callbacks = *callbacks;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static int
impl_node_get_n_ports (struct spa_node       *node,
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

static int
impl_node_get_port_ids (struct spa_node       *node,
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


static int
impl_node_add_port (struct spa_node        *node,
                               enum spa_direction    direction,
                               uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_remove_port (struct spa_node        *node,
                                  enum spa_direction    direction,
                                  uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_port_enum_formats (struct spa_node         *node,
                                        enum spa_direction     direction,
                                        uint32_t         port_id,
                                        struct spa_format      **format,
                                        const struct spa_format *filter,
                                        uint32_t         index)
{
  struct impl *this;
  int res;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, struct impl, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  res = spa_v4l2_enum_format (this, format, filter, index);

  return res;
}

static int
impl_node_port_set_format (struct spa_node         *node,
                                      enum spa_direction     direction,
                                      uint32_t         port_id,
                                      uint32_t         flags,
                                      const struct spa_format *format)
{
  struct impl *this;
  struct port *state;
  struct spa_video_info info;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, struct impl, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  state = &this->out_ports[port_id];

  if (format == NULL) {
    spa_v4l2_stream_off (this);
    spa_v4l2_clear_buffers (this);
    spa_v4l2_close (this);
    state->have_format = false;
    return SPA_RESULT_OK;
  } else {
    info.media_type = SPA_FORMAT_MEDIA_TYPE (format);
    info.media_subtype = SPA_FORMAT_MEDIA_SUBTYPE (format);

    if (info.media_type != this->type.media_type.video) {
      spa_log_error (this->log, "media type must be video");
      return SPA_RESULT_INVALID_MEDIA_TYPE;
    }

    if (info.media_subtype == this->type.media_subtype.raw) {
      if (!spa_format_video_raw_parse (format, &info.info.raw, &this->type.format_video)) {
        spa_log_error (this->log, "can't parse video raw");
        return SPA_RESULT_INVALID_MEDIA_TYPE;
      }

      if (state->have_format && info.media_type == state->current_format.media_type &&
          info.media_subtype == state->current_format.media_subtype &&
          info.info.raw.format == state->current_format.info.raw.format &&
          info.info.raw.size.width == state->current_format.info.raw.size.width &&
          info.info.raw.size.height == state->current_format.info.raw.size.height)
        return SPA_RESULT_OK;
    }
    else if (info.media_subtype == this->type.media_subtype_video.mjpg) {
      if (!spa_format_video_mjpg_parse (format, &info.info.mjpg, &this->type.format_video))
        return SPA_RESULT_INVALID_MEDIA_TYPE;

      if (state->have_format && info.media_type == state->current_format.media_type &&
          info.media_subtype == state->current_format.media_subtype &&
          info.info.mjpg.size.width == state->current_format.info.mjpg.size.width &&
          info.info.mjpg.size.height == state->current_format.info.mjpg.size.height)
        return SPA_RESULT_OK;
    }
    else if (info.media_subtype == this->type.media_subtype_video.h264) {
      if (!spa_format_video_h264_parse (format, &info.info.h264, &this->type.format_video))
        return SPA_RESULT_INVALID_MEDIA_TYPE;

      if (state->have_format && info.media_type == state->current_format.media_type &&
          info.media_subtype == state->current_format.media_subtype &&
          info.info.h264.size.width == state->current_format.info.h264.size.width &&
          info.info.h264.size.height == state->current_format.info.h264.size.height)
        return SPA_RESULT_OK;
    }
  }

  if (state->have_format && !(flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY)) {
    spa_v4l2_use_buffers (this, NULL, 0);
    state->have_format = false;
  }

  if (spa_v4l2_set_format (this, &info, flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY) < 0)
    return SPA_RESULT_INVALID_MEDIA_TYPE;

  if (!(flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY)) {
    state->current_format = info;
    state->have_format = true;
  }

  return SPA_RESULT_OK;
}

static int
impl_node_port_get_format (struct spa_node          *node,
                                      enum spa_direction      direction,
                                      uint32_t          port_id,
                                      const struct spa_format **format)
{
  struct impl *this;
  struct port *state;
  struct spa_pod_builder b = { NULL, };
  struct spa_pod_frame f[2];

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, struct impl, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  state = &this->out_ports[port_id];

  if (!state->have_format)
    return SPA_RESULT_NO_FORMAT;

  b.data = state->format_buffer;
  b.size = sizeof (state->format_buffer);

  spa_pod_builder_push_format (&b, &f[0], this->type.format,
                               state->current_format.media_type,
                               state->current_format.media_subtype);

  if (state->current_format.media_subtype == this->type.media_subtype.raw) {
    spa_pod_builder_add (&b,
        PROP (&f[1], this->type.format_video.format,     SPA_POD_TYPE_ID,        state->current_format.info.raw.format),
        PROP (&f[1], this->type.format_video.size,      -SPA_POD_TYPE_RECTANGLE, &state->current_format.info.raw.size),
        PROP (&f[1], this->type.format_video.framerate, -SPA_POD_TYPE_FRACTION,  &state->current_format.info.raw.framerate),
        0);
  }
  else if (state->current_format.media_subtype == this->type.media_subtype_video.mjpg ||
           state->current_format.media_subtype == this->type.media_subtype_video.jpeg) {
    spa_pod_builder_add (&b,
        PROP (&f[1], this->type.format_video.size,      -SPA_POD_TYPE_RECTANGLE, &state->current_format.info.mjpg.size),
        PROP (&f[1], this->type.format_video.framerate, -SPA_POD_TYPE_FRACTION,  &state->current_format.info.mjpg.framerate),
        0);
  }
  else if (state->current_format.media_subtype == this->type.media_subtype_video.h264) {
    spa_pod_builder_add (&b,
        PROP (&f[1], this->type.format_video.size,      -SPA_POD_TYPE_RECTANGLE, &state->current_format.info.h264.size),
        PROP (&f[1], this->type.format_video.framerate, -SPA_POD_TYPE_FRACTION,  &state->current_format.info.h264.framerate),
        0);
  } else
    return SPA_RESULT_NO_FORMAT;

  spa_pod_builder_pop (&b, &f[0]);

  *format = SPA_POD_BUILDER_DEREF (&b, f[0].ref, struct spa_format);

  return SPA_RESULT_OK;
}

static int
impl_node_port_get_info (struct spa_node            *node,
                                    enum spa_direction        direction,
                                    uint32_t            port_id,
                                    const struct spa_port_info **info)
{
  struct impl *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, struct impl, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  *info = &this->out_ports[port_id].info;

  return SPA_RESULT_OK;
}

static int
impl_node_port_enum_params (struct spa_node       *node,
                                       enum spa_direction   direction,
                                       uint32_t       port_id,
                                       uint32_t       index,
                                       struct spa_param     **param)
{

  struct impl *this;
  struct port *state;
  struct spa_pod_builder b = { NULL, };
  struct spa_pod_frame f[2];

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (param != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, struct impl, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  state = &this->out_ports[port_id];

  spa_pod_builder_init (&b, state->params_buffer, sizeof (state->params_buffer));

  switch (index) {
  case 0:
    spa_pod_builder_object (&b, &f[0], 0, this->type.param_alloc_buffers.Buffers,
        PROP      (&f[1], this->type.param_alloc_buffers.size,    SPA_POD_TYPE_INT, state->fmt.fmt.pix.sizeimage),
        PROP      (&f[1], this->type.param_alloc_buffers.stride,  SPA_POD_TYPE_INT, state->fmt.fmt.pix.bytesperline),
        PROP_U_MM (&f[1], this->type.param_alloc_buffers.buffers, SPA_POD_TYPE_INT, MAX_BUFFERS, 2, MAX_BUFFERS),
        PROP      (&f[1], this->type.param_alloc_buffers.align,   SPA_POD_TYPE_INT, 16));
    break;

  case 1:
    spa_pod_builder_object (&b, &f[0], 0, this->type.param_alloc_meta_enable.MetaEnable,
        PROP      (&f[1], this->type.param_alloc_meta_enable.type, SPA_POD_TYPE_ID, this->type.meta.Header),
        PROP      (&f[1], this->type.param_alloc_meta_enable.size, SPA_POD_TYPE_INT, sizeof (struct spa_meta_header)));
    break;

  default:
    return SPA_RESULT_NOT_IMPLEMENTED;
  }

  *param = SPA_POD_BUILDER_DEREF (&b, f[0].ref, struct spa_param);

  return SPA_RESULT_OK;
}

static int
impl_node_port_set_param (struct spa_node         *node,
                                     enum spa_direction     direction,
                                     uint32_t         port_id,
                                     const struct spa_param  *param)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_port_use_buffers (struct spa_node            *node,
                                       enum spa_direction        direction,
                                       uint32_t            port_id,
                                       struct spa_buffer **buffers,
                                       uint32_t            n_buffers)
{
  struct impl *this;
  struct port *state;
  int res;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, struct impl, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  state = &this->out_ports[port_id];

  if (!state->have_format)
    return SPA_RESULT_NO_FORMAT;

  if (state->n_buffers) {
    spa_v4l2_stream_off (this);
    if ((res = spa_v4l2_clear_buffers (this)) < 0)
      return res;
  }
  if (buffers != NULL) {
    if ((res = spa_v4l2_use_buffers (this, buffers, n_buffers)) < 0)
      return res;
  }
  return SPA_RESULT_OK;
}

static int
impl_node_port_alloc_buffers (struct spa_node            *node,
                                         enum spa_direction        direction,
                                         uint32_t            port_id,
                                         struct spa_param          **params,
                                         uint32_t            n_params,
                                         struct spa_buffer **buffers,
                                         uint32_t           *n_buffers)
{
  struct impl *this;
  struct port *state;
  int res;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (buffers != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, struct impl, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  state = &this->out_ports[port_id];

  if (!state->have_format)
    return SPA_RESULT_NO_FORMAT;

  res = spa_v4l2_alloc_buffers (this, params, n_params, buffers, n_buffers);

  return res;
}

static int
impl_node_port_set_io (struct spa_node       *node,
                                  enum spa_direction   direction,
                                  uint32_t       port_id,
                                  struct spa_port_io     *io)
{
  struct impl *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, struct impl, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  this->out_ports[port_id].io = io;

  return SPA_RESULT_OK;
}

static int
impl_node_port_reuse_buffer (struct spa_node         *node,
                                        uint32_t         port_id,
                                        uint32_t         buffer_id)
{
  struct impl *this;
  struct port *state;
  int res;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (port_id == 0, SPA_RESULT_INVALID_PORT);

  this = SPA_CONTAINER_OF (node, struct impl, node);
  state = &this->out_ports[port_id];

  spa_return_val_if_fail (state->n_buffers > 0, SPA_RESULT_NO_BUFFERS);
  spa_return_val_if_fail (buffer_id < state->n_buffers, SPA_RESULT_INVALID_BUFFER_ID);

  res = spa_v4l2_buffer_recycle (this, buffer_id);

  return res;
}

static int
impl_node_port_send_command (struct spa_node            *node,
                                        enum spa_direction        direction,
                                        uint32_t            port_id,
                                        struct spa_command *command)
{
  struct impl *this;
  int res;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, struct impl, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (SPA_COMMAND_TYPE (command) == this->type.command_node.Pause) {
    res = spa_v4l2_port_set_enabled (this, false);
  }
  else if (SPA_COMMAND_TYPE (command) == this->type.command_node.Start) {
    res = spa_v4l2_port_set_enabled (this, true);
  }
  else
    res = SPA_RESULT_NOT_IMPLEMENTED;

  return res;
}

static int
impl_node_process_input (struct spa_node *node)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_process_output (struct spa_node *node)
{
  struct impl *this;
  int res = SPA_RESULT_OK;
  struct spa_port_io *io;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, struct impl, node);
  io = this->out_ports[0].io;
  spa_return_val_if_fail (io != NULL, SPA_RESULT_WRONG_STATE);

  if (io->status == SPA_RESULT_HAVE_BUFFER)
    return SPA_RESULT_HAVE_BUFFER;

  if (io->buffer_id != SPA_ID_INVALID) {
    res = spa_v4l2_buffer_recycle (this, io->buffer_id);
    io->buffer_id = SPA_ID_INVALID;
  }
  return res;
}

static const struct spa_node impl_node = {
  sizeof (struct spa_node),
  NULL,
  impl_node_get_props,
  impl_node_set_props,
  impl_node_send_command,
  impl_node_set_callbacks,
  impl_node_get_n_ports,
  impl_node_get_port_ids,
  impl_node_add_port,
  impl_node_remove_port,
  impl_node_port_enum_formats,
  impl_node_port_set_format,
  impl_node_port_get_format,
  impl_node_port_get_info,
  impl_node_port_enum_params,
  impl_node_port_set_param,
  impl_node_port_use_buffers,
  impl_node_port_alloc_buffers,
  impl_node_port_set_io,
  impl_node_port_reuse_buffer,
  impl_node_port_send_command,
  impl_node_process_input,
  impl_node_process_output,
};

static int
impl_clock_get_props (struct spa_clock  *clock,
                                 struct spa_props         **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_clock_set_props (struct spa_clock *clock,
                                 const struct spa_props   *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_clock_get_time (struct spa_clock *clock,
                                int32_t          *rate,
                                int64_t          *ticks,
                                int64_t          *monotonic_time)
{
  struct impl *this;
  struct port *state;

  spa_return_val_if_fail (clock != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (clock, struct impl, clock);
  state = &this->out_ports[0];

  if (rate)
    *rate = SPA_USEC_PER_SEC;
  if (ticks)
    *ticks = state->last_ticks;
  if (monotonic_time)
    *monotonic_time = state->last_monotonic;

  return SPA_RESULT_OK;
}

static const struct spa_clock impl_clock = {
  sizeof (struct spa_clock),
  NULL,
  SPA_CLOCK_STATE_STOPPED,
  impl_clock_get_props,
  impl_clock_set_props,
  impl_clock_get_time,
};

static int
impl_get_interface (struct spa_handle               *handle,
                               uint32_t                 interface_id,
                               void                   **interface)
{
  struct impl *this;

  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (interface != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = (struct impl *) handle;

  if (interface_id == this->type.node)
    *interface = &this->node;
  else if (interface_id == this->type.clock)
    *interface = &this->clock;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static int
impl_clear (struct spa_handle *handle)
{
  return SPA_RESULT_OK;
}

static int
impl_init (const struct spa_handle_factory  *factory,
                  struct spa_handle               *handle,
                  const struct spa_dict           *info,
                  const struct spa_support        *support,
                  uint32_t                 n_support)
{
  struct impl *this;
  uint32_t i;
  const char *str;

  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  handle->get_interface = impl_get_interface;
  handle->clear = impl_clear,

  this = (struct impl *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].type, SPA_TYPE__TypeMap) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE__Log) == 0)
      this->log = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE_LOOP__MainLoop) == 0)
      this->out_ports[0].main_loop = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE_LOOP__DataLoop) == 0)
      this->out_ports[0].data_loop = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "a type-map is needed");
    return SPA_RESULT_ERROR;
  }
  if (this->out_ports[0].main_loop == NULL) {
    spa_log_error (this->log, "a main_loop is needed");
    return SPA_RESULT_ERROR;
  }
  if (this->out_ports[0].data_loop == NULL) {
    spa_log_error (this->log, "a data_loop is needed");
    return SPA_RESULT_ERROR;
  }
  init_type (&this->type, this->map);

  this->node = impl_node;
  this->clock = impl_clock;

  reset_props (&this->props);

  this->out_ports[0].log = this->log;
  this->out_ports[0].info.flags = SPA_PORT_INFO_FLAG_LIVE;

  this->out_ports[0].export_buf = true;

  if (info && (str = spa_dict_lookup (info, "device.path"))) {
    strncpy (this->props.device, str, 63);
  }

  return SPA_RESULT_OK;
}

static const struct spa_interface_info impl_interfaces[] =
{
  { SPA_TYPE__Node, },
  { SPA_TYPE__Clock, },
};

static int
impl_enum_interface_info (const struct spa_handle_factory  *factory,
                                 const struct spa_interface_info **info,
                                 uint32_t                 index)
{
  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  if (index < 0 || index >= SPA_N_ELEMENTS (impl_interfaces))
    return SPA_RESULT_ENUM_END;

  *info = &impl_interfaces[index];
  return SPA_RESULT_OK;
}

const struct spa_handle_factory spa_v4l2_source_factory =
{ NAME,
  NULL,
  sizeof (struct impl),
  impl_init,
  impl_enum_interface_info,
};
