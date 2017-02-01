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
#include <spa/video/format.h>
#include <spa/list.h>
#include <spa/log.h>
#include <spa/loop.h>
#include <spa/id-map.h>
#include <lib/debug.h>
#include <lib/props.h>

typedef struct _SpaV4l2Source SpaV4l2Source;

static const char default_device[] = "/dev/video0";

typedef struct {
  SpaProps props;
  char device[64];
  char device_name[128];
  int  device_fd;
} SpaV4l2SourceProps;

static void
reset_v4l2_source_props (SpaV4l2SourceProps *props)
{
  strncpy (props->device, default_device, 64);
  props->props.unset_mask = 7;
}

#define MAX_BUFFERS     64

typedef struct _V4l2Buffer V4l2Buffer;

struct _V4l2Buffer {
  SpaBuffer *outbuf;
  SpaMetaHeader *h;
  bool outstanding;
  bool allocated;
  struct v4l2_buffer v4l2_buffer;
};

typedef struct _V4l2Format V4l2Format;

struct _V4l2Format {
  SpaFormat fmt;
  SpaVideoFormat            format;
  SpaRectangle              size;
  SpaFraction               framerate;
  SpaVideoInterlaceMode     interlace_mode;
  SpaVideoColorRange        color_range;
  SpaVideoColorMatrix       color_matrix;
  SpaVideoTransferFunction  transfer_function;
  SpaVideoColorPrimaries    color_primaries;
  SpaPropInfo infos[16];
  SpaPropRangeInfo ranges[16];
  SpaFraction framerates[16];
};

typedef struct {
  uint32_t node;
  uint32_t clock;
} URI;

typedef struct {
  SpaLog *log;
  SpaLoop *main_loop;
  SpaLoop *data_loop;

  bool export_buf;
  bool started;

  bool next_fmtdesc;
  struct v4l2_fmtdesc fmtdesc;
  bool next_frmsize;
  struct v4l2_frmsizeenum frmsize;
  struct v4l2_frmivalenum frmival;

  V4l2Format format[2];
  V4l2Format *current_format;

  int fd;
  bool opened;
  struct v4l2_capability cap;
  struct v4l2_format fmt;
  enum v4l2_buf_type type;
  enum v4l2_memory memtype;

  V4l2Buffer   buffers[MAX_BUFFERS];
  unsigned int n_buffers;

  bool source_enabled;
  SpaSource source;

  SpaPortInfo info;
  SpaAllocParam *params[2];
  SpaAllocParamBuffers param_buffers;
  SpaAllocParamMetaEnable param_meta;
  void *io;

  int64_t last_ticks;
  int64_t last_monotonic;
} SpaV4l2State;

struct _SpaV4l2Source {
  SpaHandle handle;
  SpaNode node;
  SpaClock clock;

  URI uri;
  SpaIDMap *map;
  SpaLog *log;

  uint32_t seq;

  SpaV4l2SourceProps props[2];

  SpaNodeEventCallback event_cb;
  void *user_data;

  SpaV4l2State state[1];
};

#define CHECK_PORT(this, direction, port_id)  ((direction) == SPA_DIRECTION_OUTPUT && (port_id) == 0)

static void
update_state (SpaV4l2Source *this, SpaNodeState state)
{
  spa_log_info (this->log, "state: %d", state);
  this->node.state = state;
}
#include "v4l2-utils.c"

enum {
  PROP_ID_DEVICE,
  PROP_ID_DEVICE_NAME,
  PROP_ID_DEVICE_FD,
  PROP_ID_LAST,
};

