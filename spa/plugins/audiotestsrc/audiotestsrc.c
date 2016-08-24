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

#include <stddef.h>
#include <string.h>

#include <spa/node.h>
#include <spa/audio/format.h>

typedef struct _SpaAudioTestSrc SpaAudioTestSrc;

typedef struct {
  SpaProps props;
  uint32_t wave;
  double freq;
  double volume;
} SpaAudioTestSrcProps;

struct _SpaAudioTestSrc {
  SpaHandle handle;
  SpaNode node;

  SpaAudioTestSrcProps props[2];

  SpaEventCallback event_cb;
  void *user_data;

  SpaPortInfo info;
  SpaPortStatus status;

  bool have_format;
  SpaAudioRawFormat query_format;
  SpaAudioRawFormat current_format;
};

static const uint32_t default_wave = 1.0;
static const double default_volume = 1.0;
static const double min_volume = 0.0;
static const double max_volume = 10.0;
static const double default_freq = 440.0;
static const double min_freq = 0.0;
static const double max_freq = 50000000.0;

static const SpaPropRangeInfo volume_range[] = {
  { "min", "Minimum value", sizeof (double), &min_volume },
  { "max", "Maximum value", sizeof (double), &max_volume },
};

static const uint32_t wave_val_sine = 0;
static const uint32_t wave_val_square = 1;

static const SpaPropRangeInfo wave_range[] = {
  { "sine", "Sine", sizeof (uint32_t), &wave_val_sine },
  { "square", "Square", sizeof (uint32_t), &wave_val_square },
};

static const SpaPropRangeInfo freq_range[] = {
  { "min", "Minimum value", sizeof (double), &min_freq },
  { "max", "Maximum value", sizeof (double), &max_freq },
};

enum {
  PROP_ID_WAVE,
  PROP_ID_FREQ,
  PROP_ID_VOLUME,
  PROP_ID_LAST,
};

static const SpaPropInfo prop_info[] =
{
  { PROP_ID_WAVE,              "wave", "Oscillator waveform",
                               SPA_PROP_FLAG_READWRITE,
                               SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                               sizeof (uint32_t), &default_wave,
                               SPA_PROP_RANGE_TYPE_ENUM, SPA_N_ELEMENTS (wave_range), wave_range,
                               NULL,
                               offsetof (SpaAudioTestSrcProps, wave) },
  { PROP_ID_FREQ,              "freq", "Frequency of test signal. The sample rate needs to be at least 4 times higher",
                               SPA_PROP_FLAG_READWRITE,
                               SPA_PROP_TYPE_DOUBLE, sizeof (double),
                               sizeof (double), &default_freq,
                               SPA_PROP_RANGE_TYPE_MIN_MAX, 2, freq_range,
                               NULL,
                               offsetof (SpaAudioTestSrcProps, freq) },
  { PROP_ID_VOLUME,            "volume", "The Volume factor",
                               SPA_PROP_FLAG_READWRITE,
                               SPA_PROP_TYPE_DOUBLE, sizeof (double),
                               sizeof (double), &default_volume,
                               SPA_PROP_RANGE_TYPE_MIN_MAX, 2, volume_range,
                               NULL,
                               offsetof (SpaAudioTestSrcProps, volume) },
};

static void
reset_audiotestsrc_props (SpaAudioTestSrcProps *props)
{
  props->wave = default_wave;
  props->freq = default_freq;
  props->volume = default_volume;
}

static SpaResult
spa_audiotestsrc_node_get_props (SpaNode       *node,
                                 SpaProps     **props)
{
  SpaAudioTestSrc *this;

  if (node == NULL || node->handle == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) node->handle;

  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));
  *props = &this->props[0].props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_set_props (SpaNode         *node,
                                 const SpaProps  *props)
{
  SpaAudioTestSrc *this;
  SpaAudioTestSrcProps *p;
  SpaResult res;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) node->handle;
  p = &this->props[1];

  if (props == NULL) {
    reset_audiotestsrc_props (p);
    return SPA_RESULT_OK;
  }
  res = spa_props_copy (props, &p->props);

  return res;
}

