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

#include <libv4l2.h>
#include <pthread.h>

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

typedef struct {
  int fd;
  pthread_t thread;
  bool running;
} SpaV4l2State;

typedef struct _V4l2Buffer V4l2Buffer;

struct _V4l2Buffer {
  SpaBuffer buffer;
  SpaMeta meta[1];
  SpaMetaHeader header;
  SpaData data[1];
  V4l2Buffer *next;
};

struct _SpaV4l2Source {
  SpaHandle handle;

  SpaV4l2SourceProps tmp_props;
  SpaV4l2SourceProps props;

  bool activated;

  SpaEventCallback event_cb;
  void *user_data;

  bool have_format;
  SpaVideoRawFormat query_format;
  SpaVideoRawFormat current_format;

  SpaV4l2State state;

  SpaPortInfo info;
  SpaPortStatus status;

  V4l2Buffer buffer;
};

#include "v4l2-utils.c"

static const uint32_t min_uint32 = 1;
static const uint32_t max_uint32 = UINT32_MAX;

static const SpaPropRangeInfo uint32_range[] = {
  { "min", "Minimum value", 4, &min_uint32 },
  { "max", "Maximum value", 4, &max_uint32 },
};

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

  memcpy (&this->tmp_props, &this->props, sizeof (this->props));
  *props = &this->tmp_props.props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_set_props (SpaHandle       *handle,
                                const SpaProps  *props)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;
  SpaV4l2SourceProps *p = &this->props;
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

    case SPA_COMMAND_ACTIVATE:
      if (!this->activated) {
        spa_v4l2_open (this);
        this->activated = true;
      }
      if (this->event_cb) {
        SpaEvent event;

        event.refcount = 1;
        event.notify = NULL;
        event.type = SPA_EVENT_TYPE_ACTIVATED;
        event.port_id = -1;
        event.data = NULL;
        event.size = 0;

        this->event_cb (handle, &event, this->user_data);
      }
      break;
    case SPA_COMMAND_DEACTIVATE:
      if (this->activated) {
        spa_v4l2_close (this);
        this->activated = false;
      }
      if (this->event_cb) {
        SpaEvent event;

        event.refcount = 1;
        event.notify = NULL;
        event.type = SPA_EVENT_TYPE_DEACTIVATED;
        event.port_id = -1;
        event.data = NULL;
        event.size = 0;

        this->event_cb (handle, &event, this->user_data);
      }
      break;
    case SPA_COMMAND_START:
      spa_v4l2_start (this);
      break;
    case SPA_COMMAND_STOP:
      spa_v4l2_stop (this);
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
spa_v4l2_source_node_enum_port_formats (SpaHandle       *handle,
                                        uint32_t         port_id,
                                        unsigned int     index,
                                        SpaFormat      **format)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;

  if (handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  switch (index) {
    case 0:
      spa_video_raw_format_init (&this->query_format);
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = &this->query_format.format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_set_port_format (SpaHandle       *handle,
                                      uint32_t         port_id,
                                      int              test_only,
                                      const SpaFormat *format)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;
  SpaResult res;

  if (handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  if (format == NULL) {
    this->have_format = false;
    return SPA_RESULT_OK;
  }

  if ((res = spa_video_raw_format_parse (format, &this->current_format)) < 0)
    return res;

  this->have_format = true;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_get_port_format (SpaHandle        *handle,
                                      uint32_t          port_id,
                                      const SpaFormat **format)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;

  if (handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = &this->current_format.format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_get_port_info (SpaHandle          *handle,
                                    uint32_t            port_id,
                                    const SpaPortInfo **info)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;

  if (handle == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  *info = &this->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_get_port_props (SpaHandle  *handle,
                                     uint32_t    port_id,
                                     SpaProps  **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_set_port_props (SpaHandle       *handle,
                                     uint32_t         port_id,
                                     const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_get_port_status (SpaHandle            *handle,
                                      uint32_t              port_id,
                                      const SpaPortStatus **status)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;

  if (handle == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  *status = &this->status;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_push_port_input (SpaHandle      *handle,
                                      unsigned int    n_info,
                                      SpaInputInfo   *info)
{
  return SPA_RESULT_INVALID_PORT;
}

static SpaResult
spa_v4l2_source_node_pull_port_output (SpaHandle      *handle,
                                       unsigned int    n_info,
                                       SpaOutputInfo  *info)
{
  SpaV4l2Source *this = (SpaV4l2Source *) handle;
  unsigned int i;
  bool have_error = false;

  if (handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  for (i = 0; i < n_info; i++) {
    if (info[i].port_id != 0) {
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }
    if (!this->have_format) {
      info[i].status = SPA_RESULT_NO_FORMAT;
      have_error = true;
      continue;
    }
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
  spa_v4l2_source_node_enum_port_formats,
  spa_v4l2_source_node_set_port_format,
  spa_v4l2_source_node_get_port_format,
  spa_v4l2_source_node_get_port_info,
  spa_v4l2_source_node_get_port_props,
  spa_v4l2_source_node_set_port_props,
  spa_v4l2_source_node_get_port_status,
  spa_v4l2_source_node_push_port_input,
  spa_v4l2_source_node_pull_port_output,
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

SpaHandle *
spa_v4l2_source_new (void)
{
  SpaHandle *handle;
  SpaV4l2Source *this;

  handle = calloc (1, sizeof (SpaV4l2Source));
  handle->get_interface = spa_v4l2_source_get_interface;

  this = (SpaV4l2Source *) handle;
  this->props.props.n_prop_info = PROP_ID_LAST;
  this->props.props.prop_info = prop_info;
  this->props.props.set_prop = spa_props_generic_set_prop;
  this->props.props.get_prop = spa_props_generic_get_prop;
  reset_v4l2_source_props (&this->props);

  this->info.flags = SPA_PORT_INFO_FLAG_NONE;
  this->status.flags = SPA_PORT_STATUS_FLAG_NONE;
  return handle;
}
