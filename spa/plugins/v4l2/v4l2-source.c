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
  SpaV4l2Source *source;
  SpaBuffer *imported;
  bool outstanding;
  struct v4l2_buffer v4l2_buffer;
  V4l2Buffer *next;
};

typedef struct {
  SpaVideoRawFormat raw_format[2];
  SpaFormat *current_format;
  bool opened;
  bool have_buffers;
  int fd;
  struct v4l2_capability cap;
  struct v4l2_format fmt;
  enum v4l2_buf_type type;
  enum v4l2_memory memtype;
  struct v4l2_requestbuffers reqbuf;
  V4l2Buffer buffers[MAX_BUFFERS];
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
  { PROP_ID_DEVICE,            "device", "V4l2 device location",
                                SPA_PROP_FLAG_READWRITE,
                                SPA_PROP_TYPE_STRING, 63,
                                strlen (default_device)+1, default_device,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL,
                                offsetof (SpaV4l2SourceProps, device),
                                0, 0,
                                NULL },
  { PROP_ID_DEVICE_NAME,       "device-name", "Human-readable name of the device",
                                SPA_PROP_FLAG_READABLE,
                                SPA_PROP_TYPE_STRING, 127,
                                0, NULL,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL,
                                offsetof (SpaV4l2SourceProps, device_name),
                                0, 0,
                                NULL },
  { PROP_ID_DEVICE_FD,          "device-fd", "Device file descriptor",
                                SPA_PROP_FLAG_READABLE,
                                SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                0, NULL,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL,
                                offsetof (SpaV4l2SourceProps, device_fd),
                                0, 0,
                                NULL },
};

static SpaResult
spa_v4l2_source_node_get_props (SpaHandle     *handle,
                                SpaProps     **props)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;

  if (handle == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));
  *props = &this->props[0].props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_set_props (SpaHandle       *handle,
                                const SpaProps  *props)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;
  SpaV4l2SourceProps *p = &this->props[1];
  SpaResult res;

  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (props == NULL) {
    reset_v4l2_source_props (p);
    return SPA_RESULT_OK;
  }

  res = spa_props_copy (props, &p->props);

  return res;
}