static SpaResult
spa_audiotestsrc_node_send_command (SpaNode       *node,
                                    SpaCommand    *command)
{
  SpaAudioTestSrc *this;

  if (node == NULL || node->handle == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) node->handle;

  switch (command->type) {
    case SPA_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_COMMAND_START:
      if (this->event_cb) {
        SpaEvent event;
        SpaEventStateChange sc;

        event.type = SPA_EVENT_TYPE_STATE_CHANGE;
        event.port_id = -1;
        event.data = &sc;
        event.size = sizeof (sc);
        sc.state = SPA_NODE_STATE_STREAMING;

        this->event_cb (node, &event, this->user_data);
      }
      break;

    case SPA_COMMAND_STOP:
      if (this->event_cb) {
        SpaEvent event;
        SpaEventStateChange sc;

        event.type = SPA_EVENT_TYPE_STATE_CHANGE;
        event.port_id = -1;
        event.data = &sc;
        event.size = sizeof (sc);
        sc.state = SPA_NODE_STATE_PAUSED;

        this->event_cb (node, &event, this->user_data);
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
spa_audiotestsrc_node_set_event_callback (SpaNode       *node,
                                          SpaEventCallback event,
                                          void          *user_data)
{
  SpaAudioTestSrc *this;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) node->handle;

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_get_n_ports (SpaNode       *node,
                                   unsigned int  *n_input_ports,
                                   unsigned int  *max_input_ports,
                                   unsigned int  *n_output_ports,
                                   unsigned int  *max_output_ports)
{
  if (node == NULL || node->handle == NULL)
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
spa_audiotestsrc_node_get_port_ids (SpaNode       *node,
                                    unsigned int   n_input_ports,
                                    uint32_t      *input_ids,
                                    unsigned int   n_output_ports,
                                    uint32_t      *output_ids)
{
  if (node == NULL || node->handle == NULL || input_ids == NULL || output_ids == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_output_ports > 0)
    output_ids[0] = 0;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_add_port (SpaNode        *node,
                                SpaDirection    direction,
                                uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_remove_port (SpaNode        *node,
                                   uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_port_enum_formats (SpaNode          *node,
                                         uint32_t          port_id,
                                         SpaFormat       **format,
                                         const SpaFormat  *filter,
                                         void            **state)
{
  SpaAudioTestSrc *this;
  int index;

  if (node == NULL || node->handle == NULL || format == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      spa_audio_raw_format_init (&this->query_format);
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = &this->query_format.format;
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_set_format (SpaNode            *node,
                                       uint32_t            port_id,
                                       SpaPortFormatFlags  flags,
                                       const SpaFormat    *format)
{
  SpaAudioTestSrc *this;
  SpaResult res;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  if (format == NULL) {
    this->have_format = false;
    return SPA_RESULT_OK;
  }

  if ((res = spa_audio_raw_format_parse (format, &this->current_format)) < 0)
    return res;

  this->have_format = true;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_get_format (SpaNode          *node,
                                       uint32_t          port_id,
                                       const SpaFormat **format)
{
  SpaAudioTestSrc *this;

  if (node == NULL || node->handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = &this->current_format.format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_get_info (SpaNode            *node,
                                     uint32_t            port_id,
                                     const SpaPortInfo **info)
{
  SpaAudioTestSrc *this;

  if (node == NULL || node->handle == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  *info = &this->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_get_props (SpaNode   *node,
                                      uint32_t   port_id,
                                      SpaProps **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_port_set_props (SpaNode        *node,
                                      uint32_t        port_id,
                                      const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_port_use_buffers (SpaNode         *node,
                                        uint32_t         port_id,
                                        SpaBuffer      **buffers,
                                        uint32_t         n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_port_alloc_buffers (SpaNode         *node,
                                          uint32_t         port_id,
                                          SpaAllocParam  **params,
                                          uint32_t         n_params,
                                          SpaBuffer      **buffers,
                                          uint32_t        *n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_port_reuse_buffer (SpaNode         *node,
                                         uint32_t         port_id,
                                         uint32_t         buffer_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_port_get_status (SpaNode              *node,
                                       uint32_t              port_id,
                                       const SpaPortStatus **status)
{
  SpaAudioTestSrc *this;

  if (node == NULL || node->handle == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  *status = &this->status;

  return SPA_RESULT_OK;
}


static SpaResult
spa_audiotestsrc_node_port_push_input (SpaNode       *node,
                                       unsigned int   n_info,
                                       SpaInputInfo  *info)
{
  return SPA_RESULT_INVALID_PORT;
}

static SpaResult
spa_audiotestsrc_node_port_pull_output (SpaNode       *node,
                                        unsigned int   n_info,
                                        SpaOutputInfo *info)
{
  SpaAudioTestSrc *this;
  size_t j, size;
  uint8_t *ptr;
  unsigned int i;
  bool have_error = false;

  if (node == NULL || node->handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) node->handle;

  for (i = 0; i < n_info; i++) {
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

    size = 0;

    for (j = 0; j < size; j++)
      ptr[j] = rand();

    info[i].status = SPA_RESULT_OK;
  }
  if (have_error)
    return SPA_RESULT_ERROR;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_push_event (SpaNode   *node,
                                       uint32_t   port_id,
                                       SpaEvent  *event)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static const SpaNode audiotestsrc_node = {
  NULL,
  sizeof (SpaNode),
  spa_audiotestsrc_node_get_props,
  spa_audiotestsrc_node_set_props,
  spa_audiotestsrc_node_send_command,
  spa_audiotestsrc_node_set_event_callback,
  spa_audiotestsrc_node_get_n_ports,
  spa_audiotestsrc_node_get_port_ids,
  spa_audiotestsrc_node_add_port,
  spa_audiotestsrc_node_remove_port,
  spa_audiotestsrc_node_port_enum_formats,
  spa_audiotestsrc_node_port_set_format,
  spa_audiotestsrc_node_port_get_format,
  spa_audiotestsrc_node_port_get_info,
  spa_audiotestsrc_node_port_get_props,
  spa_audiotestsrc_node_port_set_props,
  spa_audiotestsrc_node_port_use_buffers,
  spa_audiotestsrc_node_port_alloc_buffers,
  spa_audiotestsrc_node_port_reuse_buffer,
  spa_audiotestsrc_node_port_get_status,
  spa_audiotestsrc_node_port_push_input,
  spa_audiotestsrc_node_port_pull_output,
  spa_audiotestsrc_node_port_push_event,
};

static SpaResult
spa_audiotestsrc_get_interface (SpaHandle         *handle,
                                uint32_t           interface_id,
                                void             **interface)
{
  SpaAudioTestSrc *this;

  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) handle;

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
audiotestsrc_init (const SpaHandleFactory  *factory,
                   SpaHandle               *handle)
{
  SpaAudioTestSrc *this;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_audiotestsrc_get_interface;

  this = (SpaAudioTestSrc *) handle;
  this->node = audiotestsrc_node;
  this->node.handle = handle;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  reset_audiotestsrc_props (&this->props[1]);

  this->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                     SPA_PORT_INFO_FLAG_NO_REF;
  this->status.flags = SPA_PORT_STATUS_FLAG_HAVE_OUTPUT;

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo audiotestsrc_interfaces[] =
{
  { SPA_INTERFACE_ID_NODE,
    SPA_INTERFACE_ID_NODE_NAME,
    SPA_INTERFACE_ID_NODE_DESCRIPTION,
  },
};

static SpaResult
audiotestsrc_enum_interface_info (const SpaHandleFactory  *factory,
                                  const SpaInterfaceInfo **info,
                                  void			 **state)
{
  int index;

  if (factory == NULL || info == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      *info = &audiotestsrc_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *(int*)state = ++index;
  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_audiotestsrc_factory =
{ "audiotestsrc",
  NULL,
  sizeof (SpaAudioTestSrc),
  audiotestsrc_init,
  audiotestsrc_enum_interface_info,
};
