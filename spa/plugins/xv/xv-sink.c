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

#include <spa/type-map.h>
#include <spa/log.h>
#include <spa/list.h>
#include <spa/node.h>
#include <spa/video/format-utils.h>
#include <lib/props.h>

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

#define MAX_BUFFERS     32

struct buffer {
  struct spa_buffer buffer;
  struct spa_meta meta[1];
  struct spa_meta_header header;
  struct spa_data data[1];
  struct spa_list link;
  bool outstanding;
};

struct port {
  bool opened;
  int fd;

  struct spa_port_io *io;

  bool have_format;
  uint8_t format_buffer[1024];
  struct spa_video_info current_format;
  struct spa_port_info info;

  struct buffer buffers[MAX_BUFFERS];
  struct spa_list ready;
};

struct type {
  uint32_t node;
  uint32_t props;
  uint32_t prop_device;
  uint32_t prop_device_name;
  uint32_t prop_device_fd;
  struct spa_type_media_type media_type;
  struct spa_type_media_subtype media_subtype;
  struct spa_type_format_video format_video;
  struct spa_type_command_node command_node;
};

static inline void
init_type (struct type *type, struct spa_type_map *map)
{
  type->node = spa_type_map_get_id (map, SPA_TYPE__Node);
  type->props = spa_type_map_get_id (map, SPA_TYPE__Props);
  type->prop_device = spa_type_map_get_id (map, SPA_TYPE_PROPS__device);
  type->prop_device_name = spa_type_map_get_id (map, SPA_TYPE_PROPS__deviceName);
  type->prop_device_fd = spa_type_map_get_id (map, SPA_TYPE_PROPS__deviceFd);
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_format_video_map (map, &type->format_video);
  spa_type_command_node_map (map, &type->command_node);
}

struct impl {
  struct spa_handle handle;
  struct spa_node   node;

  struct type type;
  struct spa_type_map *map;
  struct spa_log *log;

  uint8_t props_buffer[512];
  struct props props;

  struct spa_node_callbacks callbacks;
  void *user_data;

  struct port in_ports[1];
};

#define CHECK_PORT(this,d,p)  ((d) == SPA_DIRECTION_INPUT && (p) == 0)

#include "xv-utils.c"

#define PROP(f,key,type,...)                                                    \
          SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
#define PROP_R(f,key,type,...)                                                  \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_READONLY,type,1,__VA_ARGS__)

static int
spa_xv_sink_node_get_props (struct spa_node *node,
                            struct spa_props **props)
{
  struct impl *this;
  struct spa_pod_builder b = { NULL,  };
  struct spa_pod_frame f[2];

  if (node == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

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
spa_xv_sink_node_set_props (struct spa_node *node,
                            const struct spa_props *props)
{
  struct impl *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, struct impl, node);

  if (props == NULL) {
    reset_props (&this->props);
  } else {
    spa_props_query (props,
        this->type.prop_device, -SPA_POD_TYPE_STRING, this->props.device, sizeof (this->props.device),
        0);
  }
  return SPA_RESULT_OK;
}

static int
spa_xv_sink_node_send_command (struct spa_node    *node,
                               struct spa_command *command)
{
  struct impl *this;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, struct impl, node);

  if (SPA_COMMAND_TYPE (command) == this->type.command_node.Start) {
    spa_xv_start (this);
  }
  else if (SPA_COMMAND_TYPE (command) == this->type.command_node.Pause) {
    spa_xv_stop (this);
  }
  else
    return SPA_RESULT_NOT_IMPLEMENTED;

  return SPA_RESULT_OK;
}

static int
spa_xv_sink_node_set_callbacks (struct spa_node                *node,
                                const struct spa_node_callbacks *callbacks,
                                size_t                  callbacks_size,
                                void                   *user_data)
{
  struct impl *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, struct impl, node);

  this->callbacks = *callbacks;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static int
