/* Spa
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

#include <string.h>
#include <stddef.h>

#include <spa/node.h>
#include <spa/memory.h>
#include <spa/audio/format.h>

typedef struct _SpaVolume SpaVolume;

typedef struct {
  SpaProps props;
  double volume;
  bool mute;
} SpaVolumeProps;

typedef struct {
  bool have_format;
  SpaPortInfo info;
  SpaPortStatus status;
  SpaBuffer **buffers;
  unsigned int n_buffers;
  SpaBuffer *buffer;
} SpaVolumePort;

struct _SpaVolume {
  SpaHandle  handle;
  SpaNode  node;

  SpaVolumeProps props[2];

  SpaNodeEventCallback event_cb;
  void *user_data;

  bool have_format;
  SpaFormatAudio query_format;
  SpaFormatAudio current_format;

  SpaVolumePort ports[2];
};

static const double default_volume = 1.0;
static const double min_volume = 0.0;
static const double max_volume = 10.0;
static const bool default_mute = false;

static const SpaPropRangeInfo volume_range[] = {
  { "min", "Minimum value", { sizeof (double), &min_volume } },
  { "max", "Maximum value", { sizeof (double), &max_volume } },
};

enum {
  PROP_ID_VOLUME,
  PROP_ID_MUTE,
  PROP_ID_LAST,
};

static const SpaPropInfo prop_info[] =
{
  { PROP_ID_VOLUME,            offsetof (SpaVolumeProps, volume),
                               "volume", "The Volume factor",
                               SPA_PROP_FLAG_READWRITE,
                               SPA_PROP_TYPE_DOUBLE, sizeof (double),
                               SPA_PROP_RANGE_TYPE_MIN_MAX, 2, volume_range,
                               NULL },
  { PROP_ID_MUTE,              offsetof (SpaVolumeProps, mute),
                               "mute", "Mute",
                               SPA_PROP_FLAG_READWRITE,
                               SPA_PROP_TYPE_BOOL, sizeof (bool),
                               SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                               NULL },
};

static void
reset_volume_props (SpaVolumeProps *props)
{
  props->volume = default_volume;
  props->mute = default_mute;
}

static SpaResult
spa_volume_node_get_props (SpaNode        *node,
                            SpaProps     **props)
{
  SpaVolume *this;

  if (node == NULL || node->handle == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVolume *) node->handle;

  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));
  *props = &this->props[0].props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_set_props (SpaNode          *node,
                            const SpaProps  *props)
{
  SpaVolume *this;
  SpaVolumeProps *p;
  SpaResult res;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVolume *) node->handle;
  p = &this->props[1];

  if (props == NULL) {
    reset_volume_props (p);
    return SPA_RESULT_OK;
  }
  res = spa_props_copy (props, &p->props);

  return res;
}

static SpaResult
spa_volume_node_send_command (SpaNode        *node,
                              SpaNodeCommand *command)
{
  SpaVolume *this;

  if (node == NULL || node->handle == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVolume *) node->handle;

  switch (command->type) {
    case SPA_NODE_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_NODE_COMMAND_START:
      if (this->event_cb) {
        SpaNodeEvent event;
        SpaNodeEventStateChange sc;

        event.type = SPA_NODE_EVENT_TYPE_STATE_CHANGE;
        event.data = &sc;
        event.size = sizeof (sc);
        sc.state = SPA_NODE_STATE_STREAMING;

        this->event_cb (node, &event, this->user_data);
      }
      break;

    case SPA_NODE_COMMAND_PAUSE:
      if (this->event_cb) {
        SpaNodeEvent event;
        SpaNodeEventStateChange sc;

        event.type = SPA_NODE_EVENT_TYPE_STATE_CHANGE;
        event.data = &sc;
        event.size = sizeof (sc);
        sc.state = SPA_NODE_STATE_PAUSED;

        this->event_cb (node, &event, this->user_data);
      }
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
spa_volume_node_set_event_callback (SpaNode              *node,
                                    SpaNodeEventCallback  event,
                                    void                 *user_data)
{
  SpaVolume *this;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVolume *) node->handle;

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_get_n_ports (SpaNode       *node,
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
spa_volume_node_get_port_ids (SpaNode       *node,
                              unsigned int   n_input_ports,
                              uint32_t      *input_ids,
                              unsigned int   n_output_ports,
                              uint32_t      *output_ids)
{
  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports > 0 && input_ids)
    input_ids[0] = 0;
  if (n_output_ports > 0 && output_ids)
    output_ids[0] = 1;

  return SPA_RESULT_OK;
}


static SpaResult
spa_volume_node_add_port (SpaNode        *node,
                          uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_remove_port (SpaNode        *node,
                             uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_port_enum_formats (SpaNode          *node,
                                   uint32_t          port_id,
                                   SpaFormat       **format,
                                   const SpaFormat  *filter,
                                   void            **state)
{
  SpaVolume *this;
  int index;

  if (node == NULL || node->handle == NULL || format == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVolume *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      spa_format_audio_init (SPA_MEDIA_TYPE_AUDIO,
                             SPA_MEDIA_SUBTYPE_RAW,
                             &this->query_format);
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = &this->query_format.format;
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_set_format (SpaNode            *node,
                                 uint32_t            port_id,
                                 SpaPortFormatFlags  flags,
                                 const SpaFormat    *format)
{
  SpaVolume *this;
  SpaVolumePort *port;
  SpaResult res;

  if (node == NULL || node->handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVolume *) node->handle;

  if (port_id >= 2)
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];

  if (format == NULL) {
    port->have_format = false;
    return SPA_RESULT_OK;
  }

  if ((res = spa_format_audio_parse (format, &this->current_format)) < 0)
    return res;

  port->have_format = true;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_get_format (SpaNode          *node,
                                 uint32_t          port_id,
                                 const SpaFormat **format)
{
  SpaVolume *this;
  SpaVolumePort *port;

  if (node == NULL || node->handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVolume *) node->handle;

  if (port_id >= 2)
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = &this->current_format.format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_get_info (SpaNode            *node,
                               uint32_t            port_id,
                               const SpaPortInfo **info)
{
  SpaVolume *this;
  SpaVolumePort *port;

  if (node == NULL || node->handle == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVolume *) node->handle;

  if (port_id >= 2)
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];
  *info = &port->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_get_props (SpaNode    *node,
                                 uint32_t    port_id,
                                 SpaProps **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_port_set_props (SpaNode         *node,
                                 uint32_t         port_id,
                                 const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_port_use_buffers (SpaNode         *node,
                                  uint32_t         port_id,
                                  SpaBuffer      **buffers,
                                  uint32_t         n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_port_alloc_buffers (SpaNode         *node,
                                    uint32_t         port_id,
                                    SpaAllocParam  **params,
                                    uint32_t         n_params,
                                    SpaBuffer      **buffers,
                                    uint32_t        *n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_port_reuse_buffer (SpaNode         *node,
                                   uint32_t         port_id,
                                   uint32_t         buffer_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_port_get_status (SpaNode              *node,
                                 uint32_t              port_id,
                                 const SpaPortStatus **status)
{
  SpaVolume *this;
  SpaVolumePort *port;

  if (node == NULL || node->handle == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVolume *) node->handle;

  if (port_id >= 2)
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[port_id];

  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  *status = &port->status;

  return SPA_RESULT_OK;
}


static SpaResult
spa_volume_node_port_push_input (SpaNode          *node,
                                 unsigned int      n_info,
                                 SpaPortInputInfo *info)
{
  SpaVolume *this;
  unsigned int i;
  bool have_error = false;
  bool have_enough = false;

  if (node == NULL || node->handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVolume *) node->handle;

  for (i = 0; i < n_info; i++) {
    SpaVolumePort *port;
    SpaBuffer *buffer;

    if (info[i].port_id != 0) {
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }

    port = &this->ports[info[i].port_id];
    buffer = port->buffers[info[i].buffer_id];

    if (buffer == NULL) {
      info[i].status = SPA_RESULT_INVALID_ARGUMENTS;
      have_error = true;
      continue;
    }

    if (buffer) {
      if (!port->have_format) {
        info[i].status = SPA_RESULT_NO_FORMAT;
        have_error = true;
        continue;
      }

      if (port->buffer != NULL) {
        info[i].status = SPA_RESULT_HAVE_ENOUGH_INPUT;
        have_enough = true;
        continue;
      }
      port->buffer = buffer;

      this->ports[0].status.flags &= ~SPA_PORT_STATUS_FLAG_NEED_INPUT;
      this->ports[1].status.flags |= SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;
    }
    info[i].status = SPA_RESULT_OK;
  }
  if (have_error)
    return SPA_RESULT_ERROR;
  if (have_enough)
    return SPA_RESULT_HAVE_ENOUGH_INPUT;

  return SPA_RESULT_OK;
}

static SpaBuffer *
find_free_buffer (SpaVolume *this, SpaVolumePort *port)
{
  return NULL;
}

static void
release_buffer (SpaVolume *this, SpaBuffer *buffer)
{
  SpaNodeEvent event;
  SpaNodeEventReuseBuffer rb;

  event.type = SPA_NODE_EVENT_TYPE_REUSE_BUFFER;
  event.data = &rb;
  event.size = sizeof (rb);
  rb.port_id = 0;
  rb.buffer_id = buffer->id;
  this->event_cb (&this->node, &event, this->user_data);
}

static SpaResult
spa_volume_node_port_pull_output (SpaNode           *node,
                                  unsigned int       n_info,
                                  SpaPortOutputInfo *info)
{
  SpaVolume *this;
  SpaVolumePort *port;
  unsigned int si, di, i, n_samples, n_bytes, soff, doff ;
  SpaBuffer *sbuf, *dbuf;
  SpaData *sd, *dd;
  uint16_t *src, *dst;
  double volume;
  SpaMemory *sm, *dm;

  if (node == NULL || node->handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVolume *) node->handle;

  if (info->port_id != 1)
    return SPA_RESULT_INVALID_PORT;

  port = &this->ports[info[0].port_id];
  if (!port->have_format)
    return SPA_RESULT_NO_FORMAT;

  if (this->ports[0].buffer == NULL)
    return SPA_RESULT_NEED_MORE_INPUT;

  volume = this->props[1].volume;

  sbuf = this->ports[0].buffer;
  dbuf = find_free_buffer (this, port);

  si = di = 0;
  soff = doff = 0;

  while (true) {
    if (si == sbuf->n_datas || di == dbuf->n_datas)
      break;

    sd = &SPA_BUFFER_DATAS (sbuf)[si];
    dd = &SPA_BUFFER_DATAS (dbuf)[di];

    sm = spa_memory_find (&sd->mem.mem);
    dm = spa_memory_find (&dd->mem.mem);

    src = (uint16_t*) ((uint8_t*)sm->ptr + sd->mem.offset + soff);
    dst = (uint16_t*) ((uint8_t*)dm->ptr + dd->mem.offset + doff);

    n_bytes = SPA_MIN (sd->mem.size - soff, dd->mem.size - doff);
    n_samples = n_bytes / sizeof (uint16_t);

    for (i = 0; i < n_samples; i++)
      *src++ = *dst++ * volume;

    soff += n_bytes;
    doff += n_bytes;

    if (soff >= sd->mem.size) {
      si++;
      soff = 0;
    }
    if (doff >= dd->mem.size) {
      di++;
      doff = 0;
    }
  }

  if (sbuf != dbuf)
    release_buffer (this, sbuf);

  this->ports[0].buffer = NULL;
  info->buffer_id = dbuf->id;

  this->ports[0].status.flags |= SPA_PORT_STATUS_FLAG_NEED_INPUT;
  this->ports[1].status.flags &= ~SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_port_push_event (SpaNode      *node,
                                 uint32_t      port_id,
                                 SpaNodeEvent *event)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static const SpaNode volume_node = {
  NULL,
  sizeof (SpaNode),
  SPA_NODE_STATE_INIT,
  spa_volume_node_get_props,
  spa_volume_node_set_props,
  spa_volume_node_send_command,
  spa_volume_node_set_event_callback,
  spa_volume_node_get_n_ports,
  spa_volume_node_get_port_ids,
  spa_volume_node_add_port,
  spa_volume_node_remove_port,
  spa_volume_node_port_enum_formats,
  spa_volume_node_port_set_format,
  spa_volume_node_port_get_format,
  spa_volume_node_port_get_info,
  spa_volume_node_port_get_props,
  spa_volume_node_port_set_props,
  spa_volume_node_port_use_buffers,
  spa_volume_node_port_alloc_buffers,
  spa_volume_node_port_reuse_buffer,
  spa_volume_node_port_get_status,
  spa_volume_node_port_push_input,
  spa_volume_node_port_pull_output,
  spa_volume_node_port_push_event,
};

static SpaResult
spa_volume_get_interface (SpaHandle               *handle,
                          uint32_t                 interface_id,
                          void                   **interface)
{
  SpaVolume *this;

  if (handle == NULL || interface == 0)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVolume *) handle;

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
volume_clear (SpaHandle *handle)
{
  return SPA_RESULT_OK;
}

static SpaResult
volume_init (const SpaHandleFactory  *factory,
             SpaHandle               *handle,
             const void              *config)
{
  SpaVolume *this;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_volume_get_interface;
  handle->clear = volume_clear;

  this = (SpaVolume *) handle;
  this->node = volume_node;
  this->node.handle = handle;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  reset_volume_props (&this->props[1]);

  this->ports[0].info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                              SPA_PORT_INFO_FLAG_IN_PLACE;
  this->ports[1].info.flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS |
                              SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                              SPA_PORT_INFO_FLAG_NO_REF;

  this->ports[0].status.flags = SPA_PORT_STATUS_FLAG_NEED_INPUT;
  this->ports[1].status.flags = SPA_PORT_STATUS_FLAG_NONE;

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo volume_interfaces[] =
{
  { SPA_INTERFACE_ID_NODE,
    SPA_INTERFACE_ID_NODE_NAME,
    SPA_INTERFACE_ID_NODE_DESCRIPTION,
  },
};

static SpaResult
volume_enum_interface_info (const SpaHandleFactory  *factory,
                            const SpaInterfaceInfo **info,
                            void                   **state)
{
  int index;

  if (factory == NULL || info == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      *info = &volume_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_volume_factory =
{ "volume",
  NULL,
  sizeof (SpaVolume),
  volume_init,
  volume_enum_interface_info,
};
