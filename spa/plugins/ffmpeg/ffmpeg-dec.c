/* Spa FFMpeg Decoder
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
#include <spa/node.h>
#include <spa/video/format-utils.h>
#include <lib/props.h>

#define IS_VALID_PORT(this,d,id) ((id) == 0)
#define MAX_BUFFERS    32

struct buffer {
  struct spa_buffer *outbuf;
  bool outstanding;
  struct buffer *next;
};

struct port {
  bool have_format;
  struct spa_video_info current_format;
  bool have_buffers;
  struct buffer buffers[MAX_BUFFERS];
  struct spa_port_info info;
  struct spa_port_io *io;
};

struct type {
  uint32_t node;
  struct spa_type_media_type media_type;
  struct spa_type_media_subtype media_subtype;
  struct spa_type_format_video format_video;
  struct spa_type_command_node command_node;
};

static inline void
init_type (struct type *type, struct spa_type_map *map)
{
  type->node = spa_type_map_get_id (map, SPA_TYPE__Node);
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

  struct spa_node_callbacks callbacks;
  void *user_data;

  struct port in_ports[1];
  struct port out_ports[1];

  bool started;
};

static int
spa_ffmpeg_dec_node_get_props (struct spa_node       *node,
                               struct spa_props     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_ffmpeg_dec_node_set_props (struct spa_node         *node,
                               const struct spa_props  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_ffmpeg_dec_node_send_command (struct spa_node    *node,
                                  struct spa_command *command)
{
  struct impl *this;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, struct impl, node);

  if (SPA_COMMAND_TYPE (command) == this->type.command_node.Start) {
    this->started = true;
  }
  else if (SPA_COMMAND_TYPE (command) == this->type.command_node.Pause) {
    this->started = false;
  }
  else
    return SPA_RESULT_NOT_IMPLEMENTED;

  return SPA_RESULT_OK;
}

static int
spa_ffmpeg_dec_node_set_callbacks (struct spa_node                *node,
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
spa_ffmpeg_dec_node_get_n_ports (struct spa_node       *node,
                                 uint32_t      *n_input_ports,
                                 uint32_t      *max_input_ports,
                                 uint32_t      *n_output_ports,
                                 uint32_t      *max_output_ports)
{
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports)
    *n_input_ports = 1;
  if (n_output_ports)
    *n_output_ports = 1;
  if (max_input_ports)
    *max_input_ports = 1;
  if (max_output_ports)
    *max_output_ports = 1;

  return SPA_RESULT_OK;
}

static int
spa_ffmpeg_dec_node_get_port_ids (struct spa_node       *node,
                                  uint32_t       n_input_ports,
                                  uint32_t      *input_ids,
                                  uint32_t       n_output_ports,
                                  uint32_t      *output_ids)
{
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports > 0 && input_ids != NULL)
    input_ids[0] = 0;
  if (n_output_ports > 0 && output_ids != NULL)
    output_ids[0] = 0;

  return SPA_RESULT_OK;
}


static int
spa_ffmpeg_dec_node_add_port (struct spa_node        *node,
                              enum spa_direction    direction,
                              uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_ffmpeg_dec_node_remove_port (struct spa_node        *node,
                                 enum spa_direction    direction,
                                 uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_ffmpeg_dec_node_port_enum_formats (struct spa_node         *node,
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

  if (!IS_VALID_PORT (this, direction, port_id))
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
spa_ffmpeg_dec_node_port_set_format (struct spa_node         *node,
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

  if (!IS_VALID_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

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

    if (!(flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY)) {
      port->current_format = info;
      port->have_format = true;
    }
  }
  return SPA_RESULT_OK;
}

static int
spa_ffmpeg_dec_node_port_get_format (struct spa_node          *node,
                                     enum spa_direction      direction,
                                     uint32_t          port_id,
                                     const struct spa_format **format)
{
  struct impl *this;
  struct port *port;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, struct impl, node);

  if (!IS_VALID_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = NULL;

  return SPA_RESULT_OK;
}

static int
spa_ffmpeg_dec_node_port_get_info (struct spa_node            *node,
                                   enum spa_direction        direction,
                                   uint32_t            port_id,
                                   const struct spa_port_info **info)
{
  struct impl *this;
  struct port *port;

  if (node == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, struct impl, node);

  if (!IS_VALID_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];
  *info = &port->info;

  return SPA_RESULT_OK;
}

static int
spa_ffmpeg_dec_node_port_enum_params (struct spa_node        *node,
                                      enum spa_direction    direction,
                                      uint32_t        port_id,
                                      uint32_t        index,
                                      struct spa_param      **param)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_ffmpeg_dec_node_port_set_param (struct spa_node         *node,
                                    enum spa_direction     direction,
                                    uint32_t         port_id,
                                    const struct spa_param  *param)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_ffmpeg_dec_node_port_use_buffers (struct spa_node            *node,
                                      enum spa_direction        direction,
                                      uint32_t            port_id,
                                      struct spa_buffer **buffers,
                                      uint32_t            n_buffers)
{
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (!IS_VALID_PORT (node, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_ffmpeg_dec_node_port_alloc_buffers (struct spa_node            *node,
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
spa_ffmpeg_dec_node_port_set_io (struct spa_node      *node,
                                 enum spa_direction  direction,
                                 uint32_t      port_id,
                                 struct spa_port_io    *io)
{
  struct impl *this;
  struct port *port;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, struct impl, node);

  if (!IS_VALID_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];
  port->io = io;

  return SPA_RESULT_OK;
}

static int
spa_ffmpeg_dec_node_process_input (struct spa_node *node)
{
  return SPA_RESULT_INVALID_PORT;
}

static int
spa_ffmpeg_dec_node_process_output (struct spa_node *node)
{
  struct impl *this;
  struct port *port;
  struct spa_port_io *output;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, struct impl, node);

  port = &this->out_ports[0];

  if ((output = port->io) == NULL)
    return SPA_RESULT_ERROR;

  if (!port->have_format) {
    output->status = SPA_RESULT_NO_FORMAT;
    return SPA_RESULT_ERROR;
  }
  output->status = SPA_RESULT_OK;

  return SPA_RESULT_OK;
}

static int
spa_ffmpeg_dec_node_port_reuse_buffer (struct spa_node         *node,
                                       uint32_t         port_id,
                                       uint32_t         buffer_id)
{
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_ffmpeg_dec_node_port_send_command (struct spa_node            *node,
                                       enum spa_direction        direction,
                                       uint32_t            port_id,
                                       struct spa_command *command)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}


static const struct spa_node ffmpeg_dec_node = {
  sizeof (struct spa_node),
  NULL,
  spa_ffmpeg_dec_node_get_props,
  spa_ffmpeg_dec_node_set_props,
  spa_ffmpeg_dec_node_send_command,
  spa_ffmpeg_dec_node_set_callbacks,
  spa_ffmpeg_dec_node_get_n_ports,
  spa_ffmpeg_dec_node_get_port_ids,
  spa_ffmpeg_dec_node_add_port,
  spa_ffmpeg_dec_node_remove_port,
  spa_ffmpeg_dec_node_port_enum_formats,
  spa_ffmpeg_dec_node_port_set_format,
  spa_ffmpeg_dec_node_port_get_format,
  spa_ffmpeg_dec_node_port_get_info,
  spa_ffmpeg_dec_node_port_enum_params,
  spa_ffmpeg_dec_node_port_set_param,
  spa_ffmpeg_dec_node_port_use_buffers,
  spa_ffmpeg_dec_node_port_alloc_buffers,
  spa_ffmpeg_dec_node_port_set_io,
  spa_ffmpeg_dec_node_port_reuse_buffer,
  spa_ffmpeg_dec_node_port_send_command,
  spa_ffmpeg_dec_node_process_input,
  spa_ffmpeg_dec_node_process_output,
};

static int
spa_ffmpeg_dec_get_interface (struct spa_handle         *handle,
                              uint32_t           interface_id,
                              void             **interface)
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

int
spa_ffmpeg_dec_init (struct spa_handle             *handle,
                     const struct spa_dict *info,
                     const struct spa_support      *support,
                     uint32_t               n_support)
{
  struct impl *this;
  uint32_t i;

  handle->get_interface = spa_ffmpeg_dec_get_interface;

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

  this->node = ffmpeg_dec_node;

  this->in_ports[0].info.flags = 0;
  this->out_ports[0].info.flags = 0;

  return SPA_RESULT_OK;
}
