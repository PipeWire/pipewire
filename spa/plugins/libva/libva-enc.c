/* Spa Libva Encoder
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

typedef struct _SpaLibvaEnc SpaLibvaEnc;

typedef struct {
  SpaProps props;
} SpaLibvaEncProps;

static void
reset_libva_enc_props (SpaLibvaEncProps *props)
{
}

#define INPUT_PORT_ID  0
#define OUTPUT_PORT_ID  1
#define IS_VALID_PORT(id) ((id) < 2)
#define MAX_BUFFERS    32

typedef struct _LibvaBuffer LibvaBuffer;

struct _LibvaBuffer {
  SpaBuffer buffer;
  SpaMeta metas[1];
  SpaMetaHeader header;
  SpaData datas[1];
  SpaLibvaEnc *enc;
  SpaBuffer *imported;
  bool outstanding;
  LibvaBuffer *next;
};

typedef struct {
  SpaVideoRawFormat raw_format[2];
  SpaFormat *current_format;
  bool have_buffers;
  LibvaBuffer buffers[MAX_BUFFERS];
  SpaPortInfo info;
  SpaAllocParam *params[1];
  SpaAllocParamBuffers param_buffers;
  SpaPortStatus status;
} SpaLibvaState;

struct _SpaLibvaEnc {
  SpaHandle handle;

  SpaLibvaEncProps props[2];

  SpaEventCallback event_cb;
  void *user_data;

  SpaLibvaState state[2];
};

enum {
  PROP_ID_LAST,
};

static const SpaPropInfo prop_info[] =
{
  { 0, },
};

static SpaResult
spa_libva_enc_node_get_props (SpaHandle     *handle,
                              SpaProps     **props)
{
  SpaLibvaEnc *this = (SpaLibvaEnc *) handle;

  if (handle == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));
  *props = &this->props[0].props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_libva_enc_node_set_props (SpaHandle       *handle,
                              const SpaProps  *props)
{
  SpaLibvaEnc *this = (SpaLibvaEnc *) handle;
  SpaLibvaEncProps *p = &this->props[1];
  SpaResult res;

  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (props == NULL) {
    reset_libva_enc_props (p);
    return SPA_RESULT_OK;
  }

  res = spa_props_copy_values (props, &p->props);

  return res;
}

static SpaResult
spa_libva_enc_node_send_command (SpaHandle     *handle,
                                 SpaCommand    *command)
{
  SpaLibvaEnc *this = (SpaLibvaEnc *) handle;

  if (handle == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  switch (command->type) {
    case SPA_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_COMMAND_START:
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
spa_libva_enc_node_set_event_callback (SpaHandle     *handle,
                                       SpaEventCallback event,
                                       void          *user_data)
{
  SpaLibvaEnc *this = (SpaLibvaEnc *) handle;

  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_libva_enc_node_get_n_ports (SpaHandle     *handle,
                                uint32_t      *n_input_ports,
                                uint32_t      *max_input_ports,
                                uint32_t      *n_output_ports,
                                uint32_t      *max_output_ports)
{
  if (handle == NULL)
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
spa_libva_enc_node_get_port_ids (SpaHandle     *handle,
                                 uint32_t       n_input_ports,
                                 uint32_t      *input_ids,
                                 uint32_t       n_output_ports,
                                 uint32_t      *output_ids)
{
  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports > 0 && input_ids != NULL)
    input_ids[0] = 0;
  if (n_output_ports > 0 && output_ids != NULL)
    output_ids[0] = 1;

  return SPA_RESULT_OK;
}


static SpaResult
spa_libva_enc_node_add_port (SpaHandle      *handle,
                             SpaDirection    direction,
                             uint32_t       *port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_libva_enc_node_remove_port (SpaHandle      *handle,
                                 uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_libva_enc_node_port_enum_formats (SpaHandle       *handle,
                                      uint32_t         port_id,
                                      uint32_t         index,
                                      SpaFormat      **format)
{
  SpaLibvaEnc *this = (SpaLibvaEnc *) handle;
  SpaLibvaState *state;

  if (handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (!IS_VALID_PORT (port_id))
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
spa_libva_enc_node_port_set_format (SpaHandle       *handle,
                                    uint32_t         port_id,
                                    bool             test_only,
                                    const SpaFormat *format)
{
  SpaLibvaEnc *this = (SpaLibvaEnc *) handle;
  SpaLibvaState *state;
  SpaResult res;
  SpaFormat *f, *tf;
  size_t fs;

  if (handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (!IS_VALID_PORT (port_id))
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

  if (!test_only) {
    memcpy (tf, f, fs);
    state->current_format = tf;
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_libva_enc_node_port_get_format (SpaHandle        *handle,
                                    uint32_t          port_id,
                                    const SpaFormat **format)
{
  SpaLibvaEnc *this = (SpaLibvaEnc *) handle;
  SpaLibvaState *state;

  if (handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (!IS_VALID_PORT (port_id))
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (state->current_format == NULL)
    return SPA_RESULT_NO_FORMAT;

  *format = state->current_format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_libva_enc_node_port_get_info (SpaHandle          *handle,
                                  uint32_t            port_id,
                                  const SpaPortInfo **info)
{
  SpaLibvaEnc *this = (SpaLibvaEnc *) handle;

  if (handle == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (!IS_VALID_PORT (port_id))
    return SPA_RESULT_INVALID_PORT;

  *info = &this->state[port_id].info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_libva_enc_node_port_get_props (SpaHandle  *handle,
                                   uint32_t    port_id,
                                   SpaProps  **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_libva_enc_node_port_set_props (SpaHandle       *handle,
                                   uint32_t         port_id,
                                   const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_libva_enc_node_port_get_status (SpaHandle            *handle,
                                    uint32_t              port_id,
                                    const SpaPortStatus **status)
{
  SpaLibvaEnc *this = (SpaLibvaEnc *) handle;

  if (handle == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (!IS_VALID_PORT (port_id))
    return SPA_RESULT_INVALID_PORT;

  *status = &this->state[port_id].status;

  return SPA_RESULT_OK;
}

static SpaResult
spa_libva_enc_node_port_use_buffers (SpaHandle       *handle,
                                     uint32_t         port_id,
                                     SpaBuffer      **buffers,
                                     uint32_t         n_buffers)
{
  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (!IS_VALID_PORT (port_id))
    return SPA_RESULT_INVALID_PORT;

  return SPA_RESULT_OK;
}

static SpaResult
spa_libva_enc_node_port_alloc_buffers (SpaHandle       *handle,
                                       uint32_t         port_id,
                                       SpaAllocParam  **params,
                                       uint32_t         n_params,
                                       SpaBuffer      **buffers,
                                       uint32_t        *n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}


static SpaResult
spa_libva_enc_node_port_push_input (SpaHandle      *handle,
                                    uint32_t        n_info,
                                    SpaInputInfo   *info)
{
  return SPA_RESULT_INVALID_PORT;
}

static SpaResult
spa_libva_enc_node_port_pull_output (SpaHandle      *handle,
                                     uint32_t        n_info,
                                     SpaOutputInfo  *info)
{
  SpaLibvaEnc *this = (SpaLibvaEnc *) handle;
  SpaLibvaState *state;
  uint32_t i;
  bool have_error = false;

  if (handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;


  for (i = 0; i < n_info; i++) {
    if (info[i].port_id != OUTPUT_PORT_ID) {
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
    info[i].status = SPA_RESULT_OK;
  }
  if (have_error)
    return SPA_RESULT_ERROR;

  return SPA_RESULT_OK;
}

static const SpaNode libva_enc_node = {
  sizeof (SpaNode),
  spa_libva_enc_node_get_props,
  spa_libva_enc_node_set_props,
  spa_libva_enc_node_send_command,
  spa_libva_enc_node_set_event_callback,
  spa_libva_enc_node_get_n_ports,
  spa_libva_enc_node_get_port_ids,
  spa_libva_enc_node_add_port,
  spa_libva_enc_node_remove_port,
  spa_libva_enc_node_port_enum_formats,
  spa_libva_enc_node_port_set_format,
  spa_libva_enc_node_port_get_format,
  spa_libva_enc_node_port_get_info,
  spa_libva_enc_node_port_get_props,
  spa_libva_enc_node_port_set_props,
  spa_libva_enc_node_port_use_buffers,
  spa_libva_enc_node_port_alloc_buffers,
  spa_libva_enc_node_port_get_status,
  spa_libva_enc_node_port_push_input,
  spa_libva_enc_node_port_pull_output,
};

static SpaResult
spa_libva_enc_get_interface (SpaHandle               *handle,
                             uint32_t                 interface_id,
                             const void             **interface)
{
  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  switch (interface_id) {
    case SPA_INTERFACE_ID_NODE:
      *interface = &libva_enc_node;
      break;
    default:
      return SPA_RESULT_UNKNOWN_INTERFACE;
  }
  return SPA_RESULT_OK;
}

SpaHandle *
spa_libva_enc_new (void)
{
  SpaHandle *handle;
  SpaLibvaEnc *this;

  handle = calloc (1, sizeof (SpaLibvaEnc));
  handle->get_interface = spa_libva_enc_get_interface;

  this = (SpaLibvaEnc *) handle;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  this->props[1].props.set_prop = spa_props_generic_set_prop;
  this->props[1].props.get_prop = spa_props_generic_get_prop;
  reset_libva_enc_props (&this->props[1]);

  this->state[INPUT_PORT_ID].info.flags = SPA_PORT_INFO_FLAG_NONE;
  this->state[INPUT_PORT_ID].status.flags = SPA_PORT_STATUS_FLAG_NONE;

  this->state[OUTPUT_PORT_ID].info.flags = SPA_PORT_INFO_FLAG_NONE;
  this->state[OUTPUT_PORT_ID].status.flags = SPA_PORT_STATUS_FLAG_NONE;
  return handle;
}
