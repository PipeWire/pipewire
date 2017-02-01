/* Spa Xv Sink
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

#include <spa/id-map.h>
#include <spa/log.h>
#include <spa/node.h>
#include <spa/video/format.h>
#include <lib/props.h>

typedef struct _SpaXvSink SpaXvSink;

static const char default_device[] = "/dev/video0";

typedef struct {
  SpaProps props;
  char device[64];
  char device_name[128];
  int  device_fd;
} SpaXvSinkProps;

static void
reset_xv_sink_props (SpaXvSinkProps *props)
{
  strncpy (props->device, default_device, 64);
}

#define MAX_BUFFERS     256

typedef struct _XvBuffer XvBuffer;

struct _XvBuffer {
  SpaBuffer buffer;
  SpaMeta meta[1];
  SpaMetaHeader header;
  SpaData data[1];
  XvBuffer *next;
  uint32_t index;
  SpaXvSink *sink;
  bool outstanding;
};

typedef struct {
  bool opened;
  int fd;
  XvBuffer buffers[MAX_BUFFERS];
  XvBuffer *ready;
  uint32_t ready_count;
} SpaXvState;

typedef struct {
  uint32_t node;
} URI;

struct _SpaXvSink {
  SpaHandle handle;
  SpaNode   node;

  URI uri;
  SpaIDMap *map;
  SpaLog *log;

  SpaXvSinkProps props[2];

  SpaNodeEventCallback event_cb;
  void *user_data;

  SpaFormatVideo format[2];
  SpaFormat *current_format;

  SpaPortInfo info;
  SpaXvState state;

  SpaPortInput *input;
};

#define CHECK_PORT(this,d,p)  ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)

#include "xv-utils.c"

enum {
  PROP_ID_DEVICE,
  PROP_ID_DEVICE_NAME,
  PROP_ID_DEVICE_FD,
  PROP_ID_LAST,
};

static const SpaPropInfo prop_info[] =
{
  { PROP_ID_DEVICE,             offsetof (SpaXvSinkProps, device),
                                "device",
                                SPA_PROP_FLAG_READWRITE,
                                SPA_PROP_TYPE_STRING, 63,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
  { PROP_ID_DEVICE_NAME,        offsetof (SpaXvSinkProps, device_name),
                                "device-name",
                                SPA_PROP_FLAG_READABLE,
                                SPA_PROP_TYPE_STRING, 127,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
  { PROP_ID_DEVICE_FD,          offsetof (SpaXvSinkProps, device_fd),
                                "device-fd",
                                SPA_PROP_FLAG_READABLE,
                                SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
};

static void
update_state (SpaXvSink *this, SpaNodeState state)
{
  this->node.state = state;
}

static SpaResult
spa_xv_sink_node_get_props (SpaNode       *node,
                            SpaProps     **props)
{
  SpaXvSink *this;

  if (node == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));
  *props = &this->props[0].props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_set_props (SpaNode         *node,
                            const SpaProps  *props)
{
  SpaXvSink *this;
  SpaXvSinkProps *p;
  SpaResult res;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);
  p = &this->props[1];

  if (props == NULL) {
    reset_xv_sink_props (p);
    return SPA_RESULT_OK;
  }

  res = spa_props_copy_values (props, &p->props);

  return res;
}

static SpaResult
spa_xv_sink_node_send_command (SpaNode        *node,
                               SpaNodeCommand *command)
{
  SpaXvSink *this;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  switch (command->type) {
    case SPA_NODE_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_NODE_COMMAND_START:
      spa_xv_start (this);

      update_state (this, SPA_NODE_STATE_STREAMING);
      break;
    case SPA_NODE_COMMAND_PAUSE:
      spa_xv_stop (this);

      update_state (this, SPA_NODE_STATE_PAUSED);
      break;

    case SPA_NODE_COMMAND_FLUSH:
    case SPA_NODE_COMMAND_DRAIN:
    case SPA_NODE_COMMAND_MARKER:
    case SPA_NODE_COMMAND_CLOCK_UPDATE:
      return SPA_RESULT_NOT_IMPLEMENTED;
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_set_event_callback (SpaNode              *node,
                                     SpaNodeEventCallback  event,
                                     void                 *user_data)
{
  SpaXvSink *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_get_n_ports (SpaNode       *node,
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
spa_xv_sink_node_get_port_ids (SpaNode       *node,
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
spa_xv_sink_node_add_port (SpaNode        *node,
                           SpaDirection    direction,
                           uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_remove_port (SpaNode        *node,
                              SpaDirection    direction,
                              uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_port_enum_formats (SpaNode         *node,
                                    SpaDirection     direction,
                                    uint32_t         port_id,
                                    SpaFormat      **format,
                                    const SpaFormat *filter,
                                    unsigned int     index)
{
  SpaXvSink *this;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  switch (index) {
    case 0:
      spa_format_video_init (SPA_MEDIA_TYPE_VIDEO,
                             SPA_MEDIA_SUBTYPE_RAW,
                             &this->format[0]);
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = &this->format[0].format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_port_set_format (SpaNode            *node,
                                  SpaDirection        direction,
                                  uint32_t            port_id,
                                  SpaPortFormatFlags  flags,
                                  const SpaFormat    *format)
{
  SpaXvSink *this;
  SpaResult res;
  SpaFormat *f, *tf;
  size_t fs;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (format == NULL) {
    this->current_format = NULL;
    return SPA_RESULT_OK;
  }

  if (format->media_type == SPA_MEDIA_TYPE_VIDEO) {
    if (format->media_subtype == SPA_MEDIA_SUBTYPE_RAW) {
      if ((res = spa_format_video_parse (format, &this->format[0]) < 0))
        return res;

      f = &this->format[0].format;
      tf = &this->format[1].format;
      fs = sizeof (SpaVideoFormat);
    } else
      return SPA_RESULT_INVALID_MEDIA_TYPE;
  } else
    return SPA_RESULT_INVALID_MEDIA_TYPE;

  if (spa_xv_set_format (this, f, flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY) < 0)
    return SPA_RESULT_INVALID_MEDIA_TYPE;

  if (!(flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY)) {
    memcpy (tf, f, fs);
    this->current_format = tf;
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_port_get_format (SpaNode          *node,
                                  SpaDirection      direction,
                                  uint32_t          port_id,
                                  const SpaFormat **format)
{
  SpaXvSink *this;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (this->current_format == NULL)
    return SPA_RESULT_NO_FORMAT;

  *format = this->current_format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_port_get_info (SpaNode            *node,
                                SpaDirection        direction,
                                uint32_t            port_id,
                                const SpaPortInfo **info)
{
  SpaXvSink *this;

  if (node == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  *info = &this->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_port_get_props (SpaNode       *node,
                                 SpaDirection   direction,
                                 uint32_t       port_id,
                                 SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_port_set_props (SpaNode         *node,
                                 SpaDirection     direction,
                                 uint32_t         port_id,
                                 const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_port_use_buffers (SpaNode         *node,
                                   SpaDirection     direction,
                                   uint32_t         port_id,
                                   SpaBuffer      **buffers,
                                   uint32_t         n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_port_alloc_buffers (SpaNode         *node,
                                     SpaDirection     direction,
                                     uint32_t         port_id,
                                     SpaAllocParam  **params,
                                     uint32_t         n_params,
                                     SpaBuffer      **buffers,
                                     uint32_t        *n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_port_set_input (SpaNode      *node,
                                 uint32_t      port_id,
                                 SpaPortInput *input)
{
  SpaXvSink *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  if (!CHECK_PORT (this, SPA_DIRECTION_INPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->input = input;

  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_port_set_output (SpaNode       *node,
                                  uint32_t       port_id,
                                  SpaPortOutput *output)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_port_reuse_buffer (SpaNode         *node,
                                    uint32_t         port_id,
                                    uint32_t         buffer_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_port_send_command (SpaNode        *node,
                                    SpaDirection    direction,
                                    uint32_t        port_id,
                                    SpaNodeCommand *command)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_process_input (SpaNode          *node)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_process_output (SpaNode           *node)
{
  return SPA_RESULT_INVALID_PORT;
}

static const SpaNode xvsink_node = {
  sizeof (SpaNode),
  NULL,
  SPA_NODE_STATE_INIT,
  spa_xv_sink_node_get_props,
  spa_xv_sink_node_set_props,
  spa_xv_sink_node_send_command,
  spa_xv_sink_node_set_event_callback,
  spa_xv_sink_node_get_n_ports,
  spa_xv_sink_node_get_port_ids,
  spa_xv_sink_node_add_port,
  spa_xv_sink_node_remove_port,
  spa_xv_sink_node_port_enum_formats,
  spa_xv_sink_node_port_set_format,
  spa_xv_sink_node_port_get_format,
  spa_xv_sink_node_port_get_info,
  spa_xv_sink_node_port_get_props,
  spa_xv_sink_node_port_set_props,
  spa_xv_sink_node_port_use_buffers,
  spa_xv_sink_node_port_alloc_buffers,
  spa_xv_sink_node_port_set_input,
  spa_xv_sink_node_port_set_output,
  spa_xv_sink_node_port_reuse_buffer,
  spa_xv_sink_node_port_send_command,
  spa_xv_sink_node_process_input,
  spa_xv_sink_node_process_output,
};

static SpaResult
spa_xv_sink_get_interface (SpaHandle               *handle,
                           uint32_t                 interface_id,
                           void                   **interface)
{
  SpaXvSink *this;

  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaXvSink *) handle;

  if (interface_id == this->uri.node)
    *interface = &this->node;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static SpaResult
xv_sink_clear (SpaHandle *handle)
{
  return SPA_RESULT_OK;
}

static SpaResult
xv_sink_init (const SpaHandleFactory  *factory,
              SpaHandle               *handle,
              const SpaDict           *info,
              const SpaSupport        *support,
              unsigned int             n_support)
{
  SpaXvSink *this;
  unsigned int i;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_xv_sink_get_interface;
  handle->clear = xv_sink_clear;

  this = (SpaXvSink *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].uri, SPA_ID_MAP_URI) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].uri, SPA_LOG_URI) == 0)
      this->log = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "an id-map is needed");
    return SPA_RESULT_ERROR;
  }
  this->uri.node = spa_id_map_get_id (this->map, SPA_NODE_URI);

  this->node = xvsink_node;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  reset_xv_sink_props (&this->props[1]);

  this->info.flags = SPA_PORT_INFO_FLAG_NONE;

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo xv_sink_interfaces[] =
{
  { SPA_NODE_URI, },
};

static SpaResult
xv_sink_enum_interface_info (const SpaHandleFactory  *factory,
                             const SpaInterfaceInfo **info,
                             unsigned int             index)
{
  if (factory == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  switch (index) {
    case 0:
      *info = &xv_sink_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_xv_sink_factory =
{ "xv-sink",
  NULL,
  sizeof (SpaXvSink),
  xv_sink_init,
  xv_sink_enum_interface_info,
};