static SpaResult
spa_v4l2_source_node_send_command (SpaHandle     *handle,
                                   SpaCommand    *command)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;

  if (handle == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  switch (command->type) {
    case SPA_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_COMMAND_START:
      spa_v4l2_start (this);

      if (this->event_cb) {
        SpaEvent event;

        event.refcount = 1;
        event.notify = NULL;
        event.type = SPA_EVENT_TYPE_STARTED;
        event.port_id = -1;
        event.data = NULL;
        event.size = 0;

        this->event_cb (handle, &event, this->user_data);
      }
      break;
    case SPA_COMMAND_STOP:
      spa_v4l2_stop (this);

      if (this->event_cb) {
        SpaEvent event;

        event.refcount = 1;
        event.notify = NULL;
        event.type = SPA_EVENT_TYPE_STOPPED;
        event.port_id = -1;
        event.data = NULL;
        event.size = 0;

        this->event_cb (handle, &event, this->user_data);
      }
      break;

    case SPA_COMMAND_FLUSH:
    case SPA_COMMAND_DRAIN:
    case SPA_COMMAND_MARKER:
      return SPA_RESULT_NOT_IMPLEMENTED;
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_set_event_callback (SpaHandle     *handle,
                                         SpaEventCallback event,
                                         void          *user_data)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;

  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_get_n_ports (SpaHandle     *handle,
                                  unsigned int  *n_input_ports,
                                  unsigned int  *max_input_ports,
                                  unsigned int  *n_output_ports,
                                  unsigned int  *max_output_ports)
{
  if (handle == NULL)
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
spa_v4l2_source_node_get_port_ids (SpaHandle     *handle,
                                   unsigned int   n_input_ports,
                                   uint32_t      *input_ids,
                                   unsigned int   n_output_ports,
                                   uint32_t      *output_ids)
{
  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_output_ports > 0)
    output_ids[0] = 0;

  return SPA_RESULT_OK;
}


static SpaResult
spa_v4l2_source_node_add_port (SpaHandle      *handle,
                               SpaDirection    direction,
                               uint32_t       *port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_remove_port (SpaHandle      *handle,
                                  uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_port_enum_formats (SpaHandle       *handle,
                                        uint32_t         port_id,
                                        unsigned int     index,
                                        SpaFormat      **format)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;
  SpaV4l2State *state;

  if (handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  switch (index) {
    case 0:
      spa_video_raw_format_init (&state->raw_format[0]);
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = &state->raw_format[0].format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_set_format (SpaHandle       *handle,
                                      uint32_t         port_id,
                                      bool             test_only,
                                      const SpaFormat *format)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;
  SpaV4l2State *state;
  SpaResult res;
  SpaFormat *f, *tf;
  size_t fs;

  if (handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (format == NULL) {
    state->current_format = NULL;
    return SPA_RESULT_OK;
  }

  if (format->media_type == SPA_MEDIA_TYPE_VIDEO) {
    if (format->media_subtype == SPA_MEDIA_SUBTYPE_RAW) {
      if ((res = spa_video_raw_format_parse (format, &state->raw_format[0]) < 0))
        return res;

      f = &state->raw_format[0].format;
      tf = &state->raw_format[1].format;
      fs = sizeof (SpaVideoRawFormat);
    } else
      return SPA_RESULT_INVALID_MEDIA_TYPE;
  } else
    return SPA_RESULT_INVALID_MEDIA_TYPE;

  if (spa_v4l2_set_format (this, f, test_only) < 0)
    return SPA_RESULT_INVALID_MEDIA_TYPE;

  if (!test_only) {
    memcpy (tf, f, fs);
    state->current_format = tf;
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_get_format (SpaHandle        *handle,
                                      uint32_t          port_id,
                                      const SpaFormat **format)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;
  SpaV4l2State *state;

  if (handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (state->current_format == NULL)
    return SPA_RESULT_NO_FORMAT;

  *format = state->current_format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_get_info (SpaHandle          *handle,
                                    uint32_t            port_id,
                                    const SpaPortInfo **info)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;

  if (handle == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  *info = &this->state[port_id].info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_get_props (SpaHandle  *handle,
                                     uint32_t    port_id,
                                     SpaProps  **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_port_set_props (SpaHandle       *handle,
                                     uint32_t         port_id,
                                     const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_port_get_status (SpaHandle            *handle,
                                      uint32_t              port_id,
                                      const SpaPortStatus **status)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;

  if (handle == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  *status = &this->state[port_id].status;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_use_buffers (SpaHandle       *handle,
                                       uint32_t         port_id,
                                       SpaBuffer      **buffers,
                                       uint32_t         n_buffers)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;

  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  spa_v4l2_import_buffers (this, buffers, n_buffers);

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_alloc_buffers (SpaHandle       *handle,
                                         uint32_t         port_id,
                                         SpaAllocParam  **params,
                                         uint32_t         n_params,
                                         SpaBuffer      **buffers,
                                         uint32_t        *n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}


static SpaResult
spa_v4l2_source_node_port_push_input (SpaHandle      *handle,
                                      unsigned int    n_info,
                                      SpaInputInfo   *info)
{
  return SPA_RESULT_INVALID_PORT;
}

static SpaResult
spa_v4l2_source_node_port_pull_output (SpaHandle      *handle,
                                       unsigned int    n_info,
                                       SpaOutputInfo  *info)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;
  SpaV4l2State *state;
  unsigned int i;
  bool have_error = false;

  if (handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;


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

    info[i].buffer = &b->buffer;
    info[i].status = SPA_RESULT_OK;
  }
  if (have_error)
    return SPA_RESULT_ERROR;

  return SPA_RESULT_OK;
}

static const SpaNode v4l2source_node = {
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
  spa_v4l2_source_node_port_get_status,
  spa_v4l2_source_node_port_push_input,
  spa_v4l2_source_node_port_pull_output,
};

static SpaResult
spa_v4l2_source_get_interface (SpaHandle               *handle,
                               uint32_t                 interface_id,
                               const void             **interface)
{
  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  switch (interface_id) {
    case SPA_INTERFACE_ID_NODE:
      *interface = &v4l2source_node;
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
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  this->props[1].props.set_prop = spa_props_generic_set_prop;
  this->props[1].props.get_prop = spa_props_generic_get_prop;
  reset_v4l2_source_props (&this->props[1]);

  this->state[0].info.flags = SPA_PORT_INFO_FLAG_NONE;
  this->state[0].status.flags = SPA_PORT_STATUS_FLAG_NONE;

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
                               unsigned int             index,
                               const SpaInterfaceInfo **info)
{
  if (index >= 1)
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
