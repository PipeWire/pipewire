/* Spa FFMpeg Encoder
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

#include <spa/log.h>
#include <spa/type-map.h>
#include <spa/node.h>
#include <spa/video/format-utils.h>
#include <lib/props.h>

typedef struct _SpaFFMpegEnc SpaFFMpegEnc;

#define IS_VALID_PORT(this,d,id) ((id) == 0)
#define MAX_BUFFERS    32

typedef struct _FFMpegBuffer FFMpegBuffer;

struct _FFMpegBuffer {
  SpaBuffer buffer;
  SpaMeta metas[1];
  SpaMetaHeader header;
  SpaData datas[1];
  SpaFFMpegEnc *enc;
  SpaBuffer *imported;
  bool outstanding;
  FFMpegBuffer *next;
};

typedef struct {
  bool have_format;
  SpaVideoInfo current_format;
  bool have_buffers;
  FFMpegBuffer buffers[MAX_BUFFERS];
  SpaPortInfo info;
  SpaPortIO *io;
} SpaFFMpegPort;

typedef struct {
  uint32_t node;
  SpaTypeMediaType media_type;
  SpaTypeMediaSubtype media_subtype;
  SpaTypeFormatVideo format_video;
  SpaTypeCommandNode command_node;
} Type;

static inline void
init_type (Type *type, SpaTypeMap *map)
{
  type->node = spa_type_map_get_id (map, SPA_TYPE__Node);
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_format_video_map (map, &type->format_video);
  spa_type_command_node_map (map, &type->command_node);
}

struct _SpaFFMpegEnc {
  SpaHandle handle;
  SpaNode node;

  Type type;
  SpaTypeMap *map;
  SpaLog *log;

  SpaNodeCallbacks callbacks;
  void *user_data;

  SpaFFMpegPort in_ports[1];
  SpaFFMpegPort out_ports[1];

  bool started;
};

static SpaResult
spa_ffmpeg_enc_node_get_props (SpaNode       *node,
                               SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_ffmpeg_enc_node_set_props (SpaNode         *node,
                               const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_ffmpeg_enc_node_send_command (SpaNode    *node,
                                  SpaCommand *command)
{
  SpaFFMpegEnc *this;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaFFMpegEnc, node);

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

static SpaResult
spa_ffmpeg_enc_node_set_callbacks (SpaNode                *node,
                                   const SpaNodeCallbacks *callbacks,
                                   size_t                  callbacks_size,
                                   void                   *user_data)
{
  SpaFFMpegEnc *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaFFMpegEnc, node);

  this->callbacks = *callbacks;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_ffmpeg_enc_node_get_n_ports (SpaNode       *node,
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

static SpaResult
spa_ffmpeg_enc_node_get_port_ids (SpaNode       *node,
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


static SpaResult
spa_ffmpeg_enc_node_add_port (SpaNode        *node,
                              SpaDirection    direction,
                              uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_ffmpeg_enc_node_remove_port (SpaNode        *node,
                                 SpaDirection    direction,
                                 uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_ffmpeg_enc_node_port_enum_formats (SpaNode         *node,
                                       SpaDirection     direction,
                                       uint32_t         port_id,
                                       SpaFormat      **format,
                                       const SpaFormat *filter,
                                       uint32_t         index)
{
  //SpaFFMpegEnc *this;
  //SpaFFMpegPort *port;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  //this = SPA_CONTAINER_OF (node, SpaFFMpegEnc, node);

  if (!IS_VALID_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  //port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  switch (index) {
    case 0:
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = NULL;

  return SPA_RESULT_OK;
}

static SpaResult
spa_ffmpeg_enc_node_port_set_format (SpaNode         *node,
                                     SpaDirection     direction,
                                     uint32_t         port_id,
                                     uint32_t         flags,
                                     const SpaFormat *format)
{
  SpaFFMpegEnc *this;
  SpaFFMpegPort *port;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaFFMpegEnc, node);

  if (!IS_VALID_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (format == NULL) {
    port->have_format = false;
    return SPA_RESULT_OK;
  } else {
    SpaVideoInfo info = { SPA_FORMAT_MEDIA_TYPE (format),
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

static SpaResult
spa_ffmpeg_enc_node_port_get_format (SpaNode          *node,
                                     SpaDirection      direction,
                                     uint32_t          port_id,
                                     const SpaFormat **format)
{
  SpaFFMpegEnc *this;
  SpaFFMpegPort *port;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaFFMpegEnc, node);

  if (!IS_VALID_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = NULL;

  return SPA_RESULT_OK;
}

static SpaResult
spa_ffmpeg_enc_node_port_get_info (SpaNode            *node,
                                   SpaDirection        direction,
                                   uint32_t            port_id,
                                   const SpaPortInfo **info)
{
  SpaFFMpegEnc *this;
  SpaFFMpegPort *port;

  if (node == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaFFMpegEnc, node);

  if (!IS_VALID_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];
  *info = &port->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_ffmpeg_enc_node_port_enum_params (SpaNode       *node,
                                      SpaDirection   direction,
                                      uint32_t       port_id,
                                      uint32_t       index,
                                      SpaParam     **param)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_ffmpeg_enc_node_port_set_param (SpaNode         *node,
                                    SpaDirection     direction,
                                    uint32_t         port_id,
                                    const SpaParam  *param)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_ffmpeg_enc_node_port_use_buffers (SpaNode         *node,
                                      SpaDirection     direction,
                                      uint32_t         port_id,
                                      SpaBuffer      **buffers,
                                      uint32_t         n_buffers)
{
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (!IS_VALID_PORT (node, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_ffmpeg_enc_node_port_alloc_buffers (SpaNode         *node,
                                        SpaDirection     direction,
                                        uint32_t         port_id,
                                        SpaParam       **params,
                                        uint32_t         n_params,
                                        SpaBuffer      **buffers,
                                        uint32_t        *n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_ffmpeg_enc_node_port_set_io (SpaNode      *node,
                                 SpaDirection  direction,
                                 uint32_t      port_id,
                                 SpaPortIO    *io)
{
  SpaFFMpegEnc *this;
  SpaFFMpegPort *port;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaFFMpegEnc, node);

  if (!IS_VALID_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];
  port->io = io;

  return SPA_RESULT_OK;
}

static SpaResult
spa_ffmpeg_enc_node_port_reuse_buffer (SpaNode         *node,
                                       uint32_t         port_id,
                                       uint32_t         buffer_id)
{
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_ffmpeg_enc_node_port_send_command (SpaNode        *node,
                                       SpaDirection    direction,
                                       uint32_t        port_id,
                                       SpaCommand     *command)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_ffmpeg_enc_node_process_input (SpaNode *node)
{
  return SPA_RESULT_INVALID_PORT;
}

static SpaResult
spa_ffmpeg_enc_node_process_output (SpaNode *node)
{
  SpaFFMpegEnc *this;
  SpaFFMpegPort *port;
  SpaPortIO *output;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaFFMpegEnc, node);

  if ((output = this->out_ports[0].io) == NULL)
    return SPA_RESULT_OK;

  port = &this->out_ports[0];

  if (!port->have_format) {
    output->status = SPA_RESULT_NO_FORMAT;
    return SPA_RESULT_ERROR;
  }
  output->status = SPA_RESULT_OK;

  return SPA_RESULT_OK;
}

static const SpaNode ffmpeg_enc_node = {
  sizeof (SpaNode),
  NULL,
  spa_ffmpeg_enc_node_get_props,
  spa_ffmpeg_enc_node_set_props,
  spa_ffmpeg_enc_node_send_command,
  spa_ffmpeg_enc_node_set_callbacks,
  spa_ffmpeg_enc_node_get_n_ports,
  spa_ffmpeg_enc_node_get_port_ids,
  spa_ffmpeg_enc_node_add_port,
  spa_ffmpeg_enc_node_remove_port,
  spa_ffmpeg_enc_node_port_enum_formats,
  spa_ffmpeg_enc_node_port_set_format,
  spa_ffmpeg_enc_node_port_get_format,
  spa_ffmpeg_enc_node_port_get_info,
  spa_ffmpeg_enc_node_port_enum_params,
  spa_ffmpeg_enc_node_port_set_param,
  spa_ffmpeg_enc_node_port_use_buffers,
  spa_ffmpeg_enc_node_port_alloc_buffers,
  spa_ffmpeg_enc_node_port_set_io,
  spa_ffmpeg_enc_node_port_reuse_buffer,
  spa_ffmpeg_enc_node_port_send_command,
  spa_ffmpeg_enc_node_process_input,
  spa_ffmpeg_enc_node_process_output,
};

static SpaResult
spa_ffmpeg_enc_get_interface (SpaHandle         *handle,
                              uint32_t           interface_id,
                              void             **interface)
{
  SpaFFMpegEnc *this;

  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaFFMpegEnc *) handle;

  if (interface_id == this->type.node)
    *interface = &this->node;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

SpaResult
spa_ffmpeg_enc_init (SpaHandle         *handle,
                     const SpaDict     *info,
                     const SpaSupport  *support,
                     uint32_t           n_support)
{
  SpaFFMpegEnc *this;
  uint32_t i;

  handle->get_interface = spa_ffmpeg_enc_get_interface;

  this = (SpaFFMpegEnc *) handle;

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

  this->node = ffmpeg_enc_node;

  this->in_ports[0].info.flags = 0;
  this->out_ports[0].info.flags = 0;

  return SPA_RESULT_OK;
}
