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
} SpaVolumePort;

struct _SpaVolume {
  SpaHandle  handle;

  SpaVolumeProps props[2];

  SpaEventCallback event_cb;
  void *user_data;

  bool have_format;
  SpaAudioRawFormat query_format;
  SpaAudioRawFormat current_format;

  SpaVolumePort ports[2];
  SpaBuffer *input_buffer;
};

static const double default_volume = 1.0;
static const double min_volume = 0.0;
static const double max_volume = 10.0;
static const bool default_mute = false;

static const SpaPropRangeInfo volume_range[] = {
  { "min", "Minimum value", sizeof (double), &min_volume },
  { "max", "Maximum value", sizeof (double), &max_volume },
};

enum {
  PROP_ID_VOLUME,
  PROP_ID_MUTE,
  PROP_ID_LAST,
};

static const SpaPropInfo prop_info[] =
{
  { PROP_ID_VOLUME,            "volume", "The Volume factor",
                               SPA_PROP_FLAG_READWRITE,
                               SPA_PROP_TYPE_DOUBLE, sizeof (double),
                               sizeof (double), &default_volume,
                               SPA_PROP_RANGE_TYPE_MIN_MAX, 2, volume_range,
                               NULL,
                               offsetof (SpaVolumeProps, volume),
                               0, 0,
                               NULL },
  { PROP_ID_MUTE,              "mute", "Mute",
                               SPA_PROP_FLAG_READWRITE,
                               SPA_PROP_TYPE_BOOL, sizeof (bool),
                               sizeof (bool), &default_mute,
                               SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                               NULL,
                               offsetof (SpaVolumeProps, mute),
                               0, 0,
                               NULL },
};

static void
reset_volume_props (SpaVolumeProps *props)
{
  props->volume = default_volume;
  props->mute = default_mute;
}

static SpaResult
spa_volume_node_get_props (SpaHandle      *handle,
                            SpaProps     **props)
{
  SpaVolume *this = (SpaVolume *) handle;

  if (handle == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));
  *props = &this->props[0].props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_set_props (SpaHandle        *handle,
                            const SpaProps  *props)
{
  SpaVolume *this = (SpaVolume *) handle;
  SpaVolumeProps *p = &this->props[1];
  SpaResult res;

  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (props == NULL) {
    reset_volume_props (p);
    return SPA_RESULT_OK;
  }
  res = spa_props_copy (props, &p->props);

  return res;
}

