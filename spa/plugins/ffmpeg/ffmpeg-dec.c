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

#include <spa/id-map.h>
#include <spa/log.h>
#include <spa/node.h>
#include <spa/video/format-utils.h>
#include <lib/props.h>

typedef struct _SpaFFMpegDec SpaFFMpegDec;

#define IS_VALID_PORT(this,d,id) ((id) == 0)
#define MAX_BUFFERS    32

typedef struct _FFMpegBuffer FFMpegBuffer;

struct _FFMpegBuffer {
  SpaBuffer *outbuf;
  bool outstanding;
  FFMpegBuffer *next;
};

typedef struct {
  bool have_format;
  SpaVideoInfo current_format;
  bool have_buffers;
  FFMpegBuffer buffers[MAX_BUFFERS];
  SpaPortInfo info;
  void *io;
} SpaFFMpegPort;

typedef struct {
  uint32_t node;
  SpaCommandNode command_node;
} URI;

struct _SpaFFMpegDec {
  SpaHandle handle;
  SpaNode   node;

  URI uri;
  SpaIDMap *map;
  SpaLog *log;

  SpaEventNodeCallback event_cb;
  void *user_data;

  SpaFFMpegPort in_ports[1];
  SpaFFMpegPort out_ports[1];
};

enum {
  PROP_ID_LAST,
};

