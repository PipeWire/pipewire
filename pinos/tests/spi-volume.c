/* Spi
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

#include <spi/node.h>

#include "spi-plugins.h"

typedef struct _SpiVolume SpiVolume;

typedef struct {
  SpiParams param;
  double volume;
  bool mute;
} SpiVolumeParams;

typedef struct {
  SpiParams param;
  char media_type[32];
  uint32_t unset_mask;
  char format[16];
  int32_t layout;
  int32_t samplerate;
  int32_t channels;
  int32_t position[16];
} SpiVolumeFormat;

struct _SpiVolume {
  SpiNode  node;

  SpiVolumeParams params;
  SpiVolumeParams tmp_params;

  SpiEventCallback event_cb;
  void *user_data;

  bool have_format;
  SpiVolumeFormat current_format;

  bool have_input;
  SpiBuffer *input_buffer;

  SpiData data;
};

static const double default_volume = 1.0;
static const double min_volume = 0.0;
static const double max_volume = 10.0;
static const bool default_mute = false;

static const SpiParamRangeInfo volume_range[] = {
  { "min", "Minimum value", sizeof (double), &min_volume },
  { "max", "Maximum value", sizeof (double), &max_volume },
  { NULL, NULL, 0, NULL }
};

enum {
  PARAM_ID_VOLUME,
  PARAM_ID_MUTE,
  PARAM_ID_LAST,
};

static const SpiParamInfo param_info[] =
{
  { PARAM_ID_VOLUME,            "volume", "The Volume factor",
                                SPI_PARAM_FLAG_READWRITE,
                                SPI_PARAM_TYPE_DOUBLE, sizeof (double),
                                sizeof (double), &default_volume,
                                SPI_PARAM_RANGE_TYPE_MIN_MAX, volume_range,
                                NULL,
                                NULL },
  { PARAM_ID_MUTE,              "mute", "Mute",
                                SPI_PARAM_FLAG_READWRITE,
                                SPI_PARAM_TYPE_BOOL, sizeof (bool),
                                sizeof (bool), &default_mute,
                                SPI_PARAM_RANGE_TYPE_NONE, NULL,
                                NULL,
                                NULL },
};

#define CHECK_TYPE(type,expected) if (type != expected) return SPI_RESULT_WRONG_PARAM_TYPE;
#define CHECK_SIZE(size,expected) if (size != expected) return SPI_RESULT_WRONG_PARAM_SIZE;
#define CHECK_SIZE_RANGE(size,minsize,maxsize) if (size > maxsize || size < minsize) return SPI_RESULT_WRONG_PARAM_SIZE;
#define CHECK_SIZE_MAX(size,maxsize) if (size > maxsize) return SPI_RESULT_WRONG_PARAM_SIZE;
#define CHECK_UNSET(mask,index) if (mask & (1 << index)) return SPI_RESULT_PARAM_UNSET;

static SpiResult
enum_param_info (const SpiParams     *params,
                 unsigned int         index,
                 const SpiParamInfo **info)
{
  if (params == NULL || info == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (index >= PARAM_ID_LAST)
    return SPI_RESULT_ENUM_END;

  *info = &param_info[index];

  return SPI_RESULT_OK;
}

static SpiResult
set_param (SpiParams    *params,
           uint32_t      id,
           SpiParamType  type,
           size_t        size,
           const void   *value)
{
  SpiResult res = SPI_RESULT_OK;
  SpiVolumeParams *p = (SpiVolumeParams *) params;

  if (params == NULL || value == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  switch (id) {
    case PARAM_ID_VOLUME:
      CHECK_TYPE (type, SPI_PARAM_TYPE_DOUBLE);
      CHECK_SIZE (size, sizeof (double));
      memcpy (&p->volume, value, size);
      break;
    case PARAM_ID_MUTE:
      CHECK_TYPE (type, SPI_PARAM_TYPE_BOOL);
      CHECK_SIZE (size, sizeof (bool));
      memcpy (&p->mute, value, size);
      break;
    default:
      res = SPI_RESULT_INVALID_PARAM_ID;
      break;
  }
  return res;
}

static SpiResult
get_param (const SpiParams *params,
           uint32_t         id,
           SpiParamType    *type,
           size_t          *size,
           const void     **value)
{
  SpiResult res = SPI_RESULT_OK;
  SpiVolumeParams *p = (SpiVolumeParams *) params;

  if (params == NULL || type == NULL || size == NULL || value == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  switch (id) {
    case PARAM_ID_VOLUME:
      *type = SPI_PARAM_TYPE_DOUBLE;
      *value = &p->volume;
      *size = sizeof (double);
      break;
    case PARAM_ID_MUTE:
      *type = SPI_PARAM_TYPE_BOOL;
      *value = &p->mute;
      *size = sizeof (bool);
      break;
    default:
      res = SPI_RESULT_INVALID_PARAM_ID;
      break;
  }
  return res;
}

static void
reset_volume_params (SpiVolumeParams *params)
{
  params->volume = default_volume;
  params->mute = default_mute;
}

static SpiResult
spi_volume_node_get_params (SpiNode        *node,
                            SpiParams     **params)
{
  SpiVolume *this = (SpiVolume *) node;

  if (node == NULL || params == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  memcpy (&this->tmp_params, &this->params, sizeof (this->tmp_params));
  *params = &this->tmp_params.param;

  return SPI_RESULT_OK;
}

static SpiResult
spi_volume_node_set_params (SpiNode          *node,
                            const SpiParams  *params)
{
  SpiVolume *this = (SpiVolume *) node;
  SpiVolumeParams *p = &this->params;
  SpiParamType type;
  size_t size;
  const void *value;

  if (node == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (params == NULL) {
    reset_volume_params (p);
    return SPI_RESULT_OK;
  }

  if (params->get_param (params, 0, &type, &size, &value) == 0) {
    CHECK_TYPE (type, SPI_PARAM_TYPE_DOUBLE);
    CHECK_SIZE (size, sizeof (double));
    memcpy (&p->volume, value, size);
  }
  if (params->get_param (params, 1, &type, &size, &value) == 0) {
    CHECK_TYPE (type, SPI_PARAM_TYPE_BOOL);
    CHECK_SIZE (size, sizeof (bool));
    memcpy (&p->mute, value, size);
  }
  return SPI_RESULT_OK;
}

static SpiResult
spi_volume_node_send_command (SpiNode       *node,
                              SpiCommand    *command)
{
  SpiVolume *this = (SpiVolume *) node;

  if (node == NULL || command == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  switch (command->type) {
    case SPI_COMMAND_INVALID:
      return SPI_RESULT_INVALID_COMMAND;

    case SPI_COMMAND_ACTIVATE:
      if (this->event_cb) {
        SpiEvent event;

        event.refcount = 1;
        event.notify = NULL;
        event.type = SPI_EVENT_TYPE_ACTIVATED;
        event.port_id = -1;
        event.data = NULL;
        event.size = 0;

        this->event_cb (node, &event, this->user_data);
      }
      break;

    case SPI_COMMAND_DEACTIVATE:
      if (this->event_cb) {
        SpiEvent event;

        event.refcount = 1;
        event.notify = NULL;
        event.type = SPI_EVENT_TYPE_DEACTIVATED;
        event.port_id = -1;
        event.data = NULL;
        event.size = 0;

        this->event_cb (node, &event, this->user_data);
      }
      break;

    case SPI_COMMAND_START:
    case SPI_COMMAND_STOP:
    case SPI_COMMAND_FLUSH:
    case SPI_COMMAND_DRAIN:
    case SPI_COMMAND_MARKER:
      return SPI_RESULT_NOT_IMPLEMENTED;
  }
  return SPI_RESULT_OK;
}

static SpiResult
spi_volume_node_set_event_callback (SpiNode          *node,
                                    SpiEventCallback  event,
                                    void             *user_data)
{
  SpiVolume *this = (SpiVolume *) node;

  if (node == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  this->event_cb = event;
  this->user_data = user_data;

  return SPI_RESULT_OK;
}

static SpiResult
spi_volume_node_get_n_ports (SpiNode       *node,
                             unsigned int  *n_input_ports,
                             unsigned int  *max_input_ports,
                             unsigned int  *n_output_ports,
                             unsigned int  *max_output_ports)
{
  if (node == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports)
    *n_input_ports = 1;
  if (n_output_ports)
    *n_output_ports = 1;
  if (max_input_ports)
    *max_input_ports = 1;
  if (max_output_ports)
    *max_output_ports = 1;

  return SPI_RESULT_OK;
}

static SpiResult
spi_volume_node_get_port_ids (SpiNode       *node,
                              unsigned int   n_input_ports,
                              uint32_t      *input_ids,
                              unsigned int   n_output_ports,
                              uint32_t      *output_ids)
{
  if (node == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports > 0 && input_ids)
    input_ids[0] = 0;
  if (n_output_ports > 0 && output_ids)
    output_ids[0] = 1;

  return SPI_RESULT_OK;
}


static SpiResult
spi_volume_node_add_port (SpiNode        *node,
                          SpiDirection    direction,
                          uint32_t       *port_id)
{
  return SPI_RESULT_NOT_IMPLEMENTED;
}

static SpiResult
spi_volume_node_remove_port (SpiNode        *node,
                             uint32_t        port_id)
{
  return SPI_RESULT_NOT_IMPLEMENTED;
}

static const SpiParamRangeInfo format_format_range[] = {
  { "S8", "S8", 2, "S8" },
  { "U8", "U8", 2, "U8" },
  { "S16LE", "S16LE", 5, "S16LE" },
  { "S16BE", "S16BE", 5, "S16BE" },
  { "U16LE", "U16LE", 5, "U16LE" },
  { "U16BE", "U16BE", 5, "U16BE" },
  { "S24_32LE", "S24_32LE", 8, "S24_32LE" },
  { "S24_32BE", "S24_32BE", 8, "S24_32BE" },
  { "U24_32LE", "U24_32LE", 8, "U24_32LE" },
  { "U24_32BE", "U24_32BE", 8, "U24_32BE" },
  { "S32LE", "S32LE", 5, "S32LE" },
  { "S32BE", "S32BE", 5, "S32BE" },
  { "U32LE", "U32LE", 5, "U32LE" },
  { "U32BE", "U32BE", 5, "U32BE" },
  { "S24LE", "S24LE", 5, "S24LE" },
  { "S24BE", "S24BE", 5, "S24BE" },
  { "U24LE", "U24LE", 5, "U24LE" },
  { "U24BE", "U24BE", 5, "U24BE" },
  { "S20LE", "S20LE", 5, "S20LE" },
  { "S20BE", "S20BE", 5, "S20BE" },
  { "U20LE", "U20LE", 5, "U20LE" },
  { "U20BE", "U20BE", 5, "U20BE" },
  { "S18LE", "S18LE", 5, "S18LE" },
  { "S18BE", "S18BE", 5, "S18BE" },
  { "U18LE", "U18LE", 5, "U18LE" },
  { "U18BE", "U18BE", 5, "U18BE" },
  { "F32LE", "F32LE", 5, "F32LE" },
  { "F32BE", "F32BE", 5, "F32BE" },
  { "F64LE", "F64LE", 5, "F64LE" },
  { "F64BE", "F64BE", 5, "F64BE" },
  { NULL, NULL, 0, NULL }
};

static const uint32_t min_uint32 = 1;
static const uint32_t max_uint32 = UINT32_MAX;

static const SpiParamRangeInfo int32_range[] = {
  { "min", "Minimum value", 4, &min_uint32 },
  { "max", "Maximum value", 4, &max_uint32 },
  { NULL, NULL, 0, NULL }
};

enum {
  SPI_PARAM_ID_INVALID,
  SPI_PARAM_ID_MEDIA_TYPE,
  SPI_PARAM_ID_FORMAT,
  SPI_PARAM_ID_LAYOUT,
  SPI_PARAM_ID_SAMPLERATE,
  SPI_PARAM_ID_CHANNELS,
};

static const int32_t format_default_layout = 1;

static const SpiParamInfo raw_format_param_info[] =
{
  { SPI_PARAM_ID_MEDIA_TYPE, "media-type", "The media type",
                             SPI_PARAM_FLAG_READABLE,
                             SPI_PARAM_TYPE_STRING, 32,
                             12, "audio/x-raw",
                             SPI_PARAM_RANGE_TYPE_NONE, NULL,
                             NULL,
                             NULL },
  { SPI_PARAM_ID_FORMAT,     "format", "The media format",
                             SPI_PARAM_FLAG_READWRITE,
                             SPI_PARAM_TYPE_STRING, 16,
                             0, NULL,
                             SPI_PARAM_RANGE_TYPE_ENUM, format_format_range,
                             NULL,
                             NULL },
  { SPI_PARAM_ID_LAYOUT,     "layout", "Sample Layout",
                             SPI_PARAM_FLAG_READABLE,
                             SPI_PARAM_TYPE_UINT32, sizeof (uint32_t),
                             sizeof (uint32_t), &format_default_layout,
                             SPI_PARAM_RANGE_TYPE_NONE, NULL,
                             NULL,
                             NULL },
  { SPI_PARAM_ID_SAMPLERATE, "rate", "Audio sample rate",
                             SPI_PARAM_FLAG_READWRITE,
                             SPI_PARAM_TYPE_UINT32, sizeof (uint32_t),
                             0, NULL,
                             SPI_PARAM_RANGE_TYPE_MIN_MAX, int32_range,
                             NULL,
                             NULL },
  { SPI_PARAM_ID_CHANNELS,   "channels", "Audio channels",
                             SPI_PARAM_FLAG_READWRITE,
                             SPI_PARAM_TYPE_UINT32, sizeof (uint32_t),
                             0, NULL,
                             SPI_PARAM_RANGE_TYPE_MIN_MAX, int32_range,
                             NULL,
                             NULL },
};

static SpiResult
enum_raw_format_param_info (const SpiParams     *params,
                            unsigned int         index,
                            const SpiParamInfo **info)
{
  if (params == NULL || info == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (index >= 4)
    return SPI_RESULT_ENUM_END;

  *info = &raw_format_param_info[index];

  return SPI_RESULT_OK;
}

#define CHECK_TYPE(type,expected) if (type != expected) return SPI_RESULT_WRONG_PARAM_TYPE;
#define MARK_SET(mask,index)        (mask &= ~(1 << index))

static SpiResult
set_format_param (SpiParams    *params,
                  uint32_t      id,
                  SpiParamType  type,
                  size_t        size,
                  const void   *value)
{
  SpiVolumeFormat *f = (SpiVolumeFormat *) params;

  if (params == NULL || value == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  switch (id) {
    case SPI_PARAM_ID_FORMAT:
      CHECK_TYPE (type, SPI_PARAM_TYPE_STRING);
      CHECK_SIZE_MAX (size, 16);
      memcpy (f->format, value, size);
      MARK_SET (f->unset_mask, 1);
      break;
    case SPI_PARAM_ID_LAYOUT:
      CHECK_TYPE (type, SPI_PARAM_TYPE_UINT32);
      CHECK_SIZE (size, sizeof (uint32_t));
      memcpy (&f->layout, value, size);
      MARK_SET (f->unset_mask, 2);
      break;
    case SPI_PARAM_ID_SAMPLERATE:
      CHECK_TYPE (type, SPI_PARAM_TYPE_UINT32);
      CHECK_SIZE (size, sizeof (uint32_t));
      memcpy (&f->samplerate, value, size);
      MARK_SET (f->unset_mask, 3);
      break;
    case SPI_PARAM_ID_CHANNELS:
      CHECK_TYPE (type, SPI_PARAM_TYPE_UINT32);
      CHECK_SIZE (size, sizeof (uint32_t));
      memcpy (&f->channels, value, size);
      MARK_SET (f->unset_mask, 4);
      break;
    default:
      return SPI_RESULT_INVALID_PARAM_ID;
  }

  return SPI_RESULT_OK;
}



static SpiResult
get_format_param (const SpiParams *params,
                  uint32_t         id,
                  SpiParamType    *type,
                  size_t          *size,
                  const void     **value)
{
  SpiVolumeFormat *f = (SpiVolumeFormat *) params;

  if (params == NULL || type == NULL || size == NULL || value == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  switch (id) {
    case SPI_PARAM_ID_MEDIA_TYPE:
      CHECK_UNSET (f->unset_mask, 0);
      *type = SPI_PARAM_TYPE_STRING;
      *value = f->media_type;
      *size = strlen (f->media_type);
      break;
    case SPI_PARAM_ID_FORMAT:
      CHECK_UNSET (f->unset_mask, 1);
      *type = SPI_PARAM_TYPE_STRING;
      *value = f->format;
      *size = strlen (f->format);
      break;
    case SPI_PARAM_ID_LAYOUT:
      CHECK_UNSET (f->unset_mask, 2);
      *type = SPI_PARAM_TYPE_UINT32;
      *value = &f->layout;
      *size = 4;
      break;
    case SPI_PARAM_ID_SAMPLERATE:
      CHECK_UNSET (f->unset_mask, 3);
      *type = SPI_PARAM_TYPE_UINT32;
      *value = &f->samplerate;
      *size = 4;
      break;
    case SPI_PARAM_ID_CHANNELS:
      CHECK_UNSET (f->unset_mask, 4);
      *type = SPI_PARAM_TYPE_UINT32;
      *value = &f->channels;
      *size = 4;
      break;
    default:
      return SPI_RESULT_INVALID_PARAM_ID;
  }
  return SPI_RESULT_OK;
}


static SpiResult
spi_volume_node_enum_port_formats (SpiNode          *node,
                                   uint32_t          port_id,
                                   unsigned int      index,
                                   SpiParams       **format)
{
  static SpiVolumeFormat fmt;

  if (node == NULL || format == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPI_RESULT_INVALID_PORT;

  switch (index) {
    case 0:
      strcpy (fmt.media_type, "audio/x-raw");
      fmt.unset_mask = (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4);
      fmt.param.enum_param_info = enum_raw_format_param_info;
      fmt.param.set_param = set_format_param;
      fmt.param.get_param = get_format_param;
      break;
    default:
      return SPI_RESULT_ENUM_END;
  }
  *format = &fmt.param;

  return SPI_RESULT_OK;
}

static SpiResult
spi_volume_node_set_port_format (SpiNode         *node,
                                 uint32_t         port_id,
                                 int              test_only,
                                 const SpiParams *format)
{
  SpiVolume *this = (SpiVolume *) node;
  SpiParamType type;
  size_t size;
  const void *value;
  SpiVolumeFormat *fmt;

  if (node == NULL || format == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPI_RESULT_INVALID_PORT;

  fmt = &this->current_format;

  if (format == NULL) {
    fmt->param.get_param = NULL;
    this->have_format = false;
    return SPI_RESULT_OK;
  }

  if (format->get_param (format,
                         SPI_PARAM_ID_MEDIA_TYPE,
                         &type, &size, &value) < 0)
    return SPI_RESULT_INVALID_MEDIA_TYPE;
  CHECK_TYPE (type, SPI_PARAM_TYPE_STRING);
  CHECK_SIZE_MAX (size, 32);
  memcpy (fmt->media_type, value, size);

  if (format->get_param (format,
                         SPI_PARAM_ID_FORMAT,
                         &type, &size, &value) < 0)
    return SPI_RESULT_INVALID_FORMAT_PARAMS;
  CHECK_TYPE (type, SPI_PARAM_TYPE_STRING);
  CHECK_SIZE_MAX (size, 16);
  memcpy (fmt->format, value, size);

  if (format->get_param (format,
                         SPI_PARAM_ID_LAYOUT,
                         &type, &size, &value) < 0)
    return SPI_RESULT_INVALID_FORMAT_PARAMS;
  CHECK_TYPE (type, SPI_PARAM_TYPE_UINT32);
  CHECK_SIZE (size, sizeof (uint32_t));
  memcpy (&fmt->layout, value, size);

  if (format->get_param (format,
                         SPI_PARAM_ID_SAMPLERATE,
                         &type, &size, &value) < 0)
    return SPI_RESULT_INVALID_FORMAT_PARAMS;
  CHECK_TYPE (type, SPI_PARAM_TYPE_UINT32);
  CHECK_SIZE (size, sizeof (uint32_t));
  memcpy (&fmt->samplerate, value, size);

  if (format->get_param (format,
                         SPI_PARAM_ID_CHANNELS,
                         &type, &size, &value) < 0)
    return SPI_RESULT_INVALID_FORMAT_PARAMS;
  CHECK_TYPE (type, SPI_PARAM_TYPE_UINT32);
  CHECK_SIZE (size, sizeof (uint32_t));
  memcpy (&fmt->channels, value, size);

  fmt->param.enum_param_info = enum_raw_format_param_info;
  fmt->param.set_param = NULL;
  fmt->param.get_param = get_format_param;
  this->have_format = true;

  return SPI_RESULT_OK;
}

static SpiResult
spi_volume_node_get_port_format (SpiNode          *node,
                                 uint32_t          port_id,
                                 const SpiParams **format)
{
  SpiVolume *this = (SpiVolume *) node;

  if (node == NULL || format == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPI_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPI_RESULT_NO_FORMAT;

  *format = &this->current_format.param;

  return SPI_RESULT_OK;
}

static SpiResult
spi_volume_node_get_port_info (SpiNode       *node,
                               uint32_t       port_id,
                               SpiPortInfo   *info)
{
  if (node == NULL || info == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  switch (port_id) {
    case 0:
      info->flags = SPI_PORT_INFO_FLAG_CAN_USE_BUFFER |
                    SPI_PORT_INFO_FLAG_IN_PLACE;
      break;
    case 1:
      info->flags = SPI_PORT_INFO_FLAG_CAN_GIVE_BUFFER |
                    SPI_PORT_INFO_FLAG_CAN_USE_BUFFER |
                    SPI_PORT_INFO_FLAG_NO_REF;
      break;
    default:
      return SPI_RESULT_INVALID_PORT;
  }
  return SPI_RESULT_OK;
}

static SpiResult
spi_volume_node_get_port_params (SpiNode    *node,
                                 uint32_t    port_id,
                                 SpiParams **params)
{
  return SPI_RESULT_NOT_IMPLEMENTED;
}

static SpiResult
spi_volume_node_set_port_params (SpiNode         *node,
                                 uint32_t         port_id,
                                 const SpiParams *params)
{
  return SPI_RESULT_NOT_IMPLEMENTED;
}

static SpiResult
spi_volume_node_get_port_status (SpiNode        *node,
                                 uint32_t        port_id,
                                 SpiPortStatus  *status)
{
  SpiVolume *this = (SpiVolume *) node;
  SpiPortStatusFlags flags = 0;

  if (node == NULL || status == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (!this->have_format)
    return SPI_RESULT_NO_FORMAT;

  switch (port_id) {
    case 0:
      if (this->input_buffer == NULL)
        flags |= SPI_PORT_STATUS_FLAG_NEED_INPUT;
      break;
    case 1:
      if (this->input_buffer != NULL)
        flags |= SPI_PORT_STATUS_FLAG_HAVE_OUTPUT;
      break;
    default:
      return SPI_RESULT_INVALID_PORT;
  }
  status->flags = flags;

  return SPI_RESULT_OK;
}

static SpiResult
spi_volume_node_push_port_input (SpiNode        *node,
                                 unsigned int    n_info,
                                 SpiInputInfo   *info)
{
  SpiVolume *this = (SpiVolume *) node;
  SpiBuffer *buffer;
  SpiEvent *event;
  unsigned int i;
  bool have_error = false;
  bool have_enough = false;

  if (node == NULL || n_info == 0 || info == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  for (i = 0; i < n_info; i++) {
    if (info[i].port_id != 0) {
      info[i].status = SPI_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }

    event = info[i].event;
    buffer = info[i].buffer;

    if (buffer == NULL && event == NULL) {
      info[i].status = SPI_RESULT_INVALID_ARGUMENTS;
      have_error = true;
      continue;
    }

    if (buffer) {
      if (!this->have_format) {
        info[i].status = SPI_RESULT_NO_FORMAT;
        have_error = true;
        continue;
      }

      if (this->input_buffer != NULL) {
        info[i].status = SPI_RESULT_HAVE_ENOUGH_INPUT;
        have_enough = true;
        continue;
      }
      this->input_buffer = spi_buffer_ref (buffer);
    }
    if (event) {
      switch (event->type) {
        default:
          break;
      }
    }
    info[i].status = SPI_RESULT_OK;
  }
  if (have_error)
    return SPI_RESULT_ERROR;
  if (have_enough)
    return SPI_RESULT_HAVE_ENOUGH_INPUT;

  return SPI_RESULT_OK;
}

#define MIN(a,b) ((a) < (b) ? (a) : (b))

static SpiResult
spi_volume_node_pull_port_output (SpiNode        *node,
                                  unsigned int    n_info,
                                  SpiOutputInfo  *info)
{
  SpiVolume *this = (SpiVolume *) node;
  unsigned int si, di, i, n_samples, n_bytes, soff, doff ;
  SpiBuffer *sbuf, *dbuf;
  SpiData *sd, *dd;
  uint16_t *src, *dst;
  double volume;

  if (node == NULL || n_info == 0 || info == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (info->port_id != 1)
    return SPI_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPI_RESULT_NO_FORMAT;

  if (this->input_buffer == NULL)
    return SPI_RESULT_NEED_MORE_INPUT;

  volume = this->params.volume;

  sbuf = this->input_buffer;
  dbuf = info->buffer ? info->buffer : this->input_buffer;

  si = di = 0;
  soff = doff = 0;

  while (true) {
    if (si == sbuf->n_datas || di == dbuf->n_datas)
      break;

    sd = &sbuf->datas[si];
    dd = &dbuf->datas[di];

    if (sd->type != SPI_DATA_TYPE_MEMPTR) {
      si++;
      continue;
    }
    if (dd->type != SPI_DATA_TYPE_MEMPTR) {
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
    spi_buffer_unref (sbuf);

  this->input_buffer = NULL;
  info->buffer = dbuf;

  return SPI_RESULT_OK;
}

static SpiResult
spi_volume_node_enum_interface_info (SpiNode                  *node,
                                     unsigned int             index,
                                     const SpiInterfaceInfo **info)

{
  return SPI_RESULT_NOT_IMPLEMENTED;
}

static SpiResult
spi_volume_node_get_interface (SpiNode                 *node,
                               uint32_t                 interface_id,
                               void                   **interface)
{
  return SPI_RESULT_NOT_IMPLEMENTED;
}

SpiNode *
spi_volume_new (void)
{
  SpiNode *node;
  SpiVolume *this;

  node = calloc (1, sizeof (SpiVolume));

  node->get_params = spi_volume_node_get_params;
  node->set_params = spi_volume_node_set_params;
  node->send_command = spi_volume_node_send_command;
  node->set_event_callback = spi_volume_node_set_event_callback;
  node->get_n_ports = spi_volume_node_get_n_ports;
  node->get_port_ids = spi_volume_node_get_port_ids;
  node->add_port = spi_volume_node_add_port;
  node->remove_port = spi_volume_node_remove_port;
  node->enum_port_formats = spi_volume_node_enum_port_formats;
  node->set_port_format = spi_volume_node_set_port_format;
  node->get_port_format = spi_volume_node_get_port_format;
  node->get_port_info = spi_volume_node_get_port_info;
  node->get_port_params = spi_volume_node_get_port_params;
  node->set_port_params = spi_volume_node_set_port_params;
  node->get_port_status = spi_volume_node_get_port_status;
  node->push_port_input = spi_volume_node_push_port_input;
  node->pull_port_output = spi_volume_node_pull_port_output;
  node->enum_interface_info = spi_volume_node_enum_interface_info;
  node->get_interface = spi_volume_node_get_interface;

  this = (SpiVolume *) node;
  this->params.param.enum_param_info = enum_param_info;
  this->params.param.set_param = set_param;
  this->params.param.get_param = get_param;
  reset_volume_params (&this->params);

  return node;
}
