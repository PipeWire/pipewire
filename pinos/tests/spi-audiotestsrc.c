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

typedef struct _SpiAudioTestSrc SpiAudioTestSrc;

typedef struct {
  SpiParams param;
  uint32_t wave;
  double freq;
  double volume;
} SpiAudioTestSrcParams;

typedef struct {
  SpiParams param;
  char media_type[32];
  uint32_t unset_mask;
  char format[16];
  int32_t layout;
  int32_t samplerate;
  int32_t channels;
  int32_t position[16];
} SpiAudioTestSrcFormat;

struct _SpiAudioTestSrc {
  SpiNode  node;

  SpiAudioTestSrcParams params;
  SpiAudioTestSrcParams tmp_params;

  SpiEvent *event;
  SpiEvent last_event;

  SpiEventCallback event_cb;
  void *user_data;

  bool have_format;
  SpiAudioTestSrcFormat current_format;

  bool have_input;
  SpiBuffer *input_buffer;

  SpiData data;
};

static const uint32_t default_wave = 1.0;
static const double default_volume = 1.0;
static const double min_volume = 0.0;
static const double max_volume = 10.0;
static const double default_freq = 440.0;
static const double min_freq = 0.0;
static const double max_freq = 50000000.0;

static const SpiParamRangeInfo volume_range[] = {
  { "min", "Minimum value", sizeof (double), &min_volume },
  { "max", "Maximum value", sizeof (double), &max_volume },
  { NULL, NULL, 0, NULL }
};

static const uint32_t wave_val_sine = 0;
static const uint32_t wave_val_square = 1;

static const SpiParamRangeInfo wave_range[] = {
  { "sine", "Sine", sizeof (uint32_t), &wave_val_sine },
  { "square", "Square", sizeof (uint32_t), &wave_val_square },
  { NULL, NULL, 0, NULL }
};

static const SpiParamRangeInfo freq_range[] = {
  { "min", "Minimum value", sizeof (double), &min_freq },
  { "max", "Maximum value", sizeof (double), &max_freq },
  { NULL, NULL, 0, NULL }
};

enum {
  PARAM_ID_WAVE,
  PARAM_ID_FREQ,
  PARAM_ID_VOLUME,
  PARAM_ID_LAST,
};

