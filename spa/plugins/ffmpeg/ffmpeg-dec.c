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

#include <spa/node.h>
#include <spa/video/format.h>

typedef struct _SpaFFMpegDec SpaFFMpegDec;

typedef struct {
  SpaProps props;
} SpaFFMpegDecProps;

static void
reset_ffmpeg_dec_props (SpaFFMpegDecProps *props)
{
}

#define IS_VALID_PORT(this,d,id) ((id) == 0)
#define MAX_BUFFERS    32

typedef struct _FFMpegBuffer FFMpegBuffer;

struct _FFMpegBuffer {
  SpaBuffer *outbuf;
  bool outstanding;
  FFMpegBuffer *next;
};

typedef struct {
  SpaFormatVideo format[2];
  SpaFormat *current_format;
  bool have_buffers;
  FFMpegBuffer buffers[MAX_BUFFERS];
  SpaPortInfo info;
  SpaAllocParam *params[1];
  SpaAllocParamBuffers param_buffers;
  SpaPortStatus status;
} SpaFFMpegPort;

struct _SpaFFMpegDec {
  SpaHandle handle;
  SpaNode   node;

  SpaFFMpegDecProps props[2];

  SpaNodeEventCallback event_cb;
  void *user_data;

  SpaFFMpegPort in_ports[1];
  SpaFFMpegPort out_ports[1];
};

enum {
  PROP_ID_LAST,
};

static const SpaPropInfo prop_info[] =
{
  { 0, },
};

static SpaResult
spa_ffmpeg_dec_node_get_props (SpaNode       *node,
                               SpaProps     **props)
{
  SpaFFMpegDec *this;

  if (node == NULL || node->handle == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaFFMpegDec *) node->handle;

  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));
  *props = &this->props[0].props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_ffmpeg_dec_node_set_props (SpaNode         *node,
                               const SpaProps  *props)
{
  SpaFFMpegDec *this;
  SpaFFMpegDecProps *p;
  SpaResult res;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaFFMpegDec *) node->handle;
  p = &this->props[1];

  if (props == NULL) {
    reset_ffmpeg_dec_props (p);
    return SPA_RESULT_OK;
  }

  res = spa_props_copy_values (props, &p->props);

  return res;
}

static void
update_state (SpaFFMpegDec *this, SpaNodeState state)
{
  this->node.state = state;
}