static SpaResult
spa_ffmpeg_dec_node_get_props (SpaNode       *node,
                               SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_ffmpeg_dec_node_set_props (SpaNode         *node,
                               const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static void
update_state (SpaFFMpegDec *this, SpaNodeState state)
{
  this->node.state = state;
}

static SpaResult
spa_ffmpeg_dec_node_send_command (SpaNode    *node,
                                  SpaCommand *command)
{
  SpaFFMpegDec *this;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaFFMpegDec, node);

  if (SPA_COMMAND_TYPE (command) == this->uri.command_node.Start) {
    update_state (this, SPA_NODE_STATE_STREAMING);
  }
  else if (SPA_COMMAND_TYPE (command) == this->uri.command_node.Pause) {
    update_state (this, SPA_NODE_STATE_PAUSED);
  }
  else
    return SPA_RESULT_NOT_IMPLEMENTED;

  return SPA_RESULT_OK;
}

static SpaResult
spa_ffmpeg_dec_node_set_event_callback (SpaNode              *node,
                                        SpaEventNodeCallback  event,
                                        void                 *user_data)
{
  SpaFFMpegDec *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaFFMpegDec, node);

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_ffmpeg_dec_node_get_n_ports (SpaNode       *node,
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
spa_ffmpeg_dec_node_get_port_ids (SpaNode       *node,
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
spa_ffmpeg_dec_node_add_port (SpaNode        *node,
                              SpaDirection    direction,
                              uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_ffmpeg_dec_node_remove_port (SpaNode        *node,
                                 SpaDirection    direction,
                                 uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_ffmpeg_dec_node_port_enum_formats (SpaNode         *node,
                                       SpaDirection     direction,
                                       uint32_t         port_id,
                                       SpaFormat      **format,
                                       const SpaFormat *filter,
                                       uint32_t         index)
{
  //SpaFFMpegDec *this;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  //this = SPA_CONTAINER_OF (node, SpaFFMpegDec, node);

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

static SpaResult
spa_ffmpeg_dec_node_port_set_format (SpaNode            *node,
                                     SpaDirection        direction,
                                     uint32_t            port_id,
                                     SpaPortFormatFlags  flags,
                                     const SpaFormat    *format)
{
  SpaFFMpegDec *this;
  SpaFFMpegPort *port;
  SpaResult res;
  SpaVideoInfo query_format;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaFFMpegDec, node);

  if (!IS_VALID_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (format == NULL) {
    port->have_format = false;
    return SPA_RESULT_OK;
  }

  if ((res = spa_format_video_parse (format, &query_format) < 0))
    return res;

  if (!(flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY)) {
    memcpy (&port->current_format, &query_format, sizeof (SpaVideoInfo));
    port->have_format = true;
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_ffmpeg_dec_node_port_get_format (SpaNode          *node,
                                     SpaDirection      direction,
                                     uint32_t          port_id,
                                     const SpaFormat **format)
{
  SpaFFMpegDec *this;
  SpaFFMpegPort *port;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaFFMpegDec, node);

  if (!IS_VALID_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = NULL;

  return SPA_RESULT_OK;
}

static SpaResult
spa_ffmpeg_dec_node_port_get_info (SpaNode            *node,
                                   SpaDirection        direction,
                                   uint32_t            port_id,
                                   const SpaPortInfo **info)
{
  SpaFFMpegDec *this;
  SpaFFMpegPort *port;

  if (node == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaFFMpegDec, node);

  if (!IS_VALID_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];
  *info = &port->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_ffmpeg_dec_node_port_get_props (SpaNode        *node,
                                    SpaDirection    direction,
                                    uint32_t        port_id,
                                    SpaProps      **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_ffmpeg_dec_node_port_set_props (SpaNode         *node,
                                    SpaDirection     direction,
                                    uint32_t         port_id,
                                    const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_ffmpeg_dec_node_port_use_buffers (SpaNode         *node,
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
spa_ffmpeg_dec_node_port_alloc_buffers (SpaNode         *node,
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
spa_ffmpeg_dec_node_port_set_input (SpaNode      *node,
                                    uint32_t      port_id,
                                    SpaPortInput *input)
{
  SpaFFMpegDec *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaFFMpegDec, node);

  if (!IS_VALID_PORT (this, SPA_DIRECTION_INPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->in_ports[port_id].io = input;

  return SPA_RESULT_OK;
}

static SpaResult
spa_ffmpeg_dec_node_port_set_output (SpaNode       *node,
                                     uint32_t       port_id,
                                     SpaPortOutput *output)
{
  SpaFFMpegDec *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaFFMpegDec, node);

  if (!IS_VALID_PORT (this, SPA_DIRECTION_OUTPUT, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->out_ports[port_id].io = output;

  return SPA_RESULT_OK;
}

static SpaResult
spa_ffmpeg_dec_node_process_input (SpaNode *node)
{
  return SPA_RESULT_INVALID_PORT;
}

static SpaResult
spa_ffmpeg_dec_node_process_output (SpaNode *node)
{
  SpaFFMpegDec *this;
  SpaFFMpegPort *port;
  SpaPortOutput *output;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaFFMpegDec, node);

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

static SpaResult
spa_ffmpeg_dec_node_port_reuse_buffer (SpaNode         *node,
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
spa_ffmpeg_dec_node_port_send_command (SpaNode        *node,
                                       SpaDirection    direction,
                                       uint32_t        port_id,
                                       SpaCommand     *command)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}


static const SpaNode ffmpeg_dec_node = {
  sizeof (SpaNode),
  NULL,
  SPA_NODE_STATE_INIT,
  spa_ffmpeg_dec_node_get_props,
  spa_ffmpeg_dec_node_set_props,
  spa_ffmpeg_dec_node_send_command,
  spa_ffmpeg_dec_node_set_event_callback,
  spa_ffmpeg_dec_node_get_n_ports,
  spa_ffmpeg_dec_node_get_port_ids,
  spa_ffmpeg_dec_node_add_port,
  spa_ffmpeg_dec_node_remove_port,
  spa_ffmpeg_dec_node_port_enum_formats,
  spa_ffmpeg_dec_node_port_set_format,
  spa_ffmpeg_dec_node_port_get_format,
  spa_ffmpeg_dec_node_port_get_info,
  spa_ffmpeg_dec_node_port_get_props,
  spa_ffmpeg_dec_node_port_set_props,
  spa_ffmpeg_dec_node_port_use_buffers,
  spa_ffmpeg_dec_node_port_alloc_buffers,
  spa_ffmpeg_dec_node_port_set_input,
  spa_ffmpeg_dec_node_port_set_output,
  spa_ffmpeg_dec_node_port_reuse_buffer,
  spa_ffmpeg_dec_node_port_send_command,
  spa_ffmpeg_dec_node_process_input,
  spa_ffmpeg_dec_node_process_output,
};

static SpaResult
spa_ffmpeg_dec_get_interface (SpaHandle         *handle,
                              uint32_t           interface_id,
                              void             **interface)
{
  SpaFFMpegDec *this;

  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaFFMpegDec *) handle;

  if (interface_id == this->uri.node)
    *interface = &this->node;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

SpaResult
spa_ffmpeg_dec_init (SpaHandle         *handle,
                     const SpaDict     *info,
                     const SpaSupport  *support,
                     uint32_t           n_support)
{
  SpaFFMpegDec *this;
  uint32_t i;

  handle->get_interface = spa_ffmpeg_dec_get_interface;

  this = (SpaFFMpegDec *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].uri, SPA_TYPE__IDMap) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].uri, SPA_TYPE__Log) == 0)
      this->log = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "an id-map is needed");
    return SPA_RESULT_ERROR;
  }
  this->uri.node = spa_id_map_get_id (this->map, SPA_TYPE__Node);

  this->node = ffmpeg_dec_node;
  spa_command_node_map (this->map, &this->uri.command_node);

  this->in_ports[0].info.flags = SPA_PORT_INFO_FLAG_NONE;
  this->out_ports[0].info.flags = SPA_PORT_INFO_FLAG_NONE;

  return SPA_RESULT_OK;
}
