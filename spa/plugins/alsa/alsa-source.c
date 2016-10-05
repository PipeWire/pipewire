/* Spa ALSA Source
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

#include <asoundlib.h>

#include <spa/node.h>
#include <spa/queue.h>
#include <spa/audio/format.h>

#include "alsa-utils.h"

#define CHECK_PORT(this,d,p)    ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)

typedef struct _SpaALSAState SpaALSASource;

static void
update_state (SpaALSASource *this, SpaNodeState state)
{
  this->node.state = state;
}

static const char default_device[] = "hw:0";
static const uint32_t default_buffer_time = 100000;
static const uint32_t default_period_time = 10000;
static const bool default_period_event = 0;

static void
reset_alsa_props (SpaALSAProps *props)
{
  strncpy (props->device, default_device, 64);
  props->buffer_time = default_buffer_time;
  props->period_time = default_period_time;
  props->period_event = default_period_event;
  props->props.unset_mask = 0xf;
}

static const uint32_t min_uint32 = 1;
static const uint32_t max_uint32 = UINT32_MAX;

static const SpaPropRangeInfo uint32_range[] = {
  { "min", "Minimum value", { 4, &min_uint32 } },
  { "max", "Maximum value", { 4, &max_uint32 } },
};

enum {
  PROP_ID_DEVICE,
  PROP_ID_DEVICE_NAME,
  PROP_ID_CARD_NAME,
  PROP_ID_BUFFER_TIME,
  PROP_ID_PERIOD_TIME,
  PROP_ID_PERIOD_EVENT,
  PROP_ID_LAST,
};

static const SpaPropInfo prop_info[] =
{
  { PROP_ID_DEVICE,             offsetof (SpaALSAProps, device),
                                "device", "ALSA device, as defined in an asound configuration file",
                                SPA_PROP_FLAG_READWRITE,
                                SPA_PROP_TYPE_STRING, 63,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
  { PROP_ID_DEVICE_NAME,        offsetof (SpaALSAProps, device_name),
                                "device-name", "Human-readable name of the sound device",
                                SPA_PROP_FLAG_READABLE,
                                SPA_PROP_TYPE_STRING, 127,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
  { PROP_ID_CARD_NAME,          offsetof (SpaALSAProps, card_name),
                                "card-name", "Human-readable name of the sound card",
                                SPA_PROP_FLAG_READABLE,
                                SPA_PROP_TYPE_STRING, 127,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
  { PROP_ID_BUFFER_TIME,        offsetof (SpaALSAProps, buffer_time),
                                "buffer-time", "The total size of the buffer in time",
                                SPA_PROP_FLAG_READWRITE,
                                SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                SPA_PROP_RANGE_TYPE_MIN_MAX, 2, uint32_range,
                                NULL },
  { PROP_ID_PERIOD_TIME,        offsetof (SpaALSAProps, period_time),
                                "period-time", "The size of a period in time",
                                SPA_PROP_FLAG_READWRITE,
                                SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                SPA_PROP_RANGE_TYPE_MIN_MAX, 2, uint32_range,
                                NULL },
  { PROP_ID_PERIOD_EVENT,       offsetof (SpaALSAProps, period_event),
                                "period-event", "Generate an event each period",
                                SPA_PROP_FLAG_READWRITE,
                                SPA_PROP_TYPE_BOOL, sizeof (bool),
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
};

static SpaResult
spa_alsa_source_node_get_props (SpaNode       *node,
                                SpaProps     **props)
{
  SpaALSASource *this;

  if (node == NULL || node->handle == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASource *) node->handle;

  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));
  *props = &this->props[0].props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_set_props (SpaNode         *node,
                              const SpaProps  *props)
{
  SpaALSASource *this;
  SpaALSAProps *p;
  SpaResult res;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASource *) node->handle;
  p = &this->props[1];

  if (props == NULL) {
    reset_alsa_props (p);
    return SPA_RESULT_OK;
  }

  res = spa_props_copy_values (props, &p->props);

  return res;
}

static SpaResult
spa_alsa_source_node_send_command (SpaNode        *node,
                                 SpaNodeCommand *command)
{
  SpaALSASource *this;

  if (node == NULL || node->handle == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASource *) node->handle;

  switch (command->type) {
    case SPA_NODE_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_NODE_COMMAND_START:
      if (spa_alsa_start (this) < 0)
        return SPA_RESULT_ERROR;

      update_state (this, SPA_NODE_STATE_STREAMING);
      break;
    case SPA_NODE_COMMAND_PAUSE:
      if (spa_alsa_stop (this) < 0)
        return SPA_RESULT_ERROR;

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
spa_alsa_source_node_set_event_callback (SpaNode              *node,
                                       SpaNodeEventCallback  event,
                                       void                 *user_data)
{
  SpaALSASource *this;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASource *) node->handle;

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_get_n_ports (SpaNode       *node,
                                unsigned int  *n_input_ports,
                                unsigned int  *max_input_ports,
                                unsigned int  *n_output_ports,
                                unsigned int  *max_output_ports)
{
  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports)
    *n_input_ports = 0;
  if (max_input_ports)
    *max_input_ports = 0;
  if (n_output_ports)
    *n_output_ports = 1;
  if (max_output_ports)
    *max_output_ports = 1;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_get_port_ids (SpaNode       *node,
                                   unsigned int   n_input_ports,
                                   uint32_t      *input_ids,
                                   unsigned int   n_output_ports,
                                   uint32_t      *output_ids)
{
  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_output_ports > 0 && output_ids != NULL)
    output_ids[0] = 0;

  return SPA_RESULT_OK;
}


static SpaResult
spa_alsa_source_node_add_port (SpaNode        *node,
                               SpaDirection    direction,
                               uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_node_remove_port (SpaNode        *node,
                                  SpaDirection    direction,
                                  uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_node_port_enum_formats (SpaNode         *node,
                                        SpaDirection     direction,
                                        uint32_t         port_id,
                                        SpaFormat      **format,
                                        const SpaFormat *filter,
                                        void           **state)
{
  SpaALSASource *this;
  int index;

  if (node == NULL || node->handle == NULL || format == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASource *) node->handle;

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      spa_format_audio_init (SPA_MEDIA_TYPE_AUDIO,
                             SPA_MEDIA_SUBTYPE_RAW,
                             &this->query_format);
      break;
    case 1:
      spa_format_audio_init (SPA_MEDIA_TYPE_AUDIO,
                             SPA_MEDIA_SUBTYPE_AAC,
                             &this->query_format);
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = &this->query_format.format;
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

static void
recycle_buffer (SpaALSASource *this, uint32_t buffer_id)
{
  SpaALSABuffer *b;

  b = &this->buffers[buffer_id];
  if (!b->outstanding)
    return;

  b->outstanding = false;
  b->next = NULL;
  SPA_QUEUE_PUSH_TAIL (&this->free, SpaALSABuffer, next, b);
}

static SpaResult
spa_alsa_clear_buffers (SpaALSASource *this)
{
  if (this->n_buffers > 0) {
    SPA_QUEUE_INIT (&this->free);
    SPA_QUEUE_INIT (&this->ready);
    this->n_buffers = 0;
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_port_set_format (SpaNode            *node,
                                      SpaDirection        direction,
                                      uint32_t            port_id,
                                      SpaPortFormatFlags  flags,
                                      const SpaFormat    *format)
{
  SpaALSASource *this;
  SpaResult res;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASource *) node->handle;

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (format == NULL) {
    this->have_format = false;
    spa_alsa_clear_buffers (this);
    update_state (this, SPA_NODE_STATE_CONFIGURE);
    return SPA_RESULT_OK;
  }

  if ((res = spa_format_audio_parse (format, &this->current_format)) < 0)
    return res;

  if (spa_alsa_set_format (this, &this->current_format, flags) < 0)
    return SPA_RESULT_ERROR;

  this->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                     SPA_PORT_INFO_FLAG_LIVE;
  this->info.maxbuffering = this->buffer_frames * this->frame_size;
  this->info.latency = (this->period_frames * SPA_NSEC_PER_SEC) / this->rate;
  this->info.n_params = 2;
  this->info.params = this->params;
  this->params[0] = &this->param_buffers.param;
  this->param_buffers.param.type = SPA_ALLOC_PARAM_TYPE_BUFFERS;
  this->param_buffers.param.size = sizeof (this->param_buffers);
  this->param_buffers.minsize = this->period_frames * this->frame_size;
  this->param_buffers.stride = 0;
  this->param_buffers.min_buffers = 1;
  this->param_buffers.max_buffers = 32;
  this->param_buffers.align = 16;
  this->params[1] = &this->param_meta.param;
  this->param_meta.param.type = SPA_ALLOC_PARAM_TYPE_META_ENABLE;
  this->param_meta.param.size = sizeof (this->param_meta);
  this->param_meta.type = SPA_META_TYPE_HEADER;
  this->info.features = NULL;

  this->have_format = true;

  update_state (this, SPA_NODE_STATE_READY);

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_port_get_format (SpaNode          *node,
                                      SpaDirection      direction,
                                      uint32_t          port_id,
                                      const SpaFormat **format)
{
  SpaALSASource *this;

  if (node == NULL || node->handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASource *) node->handle;

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = &this->current_format.format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_port_get_info (SpaNode            *node,
                                    SpaDirection        direction,
                                    uint32_t            port_id,
                                    const SpaPortInfo **info)
{
  SpaALSASource *this;

  if (node == NULL || node->handle == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASource *) node->handle;

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  *info = &this->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_port_get_props (SpaNode       *node,
                                     SpaDirection   direction,
                                     uint32_t       port_id,
                                     SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_node_port_set_props (SpaNode         *node,
                                     SpaDirection     direction,
                                     uint32_t         port_id,
                                     const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_node_port_use_buffers (SpaNode         *node,
                                       SpaDirection     direction,
                                       uint32_t         port_id,
                                       SpaBuffer      **buffers,
                                       uint32_t         n_buffers)
{
  SpaALSASource *this;
  SpaResult res;
  int i;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  this = (SpaALSASource *) node->handle;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  if (this->n_buffers > 0) {
    if ((res = spa_alsa_clear_buffers (this)) < 0)
      return res;
  }
  for (i = 0; i < n_buffers; i++) {
    SpaALSABuffer *b = &this->buffers[i];
    b->outbuf = buffers[i];
    b->outstanding = false;

    b->h = spa_buffer_find_meta (b->outbuf, SPA_META_TYPE_HEADER);

    switch (buffers[i]->datas[0].type) {
      case SPA_DATA_TYPE_MEMFD:
      case SPA_DATA_TYPE_DMABUF:
      case SPA_DATA_TYPE_MEMPTR:
        if (buffers[i]->datas[0].data == NULL) {
          fprintf (stderr, "alsa-source: need mapped memory\n");
          continue;
        }
        break;
      default:
        break;
    }
    b->next = NULL;
    SPA_QUEUE_PUSH_TAIL (&this->free, SpaALSABuffer, next, b);
  }
  this->n_buffers = n_buffers;

  if (this->n_buffers > 0)
    update_state (this, SPA_NODE_STATE_PAUSED);
  else
    update_state (this, SPA_NODE_STATE_READY);

  return SPA_RESULT_OK;
}


static SpaResult
spa_alsa_source_node_port_alloc_buffers (SpaNode         *node,
                                         SpaDirection     direction,
                                         uint32_t         port_id,
                                         SpaAllocParam  **params,
                                         uint32_t         n_params,
                                         SpaBuffer      **buffers,
                                         uint32_t        *n_buffers)
{
  SpaALSASource *this;

  if (node == NULL || node->handle == NULL || buffers == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  this = (SpaALSASource *) node->handle;

  if (this->n_buffers == 0)
    return SPA_RESULT_NO_FORMAT;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_node_port_get_status (SpaNode              *node,
                                      SpaDirection          direction,
                                      uint32_t              port_id,
                                      const SpaPortStatus **status)
{
  SpaALSASource *this;

  if (node == NULL || node->handle == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASource *) node->handle;

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  *status = &this->status;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_port_push_input (SpaNode          *node,
                                      unsigned int      n_info,
                                      SpaPortInputInfo *info)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_node_port_pull_output (SpaNode           *node,
                                       unsigned int       n_info,
                                       SpaPortOutputInfo *info)
{
  SpaALSASource *this;
  unsigned int i;
  bool have_error = false;

  if (node == NULL || node->handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASource *) node->handle;

  for (i = 0; i < n_info; i++) {
    SpaALSABuffer *b;

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

    SPA_QUEUE_POP_HEAD (&this->ready, SpaALSABuffer, next, b);
    if (b == NULL) {
      info[i].status = SPA_RESULT_UNEXPECTED;
      have_error = true;
      continue;
    }
    b->outstanding = true;

    info[i].buffer_id = b->outbuf->id;
    info[i].status = SPA_RESULT_OK;
  }
  if (have_error)
    return SPA_RESULT_ERROR;

  return SPA_RESULT_OK;

}

static SpaResult
spa_alsa_source_node_port_reuse_buffer (SpaNode         *node,
                                        uint32_t         port_id,
                                        uint32_t         buffer_id)
{
  SpaALSASource *this;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASource *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  if (this->n_buffers == 0)
    return SPA_RESULT_NO_BUFFERS;

  if (buffer_id >= this->n_buffers)
    return SPA_RESULT_INVALID_BUFFER_ID;

  recycle_buffer (this, buffer_id);

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_source_node_port_push_event (SpaNode          *node,
                                      SpaDirection      direction,
                                      uint32_t          port_id,
                                      SpaNodeEvent     *event)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static const SpaNode alsasource_node = {
  NULL,
  sizeof (SpaNode),
  NULL,
  SPA_NODE_STATE_INIT,
  spa_alsa_source_node_get_props,
  spa_alsa_source_node_set_props,
  spa_alsa_source_node_send_command,
  spa_alsa_source_node_set_event_callback,
  spa_alsa_source_node_get_n_ports,
  spa_alsa_source_node_get_port_ids,
  spa_alsa_source_node_add_port,
  spa_alsa_source_node_remove_port,
  spa_alsa_source_node_port_enum_formats,
  spa_alsa_source_node_port_set_format,
  spa_alsa_source_node_port_get_format,
  spa_alsa_source_node_port_get_info,
  spa_alsa_source_node_port_get_props,
  spa_alsa_source_node_port_set_props,
  spa_alsa_source_node_port_use_buffers,
  spa_alsa_source_node_port_alloc_buffers,
  spa_alsa_source_node_port_get_status,
  spa_alsa_source_node_port_push_input,
  spa_alsa_source_node_port_pull_output,
  spa_alsa_source_node_port_reuse_buffer,
  spa_alsa_source_node_port_push_event,
};

static SpaResult
spa_alsa_source_clock_get_props (SpaClock  *clock,
                                 SpaProps **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_clock_set_props (SpaClock       *clock,
                                 const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_source_clock_get_time (SpaClock         *clock,
                                int32_t          *rate,
                                int64_t          *ticks,
                                int64_t          *monotonic_time)
{
  SpaALSASource *this;

  if (clock == NULL || clock->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASource *) clock->handle;

  if (rate)
    *rate = SPA_USEC_PER_SEC;
  if (ticks)
    *ticks = this->last_ticks;
  if (monotonic_time)
    *monotonic_time = this->last_monotonic;

  return SPA_RESULT_OK;
}

static const SpaClock alsasource_clock = {
  NULL,
  sizeof (SpaClock),
  NULL,
  SPA_CLOCK_STATE_STOPPED,
  spa_alsa_source_clock_get_props,
  spa_alsa_source_clock_set_props,
  spa_alsa_source_clock_get_time,
};

static SpaResult
spa_alsa_source_get_interface (SpaHandle               *handle,
                             uint32_t                 interface_id,
                             void                   **interface)
{
  SpaALSASource *this;

  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASource *) handle;

  switch (interface_id) {
    case SPA_INTERFACE_ID_NODE:
      *interface = &this->node;
      break;
    case SPA_INTERFACE_ID_CLOCK:
      *interface = &this->clock;
      break;
    default:
      return SPA_RESULT_UNKNOWN_INTERFACE;
  }
  return SPA_RESULT_OK;
}

static SpaResult
alsa_source_clear (SpaHandle *handle)
{
  return SPA_RESULT_OK;
}

static SpaResult
alsa_source_init (const SpaHandleFactory  *factory,
                  SpaHandle               *handle,
                  const SpaDict           *info,
                  const SpaInterface     **platform,
                  unsigned int             n_platform)
{
  SpaALSASource *this;
  unsigned int i;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_alsa_source_get_interface;
  handle->clear = alsa_source_clear;

  this = (SpaALSASource *) handle;
  this->node = alsasource_node;
  this->node.handle = handle;
  this->clock = alsasource_clock;
  this->clock.handle = handle;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  this->stream = SND_PCM_STREAM_CAPTURE;
  reset_alsa_props (&this->props[1]);

  this->status.flags = SPA_PORT_STATUS_FLAG_NEED_INPUT;

  for (i = 0; info && i < info->n_items; i++) {
    if (!strcmp (info->items[i].key, "alsa.card")) {
      snprintf (this->props[1].device, 63, "hw:%s", info->items[i].value);
      this->props[1].props.unset_mask &= ~1;
    }
  }

  update_state (this, SPA_NODE_STATE_CONFIGURE);

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo alsa_source_interfaces[] =
{
  { SPA_INTERFACE_ID_NODE,
    SPA_INTERFACE_ID_NODE_NAME,
    SPA_INTERFACE_ID_NODE_DESCRIPTION,
  },
  { SPA_INTERFACE_ID_CLOCK,
    SPA_INTERFACE_ID_CLOCK_NAME,
    SPA_INTERFACE_ID_CLOCK_DESCRIPTION,
  },
};

static SpaResult
alsa_source_enum_interface_info (const SpaHandleFactory  *factory,
                                 const SpaInterfaceInfo **info,
                                 void                   **state)
{
  int index;

  if (factory == NULL || info == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  index = (*state == NULL ? 0 : *(int*)state);

  if (index < 0 || index >= SPA_N_ELEMENTS (alsa_source_interfaces))
    return SPA_RESULT_ENUM_END;

  *info = &alsa_source_interfaces[index];

  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_alsa_source_factory =
{ "alsa-source",
  NULL,
  sizeof (SpaALSASource),
  alsa_source_init,
  alsa_source_enum_interface_info,
};