static SpaResult
spa_ffmpeg_dec_node_send_command (SpaNode        *node,
                                  SpaNodeCommand *command)
{
  SpaFFMpegDec *this;

  if (node == NULL || node->handle == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaFFMpegDec *) node->handle;

  switch (command->type) {
    case SPA_NODE_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_NODE_COMMAND_START:
      update_state (this, SPA_NODE_STATE_STREAMING);
      break;

    case SPA_NODE_COMMAND_PAUSE:
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
spa_ffmpeg_dec_node_set_event_callback (SpaNode              *node,
                                        SpaNodeEventCallback  event,
                                        void                 *user_data)
{
  SpaFFMpegDec *this;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaFFMpegDec *) node->handle;

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_ffmpeg_dec_node_get_n_ports (SpaNode       *node,
                                 unsigned int  *n_input_ports,
                                 unsigned int  *max_input_ports,
                                 unsigned int  *n_output_ports,
                                 unsigned int  *max_output_ports)
{
  if (node == NULL || node->handle == NULL)
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
                                  unsigned int   n_input_ports,
                                  uint32_t      *input_ids,
                                  unsigned int   n_output_ports,
                                  uint32_t      *output_ids)
{
  if (node == NULL || node->handle == NULL)
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
                                       void           **state)
{
  SpaFFMpegDec *this;
  SpaFFMpegPort *port;
  int index;

  if (node == NULL || node->handle == NULL || format == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaFFMpegDec *) node->handle;

  if (!IS_VALID_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      spa_format_video_init (SPA_MEDIA_TYPE_VIDEO,
                             SPA_MEDIA_SUBTYPE_RAW,
                             &port->format[0]);
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = &port->format[0].format;
  *(int*)state = ++index;

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

  if (node == NULL || node->handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaFFMpegDec *) node->handle;

  if (!IS_VALID_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (format == NULL) {
    port->current_format = NULL;
    return SPA_RESULT_OK;
  }

  if ((res = spa_format_video_parse (format, &port->format[0]) < 0))
    return res;

  if (!(flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY)) {
    memcpy (&port->format[1], &port->format[0], sizeof (SpaFormatVideo));
    port->current_format = &port->format[1].format;
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

  if (node == NULL || node->handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaFFMpegDec *) node->handle;

  if (!IS_VALID_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

  if (port->current_format == NULL)
    return SPA_RESULT_NO_FORMAT;

  *format = port->current_format;

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

  if (node == NULL || node->handle == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaFFMpegDec *) node->handle;

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
  if (node == NULL || node->handle == NULL)
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
spa_ffmpeg_dec_node_port_get_status (SpaNode              *node,
                                     SpaDirection          direction,
                                     uint32_t              port_id,
                                     const SpaPortStatus **status)
{
  SpaFFMpegDec *this;
  SpaFFMpegPort *port;

  if (node == NULL || node->handle == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaFFMpegDec *) node->handle;

  if (!IS_VALID_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  port = direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];
  *status = &port->status;

  return SPA_RESULT_OK;
}

static SpaResult
spa_ffmpeg_dec_node_port_push_input (SpaNode          *node,
                                     unsigned int      n_info,
                                     SpaPortInputInfo *info)
{
  return SPA_RESULT_INVALID_PORT;
}

static SpaResult
spa_ffmpeg_dec_node_port_pull_output (SpaNode           *node,
                                      unsigned int       n_info,
                                      SpaPortOutputInfo *info)
{
  SpaFFMpegDec *this;
  SpaFFMpegPort *port;
  unsigned int i;
  bool have_error = false;

  if (node == NULL || node->handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaFFMpegDec *) node->handle;

  for (i = 0; i < n_info; i++) {
    if (info[i].port_id != 0) {
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }
    port = &this->out_ports[info[i].port_id];

    if (port->current_format == NULL) {
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

static SpaResult
spa_ffmpeg_dec_node_port_reuse_buffer (SpaNode         *node,
                                       uint32_t         port_id,
                                       uint32_t         buffer_id)
{
  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_ffmpeg_dec_node_port_push_event (SpaNode      *node,
                                     SpaDirection  direction,
                                     uint32_t      port_id,
                                     SpaNodeEvent *event)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}


static const SpaNode ffmpeg_dec_node = {
  NULL,
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
  spa_ffmpeg_dec_node_port_get_status,
  spa_ffmpeg_dec_node_port_push_input,
  spa_ffmpeg_dec_node_port_pull_output,
  spa_ffmpeg_dec_node_port_reuse_buffer,
  spa_ffmpeg_dec_node_port_push_event,
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

  switch (interface_id) {
    case SPA_INTERFACE_ID_NODE:
      *interface = &this->node;
      break;
    default:
      return SPA_RESULT_UNKNOWN_INTERFACE;
  }
  return SPA_RESULT_OK;
}

SpaResult
spa_ffmpeg_dec_init (SpaHandle *handle)
{
  SpaFFMpegDec *this;

  handle->get_interface = spa_ffmpeg_dec_get_interface;

  this = (SpaFFMpegDec *) handle;
  this->node = ffmpeg_dec_node;
  this->node.handle = handle;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  reset_ffmpeg_dec_props (&this->props[1]);

  this->in_ports[0].info.flags = SPA_PORT_INFO_FLAG_NONE;
  this->in_ports[0].status.flags = SPA_PORT_STATUS_FLAG_NONE;

  this->out_ports[0].info.flags = SPA_PORT_INFO_FLAG_NONE;
  this->out_ports[0].status.flags = SPA_PORT_STATUS_FLAG_NONE;

  return SPA_RESULT_OK;
}
