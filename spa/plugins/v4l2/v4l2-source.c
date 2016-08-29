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
#include <spa/memory.h>
#include <spa/video/format.h>
#include <spa/debug.h>

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
}

#define MAX_BUFFERS     256

typedef struct _V4l2Buffer V4l2Buffer;

struct _V4l2Buffer {
  SpaBuffer buffer;
  SpaMeta metas[1];
  SpaMetaHeader header;
  SpaData datas[1];
  SpaBuffer *outbuf;
  bool outstanding;
  struct v4l2_buffer v4l2_buffer;
  V4l2Buffer *next;
  int dmafd;
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
  bool export_buf;
  bool have_buffers;

  bool next_fmtdesc;
  struct v4l2_fmtdesc fmtdesc;
  bool next_frmsize;
  struct v4l2_frmsizeenum frmsize;
  struct v4l2_frmivalenum frmival;
  void *cookie;

  V4l2Format format[2];
  V4l2Format *current_format;

  int fd;
  bool opened;
  struct v4l2_capability cap;
  struct v4l2_format fmt;
  enum v4l2_buf_type type;
  enum v4l2_memory memtype;

  struct v4l2_requestbuffers reqbuf;
  SpaMemory *alloc_mem;
  V4l2Buffer *alloc_buffers;
  V4l2Buffer *ready;
  uint32_t ready_count;

  SpaPollFd fds[1];
  SpaPollItem poll;

  SpaPortInfo info;
  SpaAllocParam *params[1];
  SpaAllocParamBuffers param_buffers;
  SpaPortStatus status;

} SpaV4l2State;

struct _SpaV4l2Source {
  SpaHandle handle;
  SpaNode node;
  SpaNodeState node_state;

  SpaV4l2SourceProps props[2];

  SpaEventCallback event_cb;
  void *user_data;

  SpaV4l2State state[1];
};

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
                                "device", "V4l2 device location",
                                SPA_PROP_FLAG_READWRITE,
                                SPA_PROP_TYPE_STRING, 63,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
  { PROP_ID_DEVICE_NAME,        offsetof (SpaV4l2SourceProps, device_name),
                                "device-name", "Human-readable name of the device",
                                SPA_PROP_FLAG_READABLE,
                                SPA_PROP_TYPE_STRING, 127,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
  { PROP_ID_DEVICE_FD,          offsetof (SpaV4l2SourceProps, device_fd),
                                "device-fd", "Device file descriptor",
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

  if (node == NULL || node->handle == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaV4l2Source *) node->handle;

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

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaV4l2Source *) node->handle;
  p = &this->props[1];

  if (props == NULL) {
    reset_v4l2_source_props (p);
    return SPA_RESULT_OK;
  }

  res = spa_props_copy (props, &p->props);

  return res;
}

static void
update_state (SpaV4l2Source *this, SpaNodeState state)
{
  SpaEvent event;
  SpaEventStateChange sc;

  if (this->node_state == state)
    return;

  this->node_state = state;

  event.type = SPA_EVENT_TYPE_STATE_CHANGE;
  event.data = &sc;
  event.size = sizeof (sc);
  sc.state = state;
  this->event_cb (&this->node, &event, this->user_data);
}