spa_xv_sink_node_get_n_ports (struct spa_node       *node,
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

static int
spa_xv_sink_node_get_port_ids (struct spa_node       *node,
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


static int
spa_xv_sink_node_add_port (struct spa_node        *node,
                           enum spa_direction    direction,
                           uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_xv_sink_node_remove_port (struct spa_node        *node,
                              enum spa_direction    direction,
                              uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_xv_sink_node_port_enum_formats (struct spa_node         *node,
                                    enum spa_direction     direction,
                                    uint32_t         port_id,
                                    struct spa_format      **format,
                                    const struct spa_format *filter,
                                    uint32_t         index)
{
  //struct impl *this;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  //this = SPA_CONTAINER_OF (node, struct impl, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  switch (index) {
    case 0:
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = NULL;

  return SPA_RESULT_OK;
}

static int
spa_xv_sink_node_port_set_format (struct spa_node         *node,
                                  enum spa_direction     direction,
                                  uint32_t         port_id,
                                  uint32_t         flags,
                                  const struct spa_format *format)
{
  struct impl *this;
  struct port *port;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, struct impl, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = &this->in_ports[0];

  if (format == NULL) {
    port->have_format = false;
    return SPA_RESULT_OK;
  } else {
    struct spa_video_info info = { SPA_FORMAT_MEDIA_TYPE (format),
                                   SPA_FORMAT_MEDIA_SUBTYPE (format), };


    if (info.media_type != this->type.media_type.video &&
        info.media_subtype != this->type.media_subtype.raw)
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    if (!spa_format_video_raw_parse (format, &info.info.raw, &this->type.format_video))
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    if (spa_xv_set_format (this, &info, flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY) < 0)
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    if (!(flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY)) {
      port->current_format = info;
      port->have_format = true;
    }
  }

  return SPA_RESULT_OK;
}

static int
spa_xv_sink_node_port_get_format (struct spa_node          *node,
                                  enum spa_direction      direction,
                                  uint32_t          port_id,
                                  const struct spa_format **format)
{
  struct impl *this;
  struct port *port;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, struct impl, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = &this->in_ports[0];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = NULL;

  return SPA_RESULT_OK;
}

static int
spa_xv_sink_node_port_get_info (struct spa_node            *node,
                                enum spa_direction        direction,
                                uint32_t            port_id,
                                const struct spa_port_info **info)
{
  struct impl *this;
  struct port *port;

  if (node == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, struct impl, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = &this->in_ports[0];

  *info = &port->info;

  return SPA_RESULT_OK;
}

static int
spa_xv_sink_node_port_enum_params (struct spa_node       *node,
                                   enum spa_direction   direction,
                                   uint32_t       port_id,
                                   uint32_t       index,
                                   struct spa_param     **param)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_xv_sink_node_port_set_param (struct spa_node         *node,
                                 enum spa_direction     direction,
                                 uint32_t         port_id,
                                 const struct spa_param  *param)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_xv_sink_node_port_use_buffers (struct spa_node            *node,
                                   enum spa_direction        direction,
                                   uint32_t            port_id,
                                   struct spa_buffer **buffers,
                                   uint32_t            n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_xv_sink_node_port_alloc_buffers (struct spa_node            *node,
                                     enum spa_direction        direction,
                                     uint32_t            port_id,
                                     struct spa_param          **params,
                                     uint32_t            n_params,
                                     struct spa_buffer **buffers,
                                     uint32_t           *n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_xv_sink_node_port_set_io (struct spa_node      *node,
                              enum spa_direction  direction,
                              uint32_t      port_id,
                              struct spa_port_io    *io)
{
  struct impl *this;
  struct port *port;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, struct impl, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = &this->in_ports[0];
  port->io = io;

  return SPA_RESULT_OK;
}

static int
spa_xv_sink_node_port_reuse_buffer (struct spa_node         *node,
                                    uint32_t         port_id,
                                    uint32_t         buffer_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_xv_sink_node_port_send_command (struct spa_node            *node,
                                    enum spa_direction        direction,
                                    uint32_t            port_id,
                                    struct spa_command *command)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_xv_sink_node_process_input (struct spa_node          *node)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_xv_sink_node_process_output (struct spa_node           *node)
{
  return SPA_RESULT_INVALID_PORT;
}

static const struct spa_node xvsink_node = {
  sizeof (struct spa_node),
  NULL,
  spa_xv_sink_node_get_props,
  spa_xv_sink_node_set_props,
  spa_xv_sink_node_send_command,
  spa_xv_sink_node_set_callbacks,
  spa_xv_sink_node_get_n_ports,
  spa_xv_sink_node_get_port_ids,
  spa_xv_sink_node_add_port,
  spa_xv_sink_node_remove_port,
  spa_xv_sink_node_port_enum_formats,
  spa_xv_sink_node_port_set_format,
  spa_xv_sink_node_port_get_format,
  spa_xv_sink_node_port_get_info,
  spa_xv_sink_node_port_enum_params,
  spa_xv_sink_node_port_set_param,
  spa_xv_sink_node_port_use_buffers,
  spa_xv_sink_node_port_alloc_buffers,
  spa_xv_sink_node_port_set_io,
  spa_xv_sink_node_port_reuse_buffer,
  spa_xv_sink_node_port_send_command,
  spa_xv_sink_node_process_input,
  spa_xv_sink_node_process_output,
};

static int
spa_xv_sink_get_interface (struct spa_handle               *handle,
                           uint32_t                 interface_id,
                           void                   **interface)
{
  struct impl *this;

  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (struct impl *) handle;

  if (interface_id == this->type.node)
    *interface = &this->node;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static int
xv_sink_clear (struct spa_handle *handle)
{
  return SPA_RESULT_OK;
}

static int
xv_sink_init (const struct spa_handle_factory  *factory,
              struct spa_handle               *handle,
              const struct spa_dict           *info,
              const struct spa_support        *support,
              uint32_t                 n_support)
{
  struct impl *this;
  uint32_t i;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_xv_sink_get_interface;
  handle->clear = xv_sink_clear;

  this = (struct impl *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].type, SPA_TYPE__TypeMap) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE__Log) == 0)
      this->log = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "a type-map is needed");
    return SPA_RESULT_ERROR;
  }
  init_type (&this->type, this->map);

  this->node = xvsink_node;
  reset_props (&this->props);

  this->in_ports[0].info.flags = 0;

  return SPA_RESULT_OK;
}

static const struct spa_interface_info xv_sink_interfaces[] =
{
  { SPA_TYPE__Node, },
};

static int
xv_sink_enum_interface_info (const struct spa_handle_factory  *factory,
                             const struct spa_interface_info **info,
                             uint32_t                 index)
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

const struct spa_handle_factory spa_xv_sink_factory =
{
  "xv-sink",
  NULL,
  sizeof (struct impl),
  xv_sink_init,
  xv_sink_enum_interface_info,
};