static const SpiParamInfo param_info[] =
{
  { PARAM_ID_WAVE,              "wave", "Oscillator waveform",
                                SPI_PARAM_FLAG_READWRITE,
                                SPI_PARAM_TYPE_UINT32, sizeof (uint32_t),
                                sizeof (uint32_t), &default_wave,
                                SPI_PARAM_RANGE_TYPE_ENUM, wave_range,
                                NULL,
                                NULL },
  { PARAM_ID_FREQ,              "freq", "Frequency of test signal. The sample rate needs to be at least 4 times higher",
                                SPI_PARAM_FLAG_READWRITE,
                                SPI_PARAM_TYPE_DOUBLE, sizeof (double),
                                sizeof (double), &default_freq,
                                SPI_PARAM_RANGE_TYPE_MIN_MAX, freq_range,
                                NULL,
                                NULL },
  { PARAM_ID_VOLUME,            "volume", "The Volume factor",
                                SPI_PARAM_FLAG_READWRITE,
                                SPI_PARAM_TYPE_DOUBLE, sizeof (double),
                                sizeof (double), &default_volume,
                                SPI_PARAM_RANGE_TYPE_MIN_MAX, volume_range,
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
  SpiAudioTestSrcParams *p = (SpiAudioTestSrcParams *) params;

  if (params == NULL || value == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  switch (id) {
    case PARAM_ID_WAVE:
      CHECK_TYPE (type, SPI_PARAM_TYPE_UINT32);
      CHECK_SIZE (size, sizeof (uint32_t));
      memcpy (&p->wave, value, size);
      break;
    case PARAM_ID_FREQ:
      CHECK_TYPE (type, SPI_PARAM_TYPE_DOUBLE);
      CHECK_SIZE (size, sizeof (double));
      memcpy (&p->freq, value, size);
      break;
    case PARAM_ID_VOLUME:
      CHECK_TYPE (type, SPI_PARAM_TYPE_DOUBLE);
      CHECK_SIZE (size, sizeof (double));
      memcpy (&p->volume, value, size);
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
  SpiAudioTestSrcParams *p = (SpiAudioTestSrcParams *) params;

  if (params == NULL || type == NULL || size == NULL || value == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  switch (id) {
    case PARAM_ID_WAVE:
      *type = SPI_PARAM_TYPE_UINT32;
      *value = &p->wave;
      *size = sizeof (uint32_t);
      break;
    case PARAM_ID_FREQ:
      *type = SPI_PARAM_TYPE_DOUBLE;
      *value = &p->freq;
      *size = sizeof (double);
      break;
    case PARAM_ID_VOLUME:
      *type = SPI_PARAM_TYPE_DOUBLE;
      *value = &p->volume;
      *size = sizeof (double);
      break;
    default:
      res = SPI_RESULT_INVALID_PARAM_ID;
      break;
  }
  return res;
}

static void
reset_audiotestsrc_params (SpiAudioTestSrcParams *params)
{
  params->wave = default_wave;
  params->freq = default_freq;
  params->volume = default_volume;
}

static SpiResult
spi_audiotestsrc_node_get_params (SpiNode        *node,
                            SpiParams     **params)
{
  SpiAudioTestSrc *this = (SpiAudioTestSrc *) node;

  if (node == NULL || params == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  memcpy (&this->tmp_params, &this->params, sizeof (this->tmp_params));
  *params = &this->tmp_params.param;

  return SPI_RESULT_OK;
}

static SpiResult
spi_audiotestsrc_node_set_params (SpiNode          *node,
                                  const SpiParams  *params)
{
  SpiAudioTestSrc *this = (SpiAudioTestSrc *) node;
  SpiAudioTestSrcParams *p = &this->params;
  SpiParamType type;
  size_t size;
  const void *value;

  if (node == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (params == NULL) {
    reset_audiotestsrc_params (p);
    return SPI_RESULT_OK;
  }

  if (params->get_param (params, PARAM_ID_WAVE, &type, &size, &value) == 0) {
    CHECK_TYPE (type, SPI_PARAM_TYPE_UINT32);
    CHECK_SIZE (size, sizeof (uint32_t));
    memcpy (&p->wave, value, size);
  }
  if (params->get_param (params, PARAM_ID_FREQ, &type, &size, &value) == 0) {
    CHECK_TYPE (type, SPI_PARAM_TYPE_DOUBLE);
    CHECK_SIZE (size, sizeof (double));
    memcpy (&p->freq, value, size);
  }
  if (params->get_param (params, PARAM_ID_VOLUME, &type, &size, &value) == 0) {
    CHECK_TYPE (type, SPI_PARAM_TYPE_DOUBLE);
    CHECK_SIZE (size, sizeof (double));
    memcpy (&p->volume, value, size);
  }
  return SPI_RESULT_OK;
}

static SpiResult
spi_audiotestsrc_node_send_command (SpiNode       *node,
                                    SpiCommand    *command)
{
  SpiAudioTestSrc *this = (SpiAudioTestSrc *) node;
  SpiResult res = SPI_RESULT_NOT_IMPLEMENTED;

  if (node == NULL || command == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  switch (command->type) {
    case SPI_COMMAND_INVALID:
      res = SPI_RESULT_INVALID_COMMAND;
      break;
    case SPI_COMMAND_ACTIVATE:
      this->last_event.type = SPI_EVENT_TYPE_ACTIVATED;
      this->last_event.data = NULL;
      this->last_event.size = 0;
      this->event = &this->last_event;
      res = SPI_RESULT_HAVE_EVENT;
      break;
    case SPI_COMMAND_DEACTIVATE:
      this->last_event.type = SPI_EVENT_TYPE_DEACTIVATED;
      this->last_event.data = NULL;
      this->last_event.size = 0;
      this->event = &this->last_event;
      res = SPI_RESULT_HAVE_EVENT;
      break;
    case SPI_COMMAND_START:
      break;
    case SPI_COMMAND_STOP:
      break;
    case SPI_COMMAND_FLUSH:
      break;
    case SPI_COMMAND_DRAIN:
      break;
    case SPI_COMMAND_MARKER:
      break;
  }
  return res;
}

static SpiResult
spi_audiotestsrc_node_get_event (SpiNode     *node,
                                 SpiEvent   **event)
{
  SpiAudioTestSrc *this = (SpiAudioTestSrc *) node;

  if (node == NULL || event == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (this->event == NULL)
    return SPI_RESULT_ERROR;

  *event = this->event;
  this->event = NULL;

  return SPI_RESULT_OK;
}

static SpiResult
spi_audiotestsrc_node_set_event_callback (SpiNode       *node,
                                          SpiEventCallback event,
                                          void          *user_data)
{
  SpiAudioTestSrc *this = (SpiAudioTestSrc *) node;

  if (node == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  this->event_cb = event;
  this->user_data = user_data;

  return SPI_RESULT_OK;
}

static SpiResult
spi_audiotestsrc_node_get_n_ports (SpiNode       *node,
                                   unsigned int  *n_input_ports,
                                   unsigned int  *max_input_ports,
                                   unsigned int  *n_output_ports,
                                   unsigned int  *max_output_ports)
{
  if (node == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports)
    *n_input_ports = 0;
  if (n_output_ports)
    *n_output_ports = 1;
  if (max_input_ports)
    *max_input_ports = 0;
  if (max_output_ports)
    *max_output_ports = 1;

  return SPI_RESULT_OK;
}

static SpiResult
spi_audiotestsrc_node_get_port_ids (SpiNode       *node,
                                    unsigned int   n_input_ports,
                                    uint32_t      *input_ids,
                                    unsigned int   n_output_ports,
                                    uint32_t      *output_ids)
{
  if (node == NULL || input_ids == NULL || output_ids == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (n_output_ports > 0)
    output_ids[0] = 0;

  return SPI_RESULT_OK;
}


static SpiResult
spi_audiotestsrc_node_add_port (SpiNode        *node,
                                SpiDirection    direction,
                                uint32_t       *port_id)
{
  return SPI_RESULT_NOT_IMPLEMENTED;
}

static SpiResult
spi_audiotestsrc_node_remove_port (SpiNode        *node,
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

static const SpiParamRangeInfo uint32_range[] = {
  { "min", "Minimum value", sizeof (uint32_t), &min_uint32 },
  { "max", "Maximum value", sizeof (uint32_t), &max_uint32 },
  { NULL, NULL, 0, NULL }
};

enum {
  SPI_PARAM_ID_MEDIA_TYPE,
  SPI_PARAM_ID_FORMAT,
  SPI_PARAM_ID_LAYOUT,
  SPI_PARAM_ID_SAMPLERATE,
  SPI_PARAM_ID_CHANNELS,
  SPI_PARAM_ID_LAST,
};

static const int32_t format_default_layout = 1;

static const SpiParamInfo raw_format_param_info[] =
{
  { SPI_PARAM_ID_MEDIA_TYPE, "media-type", "The media type",
                             SPI_PARAM_FLAG_READABLE,
                             SPI_PARAM_TYPE_STRING, 32,
                             strlen ("audio/x-raw")+1, "audio/x-raw",
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
                             SPI_PARAM_RANGE_TYPE_MIN_MAX, uint32_range,
                             NULL,
                             NULL },
  { SPI_PARAM_ID_CHANNELS,   "channels", "Audio channels",
                             SPI_PARAM_FLAG_READWRITE,
                             SPI_PARAM_TYPE_UINT32, sizeof (uint32_t),
                             0, NULL,
                             SPI_PARAM_RANGE_TYPE_MIN_MAX, uint32_range,
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

  if (index >= SPI_PARAM_ID_LAST)
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
  SpiAudioTestSrcFormat *f = (SpiAudioTestSrcFormat *) params;

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
  SpiAudioTestSrcFormat *f = (SpiAudioTestSrcFormat *) params;

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
spi_audiotestsrc_node_enum_port_formats (SpiNode          *node,
                                         uint32_t          port_id,
                                         unsigned int      index,
                                         SpiParams       **format)
{
  static SpiAudioTestSrcFormat fmt;

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
spi_audiotestsrc_node_set_port_format (SpiNode         *node,
                                       uint32_t         port_id,
                                       int              test_only,
                                       const SpiParams *format)
{
  SpiAudioTestSrc *this = (SpiAudioTestSrc *) node;
  SpiParamType type;
  size_t size;
  const void *value;
  SpiAudioTestSrcFormat *fmt;

  if (node == NULL)
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
spi_audiotestsrc_node_get_port_format (SpiNode          *node,
                                       uint32_t          port_id,
                                       const SpiParams **format)
{
  SpiAudioTestSrc *this = (SpiAudioTestSrc *) node;

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
spi_audiotestsrc_node_get_port_info (SpiNode       *node,
                                     uint32_t       port_id,
                                     SpiPortInfo   *info)
{
  if (node == NULL || info == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPI_RESULT_INVALID_PORT;

  info->flags = SPI_PORT_INFO_FLAG_CAN_USE_BUFFER |
                SPI_PORT_INFO_FLAG_NO_REF;

  return SPI_RESULT_OK;
}

static SpiResult
spi_audiotestsrc_node_get_port_params (SpiNode    *node,
                                 uint32_t    port_id,
                                 SpiParams **params)
{
  return SPI_RESULT_NOT_IMPLEMENTED;
}

static SpiResult
spi_audiotestsrc_node_set_port_params (SpiNode         *node,
                                       uint32_t         port_id,
                                       const SpiParams *params)
{
  return SPI_RESULT_NOT_IMPLEMENTED;
}

static SpiResult
spi_audiotestsrc_node_get_port_status (SpiNode        *node,
                                       uint32_t        port_id,
                                       SpiPortStatus  *status)
{
  SpiAudioTestSrc *this = (SpiAudioTestSrc *) node;

  if (node == NULL || status == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPI_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPI_RESULT_NO_FORMAT;

  status->flags = SPI_PORT_STATUS_FLAG_HAVE_OUTPUT;

  return SPI_RESULT_OK;
}

static SpiResult
spi_audiotestsrc_node_send_port_data (SpiNode       *node,
                                      SpiDataInfo   *data)
{
  return SPI_RESULT_INVALID_PORT;
}

static SpiResult
spi_audiotestsrc_node_receive_port_data (SpiNode      *node,
                                         unsigned int  n_data,
                                         SpiDataInfo  *data)
{
  SpiAudioTestSrc *this = (SpiAudioTestSrc *) node;
  size_t i, size;
  uint8_t *ptr;

  if (node == NULL || n_data == 0 || data == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (data->port_id != 0)
    return SPI_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPI_RESULT_NO_FORMAT;

  if (data->buffer == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  ptr = data->buffer->datas[0].data;
  size = data->buffer->datas[0].size;

  for (i = 0; i < size; i++)
    ptr[i] = rand();

  return SPI_RESULT_OK;
}

SpiNode *
spi_audiotestsrc_new (void)
{
  SpiNode *node;
  SpiAudioTestSrc *this;

  node = calloc (1, sizeof (SpiAudioTestSrc));

  node->get_params = spi_audiotestsrc_node_get_params;
  node->set_params = spi_audiotestsrc_node_set_params;
  node->send_command = spi_audiotestsrc_node_send_command;
  node->get_event = spi_audiotestsrc_node_get_event;
  node->set_event_callback = spi_audiotestsrc_node_set_event_callback;
  node->get_n_ports = spi_audiotestsrc_node_get_n_ports;
  node->get_port_ids = spi_audiotestsrc_node_get_port_ids;
  node->add_port = spi_audiotestsrc_node_add_port;
  node->remove_port = spi_audiotestsrc_node_remove_port;
  node->enum_port_formats = spi_audiotestsrc_node_enum_port_formats;
  node->set_port_format = spi_audiotestsrc_node_set_port_format;
  node->get_port_format = spi_audiotestsrc_node_get_port_format;
  node->get_port_info = spi_audiotestsrc_node_get_port_info;
  node->get_port_params = spi_audiotestsrc_node_get_port_params;
  node->set_port_params = spi_audiotestsrc_node_set_port_params;
  node->get_port_status = spi_audiotestsrc_node_get_port_status;
  node->send_port_data = spi_audiotestsrc_node_send_port_data;
  node->receive_port_data = spi_audiotestsrc_node_receive_port_data;

  this = (SpiAudioTestSrc *) node;
  this->params.param.enum_param_info = enum_param_info;
  this->params.param.set_param = set_param;
  this->params.param.get_param = get_param;
  reset_audiotestsrc_params (&this->params);

  return node;
}