static SpaResult
spa_v4l2_source_node_send_command (SpaNode       *node,
                                   SpaCommand    *command)
{
  SpaV4l2Source *this;
  SpaResult res;

  if (node == NULL || node->handle == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaV4l2Source *) node->handle;

  switch (command->type) {
    case SPA_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_COMMAND_START:
    {
      SpaV4l2State *state = &this->state[0];

      if (state->current_format == NULL)
        return SPA_RESULT_NO_FORMAT;

      if (!state->have_buffers)
        return SPA_RESULT_NO_BUFFERS;

      if ((res = spa_v4l2_start (this)) < 0)
        return res;

      update_state (this, SPA_NODE_STATE_STREAMING);
      break;
    }
    case SPA_COMMAND_PAUSE:
    {
      SpaV4l2State *state = &this->state[0];

      if (state->current_format == NULL)
        return SPA_RESULT_NO_FORMAT;

      if (!state->have_buffers)
        return SPA_RESULT_NO_BUFFERS;

      if ((res = spa_v4l2_pause (this)) < 0)
        return res;

      update_state (this, SPA_NODE_STATE_PAUSED);
      break;
    }

    case SPA_COMMAND_FLUSH:
    case SPA_COMMAND_DRAIN:
    case SPA_COMMAND_MARKER:
      return SPA_RESULT_NOT_IMPLEMENTED;
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_set_event_callback (SpaNode       *node,
                                         SpaEventCallback event,
                                         void          *user_data)
{
  SpaV4l2Source *this;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaV4l2Source *) node->handle;

  this->event_cb = event;
  this->user_data = user_data;

  update_state (this, SPA_NODE_STATE_CONFIGURE);

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_get_n_ports (SpaNode       *node,
                                  unsigned int  *n_input_ports,
                                  unsigned int  *max_input_ports,
                                  unsigned int  *n_output_ports,
                                  unsigned int  *max_output_ports)
{
  if (node == NULL || node->handle == NULL)
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
  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_output_ports > 0)
    output_ids[0] = 0;

  return SPA_RESULT_OK;
}


static SpaResult
spa_v4l2_source_node_add_port (SpaNode        *node,
                               uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_remove_port (SpaNode        *node,
                                  uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static void
spa_v4l2_format_init (V4l2Format *f)
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
}

static SpaResult
spa_v4l2_source_node_port_enum_formats (SpaNode         *node,
                                        uint32_t         port_id,
                                        SpaFormat      **format,
                                        const SpaFormat *filter,
                                        void           **state)
{
  SpaV4l2Source *this;
  SpaResult res;

  if (node == NULL || node->handle == NULL || format == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaV4l2Source *) node->handle;

  fprintf (stderr, "%d\n", port_id);

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  res = spa_v4l2_enum_format (this, format, filter, state);

  return res;
}

static SpaResult
spa_v4l2_source_node_port_set_format (SpaNode            *node,
                                      uint32_t            port_id,
                                      SpaPortFormatFlags  flags,
                                      const SpaFormat    *format)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;
  SpaResult res;
  V4l2Format *f, *tf;
  size_t fs;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaV4l2Source *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (format == NULL) {
    spa_v4l2_clear_buffers (this);
    spa_v4l2_close (this);
    state->current_format = NULL;
    update_state (this, SPA_NODE_STATE_CONFIGURE);
    return SPA_RESULT_OK;
  }

  f = &state->format[0];
  tf = &state->format[1];
  fs = sizeof (V4l2Format);

  if ((SpaFormat*)f != format) {
    spa_v4l2_format_init (f);
    f->fmt.media_type = format->media_type;
    f->fmt.media_subtype = format->media_subtype;
    if ((res = spa_props_copy (&format->props, &f->fmt.props) < 0))
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
    memcpy (tf, f, fs);
    state->current_format = tf;

    update_state (this, SPA_NODE_STATE_READY);
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_get_format (SpaNode          *node,
                                      uint32_t          port_id,
                                      const SpaFormat **format)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;

  if (node == NULL || node->handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaV4l2Source *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (state->current_format == NULL)
    return SPA_RESULT_NO_FORMAT;

  *format = &state->current_format->fmt;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_get_info (SpaNode            *node,
                                    uint32_t            port_id,
                                    const SpaPortInfo **info)
{
  SpaV4l2Source *this;

  if (node == NULL || node->handle == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaV4l2Source *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  *info = &this->state[port_id].info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_get_props (SpaNode    *node,
                                     uint32_t    port_id,
                                     SpaProps  **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_port_set_props (SpaNode         *node,
                                     uint32_t         port_id,
                                     const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_port_use_buffers (SpaNode         *node,
                                       uint32_t         port_id,
                                       SpaBuffer      **buffers,
                                       uint32_t         n_buffers)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;
  SpaResult res;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaV4l2Source *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (state->current_format == NULL)
    return SPA_RESULT_NO_FORMAT;

  if (state->have_buffers) {
    if ((res = spa_v4l2_clear_buffers (this)) < 0)
      return res;
  }
  if (buffers != NULL) {
    if ((res = spa_v4l2_use_buffers (this, buffers, n_buffers)) < 0)
      return res;
  }

  if (state->have_buffers)
    update_state (this, SPA_NODE_STATE_PAUSED);
  else
    update_state (this, SPA_NODE_STATE_READY);

  return res;
}

static SpaResult
spa_v4l2_source_node_port_alloc_buffers (SpaNode         *node,
                                         uint32_t         port_id,
                                         SpaAllocParam  **params,
                                         unsigned int     n_params,
                                         SpaBuffer      **buffers,
                                         unsigned int    *n_buffers)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;
  SpaResult res;

  if (node == NULL || node->handle == NULL || buffers == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaV4l2Source *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (state->current_format == NULL)
    return SPA_RESULT_NO_FORMAT;

  res = spa_v4l2_alloc_buffers (this, params, n_params, buffers, n_buffers);

  if (state->have_buffers)
    update_state (this, SPA_NODE_STATE_PAUSED);

  return res;
}

static SpaResult
spa_v4l2_source_node_port_reuse_buffer (SpaNode         *node,
                                        uint32_t         port_id,
                                        uint32_t         buffer_id)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;
  SpaResult res;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaV4l2Source *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (!state->have_buffers)
    return SPA_RESULT_NO_BUFFERS;

  if (buffer_id >= state->reqbuf.count)
    return SPA_RESULT_INVALID_BUFFER_ID;

  res = spa_v4l2_buffer_recycle (this, buffer_id);

  return res;
}

static SpaResult
spa_v4l2_source_node_port_get_status (SpaNode              *node,
                                      uint32_t              port_id,
                                      const SpaPortStatus **status)
{
  SpaV4l2Source *this;

  if (node == NULL || node->handle == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaV4l2Source *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  *status = &this->state[port_id].status;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_push_input (SpaNode        *node,
                                      unsigned int    n_info,
                                      SpaInputInfo   *info)
{
  return SPA_RESULT_INVALID_PORT;
}

static SpaResult
spa_v4l2_source_node_port_pull_output (SpaNode        *node,
                                       unsigned int    n_info,
                                       SpaOutputInfo  *info)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;
  unsigned int i;
  bool have_error = false;

  if (node == NULL || node->handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaV4l2Source *) node->handle;

  for (i = 0; i < n_info; i++) {
    V4l2Buffer *b;

    if (info[i].port_id != 0) {
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }
    state = &this->state[info[i].port_id];

    if (state->current_format == NULL) {
      info[i].status = SPA_RESULT_NO_FORMAT;
      have_error = true;
      continue;
    }
    if (state->ready_count == 0) {
      info[i].status = SPA_RESULT_UNEXPECTED;
      have_error = true;
      continue;
    }

    b = state->ready;
    state->ready = b->next;
    state->ready_count--;

    b->outstanding = true;

    info[i].buffer_id = b->outbuf->id;
    info[i].status = SPA_RESULT_OK;
  }
  if (have_error)
    return SPA_RESULT_ERROR;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_push_event (SpaNode    *node,
                                      uint32_t    port_id,
                                      SpaEvent   *event)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}


static const SpaNode v4l2source_node = {
  NULL,
  sizeof (SpaNode),
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
  spa_v4l2_source_node_port_reuse_buffer,
  spa_v4l2_source_node_port_get_status,
  spa_v4l2_source_node_port_push_input,
  spa_v4l2_source_node_port_pull_output,
  spa_v4l2_source_node_port_push_event,
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

  switch (interface_id) {
    case SPA_INTERFACE_ID_NODE:
      *interface = &this->node;
      break;
    default:
      return SPA_RESULT_UNKNOWN_INTERFACE;
  }
  return SPA_RESULT_OK;
}

static SpaResult
v4l2_source_init (const SpaHandleFactory  *factory,
                  SpaHandle               *handle)
{
  SpaV4l2Source *this;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_v4l2_source_get_interface;

  this = (SpaV4l2Source *) handle;
  this->node = v4l2source_node;
  this->node.handle = handle;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  reset_v4l2_source_props (&this->props[1]);

  this->state[0].info.flags = SPA_PORT_INFO_FLAG_NONE;
  this->state[0].status.flags = SPA_PORT_STATUS_FLAG_NONE;

  this->state[0].export_buf = true;

  this->node_state = SPA_NODE_STATE_INIT;

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo v4l2_source_interfaces[] =
{
  { SPA_INTERFACE_ID_NODE,
    SPA_INTERFACE_ID_NODE_NAME,
    SPA_INTERFACE_ID_NODE_DESCRIPTION,
  },
};

static SpaResult
v4l2_source_enum_interface_info (const SpaHandleFactory  *factory,
                                 const SpaInterfaceInfo **info,
                                 void                   **state)
{
  int index;

  if (factory == NULL || info == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      *info = &v4l2_source_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *(int*)state = ++index;
  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_v4l2_source_factory =
{ "v4l2-source",
  NULL,
  sizeof (SpaV4l2Source),
  v4l2_source_init,
  v4l2_source_enum_interface_info,
};