static const SpaPropInfo prop_info[] =
{
  { PROP_ID_DEVICE,             offsetof (SpaV4l2SourceProps, device),
                                "device",
                                SPA_PROP_FLAG_READWRITE,
                                SPA_PROP_TYPE_STRING, 63,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
  { PROP_ID_DEVICE_NAME,        offsetof (SpaV4l2SourceProps, device_name),
                                "device-name",
                                SPA_PROP_FLAG_READABLE,
                                SPA_PROP_TYPE_STRING, 127,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
  { PROP_ID_DEVICE_FD,          offsetof (SpaV4l2SourceProps, device_fd),
                                "device-fd",
                                SPA_PROP_FLAG_READABLE,
                                SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
};

static SpaResult
spa_v4l2_source_node_get_props (SpaNode       *node,
                                SpaProps     **props)
{
  SpaV4l2Source *this;

  if (node == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));
  *props = &this->props[0].props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_set_props (SpaNode         *node,
                                const SpaProps  *props)
{
  SpaV4l2Source *this;
  SpaV4l2SourceProps *p;
  SpaResult res;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);
  p = &this->props[1];

  if (props == NULL) {
    reset_v4l2_source_props (p);
    return SPA_RESULT_OK;
  }

  res = spa_props_copy_values (props, &p->props);

  return res;
}

static SpaResult
do_pause_done (SpaLoop        *loop,
               bool            async,
               uint32_t        seq,
               size_t          size,
               void           *data,
               void           *user_data)
{
  SpaV4l2Source *this = user_data;
  SpaV4l2State *state = &this->state[0];
  SpaNodeEventAsyncComplete *ac = data;


  if (SPA_RESULT_IS_OK (ac->res))
    ac->res = spa_v4l2_stream_off (this);

  if (SPA_RESULT_IS_OK (ac->res)) {
    state->started = false;
    update_state (this, SPA_NODE_STATE_PAUSED);
  }
  this->event_cb (&this->node, &ac->event, this->user_data);

  return SPA_RESULT_OK;
}

static SpaResult
do_pause (SpaLoop        *loop,
          bool            async,
          uint32_t        seq,
          size_t          size,
          void           *data,
          void           *user_data)
{
  SpaV4l2Source *this = user_data;
  SpaResult res;
  SpaNodeEventAsyncComplete ac;
  SpaNodeCommand *cmd = data;

  res = spa_node_port_send_command (&this->node,
                                    SPA_DIRECTION_OUTPUT,
                                    0,
                                    cmd);

  if (async) {
    ac.event.type = SPA_NODE_EVENT_TYPE_ASYNC_COMPLETE;
    ac.event.size = sizeof (SpaNodeEventAsyncComplete);
    ac.seq = seq;
    ac.res = res;
    spa_loop_invoke (this->state[0].main_loop,
                     do_pause_done,
                     seq,
                     sizeof (ac),
                     &ac,
                     this);
  }
  return res;
}

static SpaResult
do_start_done (SpaLoop        *loop,
               bool            async,
               uint32_t        seq,
               size_t          size,
               void           *data,
               void           *user_data)
{
  SpaV4l2Source *this = user_data;
  SpaV4l2State *state = &this->state[0];
  SpaNodeEventAsyncComplete *ac = data;

  if (SPA_RESULT_IS_OK (ac->res)) {
    state->started = true;
    update_state (this, SPA_NODE_STATE_STREAMING);
  }
  this->event_cb (&this->node, &ac->event, this->user_data);

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
  SpaV4l2Source *this = user_data;
  SpaResult res;
  SpaNodeEventAsyncComplete ac;
  SpaNodeCommand *cmd = data;

  res = spa_node_port_send_command (&this->node,
                                    SPA_DIRECTION_OUTPUT,
                                    0,
                                    cmd);

  if (async) {
    ac.event.type = SPA_NODE_EVENT_TYPE_ASYNC_COMPLETE;
    ac.event.size = sizeof (SpaNodeEventAsyncComplete);
    ac.seq = seq;
    ac.res = res;
    spa_loop_invoke (this->state[0].main_loop,
                     do_start_done,
                     seq,
                     sizeof (ac),
                     &ac,
                     this);
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_send_command (SpaNode        *node,
                                   SpaNodeCommand *command)
{
  SpaV4l2Source *this;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  switch (command->type) {
    case SPA_NODE_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_NODE_COMMAND_START:
    {
      SpaV4l2State *state = &this->state[0];
      SpaResult res;

      if (state->current_format == NULL)
        return SPA_RESULT_NO_FORMAT;

      if (state->n_buffers == 0)
        return SPA_RESULT_NO_BUFFERS;

      if (state->started)
        return SPA_RESULT_OK;

      if ((res = spa_v4l2_stream_on (this)) < 0)
        return res;

      return spa_loop_invoke (this->state[0].data_loop,
                              do_start,
                              ++this->seq,
                              command->size,
                              command,
                              this);
    }
    case SPA_NODE_COMMAND_PAUSE:
    {
      SpaV4l2State *state = &this->state[0];

      if (state->current_format == NULL)
        return SPA_RESULT_NO_FORMAT;

      if (state->n_buffers == 0)
        return SPA_RESULT_NO_BUFFERS;

      if (!state->started)
        return SPA_RESULT_OK;

      return spa_loop_invoke (this->state[0].data_loop,
                              do_pause,
                              ++this->seq,
                              command->size,
                              command,
                              this);
    }

    case SPA_NODE_COMMAND_FLUSH:
    case SPA_NODE_COMMAND_DRAIN:
    case SPA_NODE_COMMAND_MARKER:
      return SPA_RESULT_NOT_IMPLEMENTED;

    case SPA_NODE_COMMAND_CLOCK_UPDATE:
    {
      return SPA_RESULT_OK;
    }
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_set_event_callback (SpaNode              *node,
                                         SpaNodeEventCallback  event,
                                         void                 *user_data)
{
  SpaV4l2Source *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_get_n_ports (SpaNode       *node,
                                  unsigned int  *n_input_ports,
                                  unsigned int  *max_input_ports,
                                  unsigned int  *n_output_ports,
                                  unsigned int  *max_output_ports)
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
spa_v4l2_source_node_get_port_ids (SpaNode       *node,
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
spa_v4l2_source_node_add_port (SpaNode        *node,
                               SpaDirection    direction,
                               uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_remove_port (SpaNode        *node,
                                  SpaDirection    direction,
                                  uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_format_init (V4l2Format *f, const SpaFormat *sf)
{
  f->fmt.props.n_prop_info = 3;
  f->fmt.props.prop_info = f->infos;

  spa_prop_info_fill_video (&f->infos[0],
                            SPA_PROP_ID_VIDEO_FORMAT,
                            offsetof (V4l2Format, format));
  spa_prop_info_fill_video (&f->infos[1],
                            SPA_PROP_ID_VIDEO_SIZE,
                            offsetof (V4l2Format, size));
  spa_prop_info_fill_video (&f->infos[2],
                            SPA_PROP_ID_VIDEO_FRAMERATE,
                            offsetof (V4l2Format, framerate));

  f->fmt.media_type = sf->media_type;
  f->fmt.media_subtype = sf->media_subtype;

  return spa_props_copy_values (&sf->props, &f->fmt.props);
}

static SpaResult
spa_v4l2_source_node_port_enum_formats (SpaNode         *node,
                                        SpaDirection     direction,
                                        uint32_t         port_id,
                                        SpaFormat      **format,
                                        const SpaFormat *filter,
                                        unsigned int     index)
{
  SpaV4l2Source *this;
  SpaResult res;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  res = spa_v4l2_enum_format (this, format, filter, index);

  return res;
}

static SpaResult
spa_v4l2_source_node_port_set_format (SpaNode            *node,
                                      SpaDirection        direction,
                                      uint32_t            port_id,
                                      SpaPortFormatFlags  flags,
                                      const SpaFormat    *format)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;
  SpaResult res;
  V4l2Format *f, *tf;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (format == NULL) {
    spa_v4l2_stream_off (this);
    spa_v4l2_clear_buffers (this);
    spa_v4l2_close (this);
    state->current_format = NULL;
    update_state (this, SPA_NODE_STATE_CONFIGURE);
    return SPA_RESULT_OK;
  }

  f = &state->format[0];
  tf = &state->format[1];

  if ((SpaFormat*)f != format) {
    if ((res = spa_v4l2_format_init (f, format)) < 0)
      return res;
  } else {
    f = (V4l2Format*)format;
  }

  if (state->current_format) {
    if (f->fmt.media_type == state->current_format->fmt.media_type &&
       f->fmt.media_subtype == state->current_format->fmt.media_subtype &&
       f->format == state->current_format->format &&
       f->size.width == state->current_format->size.width &&
       f->size.height == state->current_format->size.height)
      return SPA_RESULT_OK;

    if (!(flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY)) {
      spa_v4l2_use_buffers (this, NULL, 0);
      state->current_format = NULL;
    }
  }

  if (spa_v4l2_set_format (this, f, flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY) < 0)
    return SPA_RESULT_INVALID_MEDIA_TYPE;

  if (!(flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY)) {
    if ((res = spa_v4l2_format_init (tf, &f->fmt)) < 0)
      return res;
    state->current_format = tf;

    update_state (this, SPA_NODE_STATE_READY);
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_get_format (SpaNode          *node,
                                      SpaDirection      direction,
                                      uint32_t          port_id,
                                      const SpaFormat **format)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (state->current_format == NULL)
    return SPA_RESULT_NO_FORMAT;

  *format = &state->current_format->fmt;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_get_info (SpaNode            *node,
                                    SpaDirection        direction,
                                    uint32_t            port_id,
                                    const SpaPortInfo **info)
{
  SpaV4l2Source *this;

  if (node == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  *info = &this->state[port_id].info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_get_props (SpaNode       *node,
                                     SpaDirection   direction,
                                     uint32_t       port_id,
                                     SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_port_set_props (SpaNode         *node,
                                     SpaDirection     direction,
                                     uint32_t         port_id,
                                     const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_port_use_buffers (SpaNode         *node,
                                       SpaDirection     direction,
                                       uint32_t         port_id,
                                       SpaBuffer      **buffers,
                                       uint32_t         n_buffers)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;
  SpaResult res;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (state->current_format == NULL)
    return SPA_RESULT_NO_FORMAT;

  if (state->n_buffers) {
    if ((res = spa_v4l2_clear_buffers (this)) < 0)
      return res;
  }
  if (buffers != NULL) {
    if ((res = spa_v4l2_use_buffers (this, buffers, n_buffers)) < 0)
      return res;
  }

  if (state->n_buffers)
    update_state (this, SPA_NODE_STATE_PAUSED);
  else
    update_state (this, SPA_NODE_STATE_READY);

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_alloc_buffers (SpaNode         *node,
                                         SpaDirection     direction,
                                         uint32_t         port_id,
                                         SpaAllocParam  **params,
                                         unsigned int     n_params,
                                         SpaBuffer      **buffers,
                                         unsigned int    *n_buffers)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;
  SpaResult res;

  if (node == NULL || buffers == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (state->current_format == NULL)
    return SPA_RESULT_NO_FORMAT;

  res = spa_v4l2_alloc_buffers (this, params, n_params, buffers, n_buffers);

  if (state->n_buffers) {
    if (state->started)
      update_state (this, SPA_NODE_STATE_STREAMING);
    else
      update_state (this, SPA_NODE_STATE_PAUSED);
  }

  return res;
}

static SpaResult
spa_v4l2_source_node_port_set_input (SpaNode      *node,
                                     uint32_t      port_id,
                                     SpaPortInput *input)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_port_set_output (SpaNode       *node,
                                      uint32_t       port_id,
                                      SpaPortOutput *output)
{
  SpaV4l2Source *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  if (!CHECK_PORT (this, SPA_DIRECTION_OUTPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->state[port_id].io = output;

  return SPA_RESULT_OK;
}


static SpaResult
spa_v4l2_source_node_port_reuse_buffer (SpaNode         *node,
                                        uint32_t         port_id,
                                        uint32_t         buffer_id)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;
  SpaResult res;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (port_id == 0, SPA_RESULT_INVALID_PORT);

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);
  state = &this->state[port_id];

  spa_return_val_if_fail (state->n_buffers > 0, SPA_RESULT_NO_BUFFERS);
  spa_return_val_if_fail (buffer_id < state->n_buffers, SPA_RESULT_INVALID_BUFFER_ID);

  res = spa_v4l2_buffer_recycle (this, buffer_id);

  return res;
}

static SpaResult
spa_v4l2_source_node_port_send_command (SpaNode        *node,
                                        SpaDirection    direction,
                                        uint32_t        port_id,
                                        SpaNodeCommand *command)
{
  SpaV4l2Source *this;
  SpaResult res;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  switch (command->type) {
    case SPA_NODE_COMMAND_PAUSE:
      res = spa_v4l2_port_set_enabled (this, false);
      break;

    case SPA_NODE_COMMAND_START:
      res = spa_v4l2_port_set_enabled (this, true);
      break;

    default:
      res = SPA_RESULT_NOT_IMPLEMENTED;
      break;
  }
  return res;
}

static SpaResult
spa_v4l2_source_node_process_input (SpaNode *node)
{
  return SPA_RESULT_INVALID_PORT;
}

static SpaResult
spa_v4l2_source_node_process_output (SpaNode *node)
{
  return SPA_RESULT_OK;
}

static const SpaNode v4l2source_node = {
  sizeof (SpaNode),
  NULL,
  SPA_NODE_STATE_INIT,
  spa_v4l2_source_node_get_props,
  spa_v4l2_source_node_set_props,
  spa_v4l2_source_node_send_command,
  spa_v4l2_source_node_set_event_callback,
  spa_v4l2_source_node_get_n_ports,
  spa_v4l2_source_node_get_port_ids,
  spa_v4l2_source_node_add_port,
  spa_v4l2_source_node_remove_port,
  spa_v4l2_source_node_port_enum_formats,
  spa_v4l2_source_node_port_set_format,
  spa_v4l2_source_node_port_get_format,
  spa_v4l2_source_node_port_get_info,
  spa_v4l2_source_node_port_get_props,
  spa_v4l2_source_node_port_set_props,
  spa_v4l2_source_node_port_use_buffers,
  spa_v4l2_source_node_port_alloc_buffers,
  spa_v4l2_source_node_port_set_input,
  spa_v4l2_source_node_port_set_output,
  spa_v4l2_source_node_port_reuse_buffer,
  spa_v4l2_source_node_port_send_command,
  spa_v4l2_source_node_process_input,
  spa_v4l2_source_node_process_output,
};

static SpaResult
spa_v4l2_source_clock_get_props (SpaClock  *clock,
                                 SpaProps **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_clock_set_props (SpaClock       *clock,
                                 const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_clock_get_time (SpaClock         *clock,
                                int32_t          *rate,
                                int64_t          *ticks,
                                int64_t          *monotonic_time)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;

  if (clock == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (clock, SpaV4l2Source, clock);
  state = &this->state[0];

  if (rate)
    *rate = SPA_USEC_PER_SEC;
  if (ticks)
    *ticks = state->last_ticks;
  if (monotonic_time)
    *monotonic_time = state->last_monotonic;

  return SPA_RESULT_OK;
}

static const SpaClock v4l2source_clock = {
  sizeof (SpaClock),
  NULL,
  SPA_CLOCK_STATE_STOPPED,
  spa_v4l2_source_clock_get_props,
  spa_v4l2_source_clock_set_props,
  spa_v4l2_source_clock_get_time,
};

static SpaResult
spa_v4l2_source_get_interface (SpaHandle               *handle,
                               uint32_t                 interface_id,
                               void                   **interface)
{
  SpaV4l2Source *this;

  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaV4l2Source *) handle;

  if (interface_id == this->uri.node)
    *interface = &this->node;
  else if (interface_id == this->uri.clock)
    *interface = &this->clock;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static SpaResult
v4l2_source_clear (SpaHandle *handle)
{
  return SPA_RESULT_OK;
}

static SpaResult
v4l2_source_init (const SpaHandleFactory  *factory,
                  SpaHandle               *handle,
                  const SpaDict           *info,
                  const SpaSupport        *support,
                  unsigned int             n_support)
{
  SpaV4l2Source *this;
  unsigned int i;
  const char *str;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_v4l2_source_get_interface;
  handle->clear = v4l2_source_clear,

  this = (SpaV4l2Source *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].uri, SPA_ID_MAP_URI) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].uri, SPA_LOG_URI) == 0)
      this->log = support[i].data;
    else if (strcmp (support[i].uri, SPA_LOOP__MainLoop) == 0)
      this->state[0].main_loop = support[i].data;
    else if (strcmp (support[i].uri, SPA_LOOP__DataLoop) == 0)
      this->state[0].data_loop = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "an id-map is needed");
    return SPA_RESULT_ERROR;
  }
  if (this->state[0].main_loop == NULL) {
    spa_log_error (this->log, "a main_loop is needed");
    return SPA_RESULT_ERROR;
  }
  if (this->state[0].data_loop == NULL) {
    spa_log_error (this->log, "a data_loop is needed");
    return SPA_RESULT_ERROR;
  }
  this->uri.node = spa_id_map_get_id (this->map, SPA_NODE_URI);
  this->uri.clock = spa_id_map_get_id (this->map, SPA_CLOCK_URI);

  this->node = v4l2source_node;
  this->clock = v4l2source_clock;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  reset_v4l2_source_props (&this->props[1]);

  this->state[0].log = this->log;
  this->state[0].info.flags = SPA_PORT_INFO_FLAG_LIVE;

  this->state[0].export_buf = true;

  if (info && (str = spa_dict_lookup (info, "device.path"))) {
    strncpy (this->props[1].device, str, 63);
    this->props[1].props.unset_mask &= ~1;
  }

  update_state (this, SPA_NODE_STATE_CONFIGURE);

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo v4l2_source_interfaces[] =
{
  { SPA_NODE_URI, },
  { SPA_CLOCK_URI, },
};

static SpaResult
v4l2_source_enum_interface_info (const SpaHandleFactory  *factory,
                                 const SpaInterfaceInfo **info,
                                 unsigned int             index)
{
  if (factory == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (index < 0 || index >= SPA_N_ELEMENTS (v4l2_source_interfaces))
    return SPA_RESULT_ENUM_END;

  *info = &v4l2_source_interfaces[index];
  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_v4l2_source_factory =
{ "v4l2-source",
  NULL,
  sizeof (SpaV4l2Source),
  v4l2_source_init,
  v4l2_source_enum_interface_info,
};