static SpaResult
spa_volume_node_send_command (SpaHandle     *handle,
                              SpaCommand    *command)
{
  SpaVolume *this = (SpaVolume *) handle;

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
spa_volume_node_set_event_callback (SpaHandle        *handle,
                                    SpaEventCallback  event,
                                    void             *user_data)
{
  SpaVolume *this = (SpaVolume *) handle;

  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_get_n_ports (SpaHandle     *handle,
                             unsigned int  *n_input_ports,
                             unsigned int  *max_input_ports,
                             unsigned int  *n_output_ports,
                             unsigned int  *max_output_ports)
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
spa_volume_node_get_port_ids (SpaHandle     *handle,
                              unsigned int   n_input_ports,
                              uint32_t      *input_ids,
                              unsigned int   n_output_ports,
                              uint32_t      *output_ids)
{
  if (handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports > 0 && input_ids)
    input_ids[0] = 0;
  if (n_output_ports > 0 && output_ids)
    output_ids[0] = 1;

  return SPA_RESULT_OK;
}


static SpaResult
spa_volume_node_add_port (SpaHandle      *handle,
                          SpaDirection    direction,
                          uint32_t       *port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_remove_port (SpaHandle      *handle,
                             uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_enum_port_formats (SpaHandle        *handle,
                                   uint32_t          port_id,
                                   unsigned int      index,
                                   SpaFormat       **format)
{
  SpaVolume *this = (SpaVolume *) handle;

  if (handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  switch (index) {
    case 0:
      spa_audio_raw_format_init (&this->query_format);
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = &this->query_format.format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_set_port_format (SpaHandle       *handle,
                                 uint32_t         port_id,
                                 bool             test_only,
                                 const SpaFormat *format)
{
  SpaVolume *this = (SpaVolume *) handle;
  SpaResult res;

  if (handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id >= 2)
    return SPA_RESULT_INVALID_PORT;

  if (format == NULL) {
    this->ports[port_id].have_format = false;
    return SPA_RESULT_OK;
  }

  if ((res = spa_audio_raw_format_parse (format, &this->current_format)) < 0)
    return res;

  this->ports[port_id].have_format = true;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_get_port_format (SpaHandle        *handle,
                                 uint32_t          port_id,
                                 const SpaFormat **format)
{
  SpaVolume *this = (SpaVolume *) handle;

  if (handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id >= 2)
    return SPA_RESULT_INVALID_PORT;

  if (!this->ports[port_id].have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = &this->current_format.format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_get_port_info (SpaHandle          *handle,
                               uint32_t            port_id,
                               const SpaPortInfo **info)
{
  SpaVolume *this = (SpaVolume *) handle;

  if (handle == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id >= 2)
    return SPA_RESULT_INVALID_PORT;

  *info = &this->ports[port_id].info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_get_port_props (SpaHandle  *handle,
                                 uint32_t    port_id,
                                 SpaProps **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_set_port_props (SpaHandle       *handle,
                                 uint32_t         port_id,
                                 const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_volume_node_get_port_status (SpaHandle            *handle,
                                 uint32_t              port_id,
                                 const SpaPortStatus **status)
{
  SpaVolume *this = (SpaVolume *) handle;

  if (handle == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id >= 2)
    return SPA_RESULT_INVALID_PORT;

  if (!this->ports[port_id].have_format)
    return SPA_RESULT_NO_FORMAT;

  *status = &this->ports[port_id].status;

  return SPA_RESULT_OK;
}

static SpaResult
spa_volume_node_push_port_input (SpaHandle      *handle,
                                 unsigned int    n_info,
                                 SpaInputInfo   *info)
{
  SpaVolume *this = (SpaVolume *) handle;
  SpaBuffer *buffer;
  SpaEvent *event;
  unsigned int i;
  bool have_error = false;
  bool have_enough = false;

  if (handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  for (i = 0; i < n_info; i++) {
    if (info[i].port_id != 0) {
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }

    event = info[i].event;
    buffer = info[i].buffer;

    if (buffer == NULL && event == NULL) {
      info[i].status = SPA_RESULT_INVALID_ARGUMENTS;
      have_error = true;
      continue;
    }

    if (buffer) {
      if (!this->ports[0].have_format) {
        info[i].status = SPA_RESULT_NO_FORMAT;
        have_error = true;
        continue;
      }

      if (this->input_buffer != NULL) {
        info[i].status = SPA_RESULT_HAVE_ENOUGH_INPUT;
        have_enough = true;
        continue;
      }
      this->input_buffer = spa_buffer_ref (buffer);

      this->ports[0].status.flags &= ~SPA_PORT_STATUS_FLAG_NEED_INPUT;
      this->ports[1].status.flags |= SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;
    }
    if (event) {
      switch (event->type) {
        default:
          break;
      }
    }
    info[i].status = SPA_RESULT_OK;
  }
  if (have_error)
    return SPA_RESULT_ERROR;
  if (have_enough)
    return SPA_RESULT_HAVE_ENOUGH_INPUT;

  return SPA_RESULT_OK;
}

#define MIN(a,b) ((a) < (b) ? (a) : (b))

static SpaResult
spa_volume_node_pull_port_output (SpaHandle      *handle,
                                  unsigned int    n_info,
                                  SpaOutputInfo  *info)
{
  SpaVolume *this = (SpaVolume *) handle;
  unsigned int si, di, i, n_samples, n_bytes, soff, doff ;
  SpaBuffer *sbuf, *dbuf;
  SpaData *sd, *dd;
  uint16_t *src, *dst;
  double volume;

  if (handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (info->port_id != 1)
    return SPA_RESULT_INVALID_PORT;

  if (!this->ports[1].have_format)
    return SPA_RESULT_NO_FORMAT;

  if (this->input_buffer == NULL)
    return SPA_RESULT_NEED_MORE_INPUT;

  volume = this->props[1].volume;

  sbuf = this->input_buffer;
  dbuf = info->buffer ? info->buffer : this->input_buffer;

  si = di = 0;
  soff = doff = 0;

  while (true) {
    if (si == sbuf->n_datas || di == dbuf->n_datas)
      break;

    sd = &sbuf->datas[si];
    dd = &dbuf->datas[di];

    if (sd->type != SPA_DATA_TYPE_MEMPTR) {
      si++;
      continue;
    }
    if (dd->type != SPA_DATA_TYPE_MEMPTR) {
      di++;
      continue;
    }
    src = (uint16_t*) ((uint8_t*)sd->data + soff);
    dst = (uint16_t*) ((uint8_t*)dd->data + doff);

    n_bytes = MIN (sd->size - soff, dd->size - doff);
    n_samples = n_bytes / sizeof (uint16_t);

    for (i = 0; i < n_samples; i++)
      *src++ = *dst++ * volume;

    soff += n_bytes;
    doff += n_bytes;

    if (soff >= sd->size) {
      si++;
      soff = 0;
    }
    if (doff >= dd->size) {
      di++;
      doff = 0;
    }
  }

  if (sbuf != dbuf)
    spa_buffer_unref (sbuf);

  this->input_buffer = NULL;
  info->buffer = dbuf;

  this->ports[0].status.flags |= SPA_PORT_STATUS_FLAG_NEED_INPUT;
  this->ports[1].status.flags &= ~SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;

  return SPA_RESULT_OK;
}

static const SpaNode volume_node = {
  sizeof (SpaNode),
  spa_volume_node_get_props,
  spa_volume_node_set_props,
  spa_volume_node_send_command,
  spa_volume_node_set_event_callback,
  spa_volume_node_get_n_ports,
  spa_volume_node_get_port_ids,
  spa_volume_node_add_port,
  spa_volume_node_remove_port,
  spa_volume_node_enum_port_formats,
  spa_volume_node_set_port_format,
  spa_volume_node_get_port_format,
  spa_volume_node_get_port_info,
  spa_volume_node_get_port_props,
  spa_volume_node_set_port_props,
  spa_volume_node_get_port_status,
  spa_volume_node_push_port_input,
  spa_volume_node_pull_port_output,
};

static SpaResult
spa_volume_get_interface (SpaHandle               *handle,
                          uint32_t                 interface_id,
                          const void             **interface)
{
  if (handle == NULL || interface == 0)
    return SPA_RESULT_INVALID_ARGUMENTS;

  switch (interface_id) {
    case SPA_INTERFACE_ID_NODE:
      *interface = &volume_node;
      break;
    default:
      return SPA_RESULT_UNKNOWN_INTERFACE;

  }
  return SPA_RESULT_OK;
}

SpaHandle *
spa_volume_new (void)
{
  SpaHandle *handle;
  SpaVolume *this;

  handle = calloc (1, sizeof (SpaVolume));
  handle->get_interface = spa_volume_get_interface;

  this = (SpaVolume *) handle;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  this->props[1].props.set_prop = spa_props_generic_set_prop;
  this->props[1].props.get_prop = spa_props_generic_get_prop;
  reset_volume_props (&this->props[1]);

  this->ports[0].info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFER |
                              SPA_PORT_INFO_FLAG_IN_PLACE;
  this->ports[1].info.flags = SPA_PORT_INFO_FLAG_CAN_GIVE_BUFFER |
                              SPA_PORT_INFO_FLAG_CAN_USE_BUFFER |
                              SPA_PORT_INFO_FLAG_NO_REF;

  this->ports[0].status.flags = SPA_PORT_STATUS_FLAG_NEED_INPUT;
  this->ports[1].status.flags = SPA_PORT_STATUS_FLAG_NONE;

  return handle;
}
