/* Spi ALSA Sink
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

#include <spi/node.h>

#include <asoundlib.h>
#include <pthread.h>

#include "spi-plugins.h"

typedef struct _SpiALSASink SpiALSASink;


static const char default_device[] = "default";
static const uint32_t default_buffer_time = 10000;
static const uint32_t default_period_time = 5000;
static const bool default_period_event = 0;

typedef struct {
  SpiParams param;
  char device[64];
  char device_name[128];
  char card_name[128];
  uint32_t buffer_time;
  uint32_t period_time;
  bool period_event;
} SpiALSASinkParams;

static void
reset_alsa_sink_params (SpiALSASinkParams *params)
{
  strncpy (params->device, default_device, 64);
  params->buffer_time = default_buffer_time;
  params->period_time = default_period_time;
  params->period_event = default_period_event;
}

typedef struct {
  SpiParams param;
  char media_type[32];
  uint32_t unset_mask;
  char format[16];
  uint32_t layout;
  uint32_t samplerate;
  uint32_t channels;
  uint32_t position[16];
  uint32_t mpegversion;
  uint32_t mpegaudioversion;
  bool parsed;
} SpiALSASinkFormat;

typedef struct {
  snd_pcm_t *handle;
  snd_output_t *output;
  snd_pcm_sframes_t buffer_size;
  snd_pcm_sframes_t period_size;
  snd_pcm_channel_area_t areas[16];
  pthread_t thread;
  bool running;
} SpiALSAState;


typedef struct _ALSABuffer ALSABuffer;

struct _ALSABuffer {
  SpiBuffer buffer;
  SpiMeta meta[1];
  SpiMetaHeader header;
  SpiData data[1];
  ALSABuffer *next;
};

struct _SpiALSASink {
  SpiHandle handle;
  SpiNode   node;

  SpiALSASinkParams params;

  bool activated;

  SpiEventCallback event_cb;
  void *user_data;

  int have_format;
  SpiALSASinkFormat current_format;

  SpiALSAState state;

  SpiBuffer *input_buffer;

  ALSABuffer buffer;
};

#include "alsa-utils.c"

static const uint32_t default_samplerate = 44100;
static const uint32_t min_samplerate = 1;
static const uint32_t max_samplerate = UINT32_MAX;

static const SpiParamRangeInfo int32_range[] = {
  { "min", "Minimum value", 4, &min_samplerate },
  { "max", "Maximum value", 4, &max_samplerate },
  { NULL, NULL, 0, NULL }
};

enum {
  PARAM_ID_DEVICE,
  PARAM_ID_DEVICE_NAME,
  PARAM_ID_CARD_NAME,
  PARAM_ID_BUFFER_TIME,
  PARAM_ID_PERIOD_TIME,
  PARAM_ID_PERIOD_EVENT,
  PARAM_ID_LAST,
};

static const SpiParamInfo param_info[] =
{
  { PARAM_ID_DEVICE,            "device", "ALSA device, as defined in an asound configuration file",
                                SPI_PARAM_FLAG_READWRITE,
                                SPI_PARAM_TYPE_STRING, 63,
                                strlen (default_device)+1, default_device,
                                SPI_PARAM_RANGE_TYPE_NONE, NULL,
                                NULL,
                                NULL },
  { PARAM_ID_DEVICE_NAME,       "device-name", "Human-readable name of the sound device",
                                SPI_PARAM_FLAG_READABLE,
                                SPI_PARAM_TYPE_STRING, 127,
                                0, NULL,
                                SPI_PARAM_RANGE_TYPE_NONE, NULL,
                                NULL,
                                NULL },
  { PARAM_ID_CARD_NAME,         "card-name", "Human-readable name of the sound card",
                                SPI_PARAM_FLAG_READABLE,
                                SPI_PARAM_TYPE_STRING, 127,
                                0, NULL,
                                SPI_PARAM_RANGE_TYPE_NONE, NULL,
                                NULL,
                                NULL },
  { PARAM_ID_BUFFER_TIME,       "buffer-time", "The total size of the buffer in time",
                                SPI_PARAM_FLAG_READWRITE,
                                SPI_PARAM_TYPE_UINT32, sizeof (uint32_t),
                                sizeof (uint32_t), &default_buffer_time,
                                SPI_PARAM_RANGE_TYPE_MIN_MAX, int32_range,
                                NULL,
                                NULL },
  { PARAM_ID_PERIOD_TIME,       "period-time", "The size of a period in time",
                                SPI_PARAM_FLAG_READWRITE,
                                SPI_PARAM_TYPE_UINT32, sizeof (uint32_t),
                                sizeof (uint32_t), &default_period_time,
                                SPI_PARAM_RANGE_TYPE_MIN_MAX, int32_range,
                                NULL,
                                NULL },
  { PARAM_ID_PERIOD_EVENT,      "period-event", "Generate an event each period",
                                SPI_PARAM_FLAG_READWRITE,
                                SPI_PARAM_TYPE_BOOL, sizeof (bool),
                                sizeof (bool), &default_period_event,
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
  SpiALSASinkParams *p = (SpiALSASinkParams *) params;

  if (params == NULL || value == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  switch (id) {
    case PARAM_ID_DEVICE:
      CHECK_TYPE (type, SPI_PARAM_TYPE_STRING);
      CHECK_SIZE_MAX (size, 64);
      strncpy (p->device, value, 64);
      break;
    case PARAM_ID_BUFFER_TIME:
      CHECK_TYPE (type, SPI_PARAM_TYPE_UINT32);
      CHECK_SIZE (size, sizeof (uint32_t));
      memcpy (&p->buffer_time, value, size);
      break;
    case PARAM_ID_PERIOD_TIME:
      CHECK_TYPE (type, SPI_PARAM_TYPE_UINT32);
      CHECK_SIZE (size, sizeof (uint32_t));
      memcpy (&p->period_time, value, size);
      break;
    case PARAM_ID_PERIOD_EVENT:
      CHECK_TYPE (type, SPI_PARAM_TYPE_BOOL);
      CHECK_SIZE (size, sizeof (uint32_t));
      memcpy (&p->period_event, value, size);
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
  SpiALSASinkParams *p = (SpiALSASinkParams *) params;

  if (params == NULL || type == NULL || size == NULL || value == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  switch (id) {
    case PARAM_ID_DEVICE:
      *type = SPI_PARAM_TYPE_STRING;
      *value = p->device;
      *size = strlen (p->device)+1;
      break;
    case PARAM_ID_DEVICE_NAME:
      *type = SPI_PARAM_TYPE_STRING;
      *value = p->device_name;
      *size = strlen (p->device_name)+1;
      break;
    case PARAM_ID_CARD_NAME:
      *type = SPI_PARAM_TYPE_STRING;
      *value = p->card_name;
      *size = strlen (p->card_name)+1;
      break;
    case PARAM_ID_BUFFER_TIME:
      *type = SPI_PARAM_TYPE_UINT32;
      *value = &p->buffer_time;
      *size = sizeof (uint32_t);
      break;
    case PARAM_ID_PERIOD_TIME:
      *type = SPI_PARAM_TYPE_UINT32;
      *value = &p->period_time;
      *size = sizeof (uint32_t);
      break;
    case PARAM_ID_PERIOD_EVENT:
      *type = SPI_PARAM_TYPE_BOOL;
      *value = &p->period_event;
      *size = sizeof (bool);
      break;
    default:
      res = SPI_RESULT_INVALID_PARAM_ID;
      break;
  }
  return res;
}


static SpiResult
spi_alsa_sink_node_get_params (SpiHandle     *node,
                               SpiParams     **params)
{
  static SpiALSASinkParams p;
  SpiALSASink *this = (SpiALSASink *) node;

  if (node == NULL || params == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  memcpy (&p, &this->params, sizeof (p));
  *params = &p.param;

  return SPI_RESULT_OK;
}

static SpiResult
spi_alsa_sink_node_set_params (SpiHandle        *node,
                               const SpiParams  *params)
{
  SpiALSASink *this = (SpiALSASink *) node;
  SpiALSASinkParams *p = &this->params;
  SpiParamType type;
  size_t size;
  const void *value;

  if (node == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (params == NULL) {
    reset_alsa_sink_params (p);
    return SPI_RESULT_OK;
  }

  if (params->get_param (params, PARAM_ID_DEVICE, &type, &size, &value) == 0) {
    CHECK_TYPE (type, SPI_PARAM_TYPE_STRING);
    CHECK_SIZE_MAX (size, 64);
    strncpy (p->device, value, 64);
  }
  if (params->get_param (params, PARAM_ID_BUFFER_TIME, &type, &size, &value) == 0) {
    CHECK_TYPE (type, SPI_PARAM_TYPE_UINT32);
    CHECK_SIZE (size, sizeof (uint32_t));
    memcpy (&p->buffer_time, value, size);
  }
  if (params->get_param (params, PARAM_ID_PERIOD_TIME, &type, &size, &value) == 0) {
    CHECK_TYPE (type, SPI_PARAM_TYPE_UINT32);
    CHECK_SIZE (size, sizeof (uint32_t));
    memcpy (&p->period_time, value, size);
  }
  if (params->get_param (params, PARAM_ID_PERIOD_EVENT, &type, &size, &value) == 0) {
    CHECK_TYPE (type, SPI_PARAM_TYPE_BOOL);
    CHECK_SIZE (size, sizeof (bool));
    memcpy (&p->period_event, value, size);
  }
  return SPI_RESULT_OK;
}

static SpiResult
spi_alsa_sink_node_send_command (SpiHandle     *node,
                                 SpiCommand    *command)
{
  SpiALSASink *this = (SpiALSASink *) node;

  if (node == NULL || command == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  switch (command->type) {
    case SPI_COMMAND_INVALID:
      return SPI_RESULT_INVALID_COMMAND;

    case SPI_COMMAND_ACTIVATE:
      if (!this->activated) {
        spi_alsa_open (this);
        this->activated = true;
      }
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
      if (this->activated) {
        spi_alsa_close (this);
        this->activated = false;
      }
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
      spi_alsa_start (this);
      break;
    case SPI_COMMAND_STOP:
      spi_alsa_stop (this);
      break;
    case SPI_COMMAND_FLUSH:
    case SPI_COMMAND_DRAIN:
    case SPI_COMMAND_MARKER:
      return SPI_RESULT_NOT_IMPLEMENTED;
  }
  return SPI_RESULT_OK;
}

static SpiResult
spi_alsa_sink_node_set_event_callback (SpiHandle     *node,
                                       SpiEventCallback event,
                                       void          *user_data)
{
  SpiALSASink *this = (SpiALSASink *) node;

  if (node == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  this->event_cb = event;
  this->user_data = user_data;

  return SPI_RESULT_OK;
}

static SpiResult
spi_alsa_sink_node_get_n_ports (SpiHandle     *node,
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
    *n_output_ports = 0;
  if (max_input_ports)
    *max_input_ports = 1;
  if (max_output_ports)
    *max_output_ports = 0;

  return SPI_RESULT_OK;
}

static SpiResult
spi_alsa_sink_node_get_port_ids (SpiHandle     *node,
                                 unsigned int   n_input_ports,
                                 uint32_t      *input_ids,
                                 unsigned int   n_output_ports,
                                 uint32_t      *output_ids)
{
  if (node == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports > 0)
    input_ids[0] = 0;

  return SPI_RESULT_OK;
}


static SpiResult
spi_alsa_sink_node_add_port (SpiHandle      *node,
                             SpiDirection    direction,
                             uint32_t       *port_id)
{
  return SPI_RESULT_NOT_IMPLEMENTED;
}

static SpiResult
spi_alsa_sink_node_remove_port (SpiHandle      *node,
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

enum {
  SPI_PARAM_ID_MEDIA_TYPE,
  SPI_PARAM_ID_FORMAT,
  SPI_PARAM_ID_LAYOUT,
  SPI_PARAM_ID_SAMPLERATE,
  SPI_PARAM_ID_CHANNELS,
  SPI_PARAM_ID_MPEG_VERSION,
  SPI_PARAM_ID_MPEG_AUDIO_VERSION,
  SPI_PARAM_ID_PARSED,
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

  if (index >= 5)
    return SPI_RESULT_ENUM_END;
  *info = &raw_format_param_info[index];
  return SPI_RESULT_OK;
}

static const uint32_t default_mpeg_version = 1;
static const uint32_t min_mpeg_audio_version = 1;
static const uint32_t max_mpeg_audio_version = 2;
static const bool default_parsed = 1;

static const SpiParamRangeInfo mpeg_audio_version_range[] = {
  { "min", "Minimum value", 4, &min_mpeg_audio_version },
  { "max", "Maximum value", 4, &max_mpeg_audio_version },
  { NULL, NULL, 0, NULL }
};

static const SpiParamInfo mpeg_format_param_info[] =
{
  { SPI_PARAM_ID_MEDIA_TYPE,            "media-type", "The media type",
                                        SPI_PARAM_FLAG_READABLE,
                                        SPI_PARAM_TYPE_STRING, 32,
                                        strlen ("audio/mpeg")+1, "audio/mpeg",
                                        SPI_PARAM_RANGE_TYPE_NONE, NULL,
                                        NULL,
                                        NULL },
  { SPI_PARAM_ID_MPEG_VERSION,          "mpegversion", "The MPEG version",
                                        SPI_PARAM_FLAG_READABLE,
                                        SPI_PARAM_TYPE_UINT32, sizeof (uint32_t),
                                        sizeof (uint32_t), &default_mpeg_version,
                                        SPI_PARAM_RANGE_TYPE_NONE, NULL,
                                        NULL,
                                        NULL },
  { SPI_PARAM_ID_MPEG_AUDIO_VERSION,    "mpegaudioversion", "The MPEG audio version",
                                        SPI_PARAM_FLAG_READWRITE,
                                        SPI_PARAM_TYPE_UINT32, sizeof (uint32_t),
                                        0, NULL,
                                        SPI_PARAM_RANGE_TYPE_MIN_MAX, mpeg_audio_version_range,
                                        NULL,
                                        NULL },
  { SPI_PARAM_ID_PARSED,                "parsed", "Parsed input",
                                        SPI_PARAM_FLAG_READABLE,
                                        SPI_PARAM_TYPE_BOOL, sizeof (bool),
                                        sizeof (bool), &default_parsed,
                                        SPI_PARAM_RANGE_TYPE_NONE, NULL,
                                        NULL,
                                        NULL },
};

static SpiResult
enum_mpeg_format_param_info (const SpiParams     *params,
                             unsigned int         index,
                             const SpiParamInfo **info)
{
  if (params == NULL || info == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (index >= 4)
    return SPI_RESULT_ENUM_END;
  *info = &mpeg_format_param_info[index];
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
  SpiALSASinkFormat *f = (SpiALSASinkFormat *) params;

  if (params == NULL || value == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  switch (id) {
    case SPI_PARAM_ID_FORMAT:
      CHECK_TYPE (type, SPI_PARAM_TYPE_STRING);
      CHECK_SIZE_MAX (size, 16);
      strncpy (f->format, value, 16);
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
    case SPI_PARAM_ID_MPEG_AUDIO_VERSION:
      CHECK_TYPE (type, SPI_PARAM_TYPE_UINT32);
      CHECK_SIZE (size, sizeof (uint32_t));
      memcpy (&f->mpegaudioversion, value, size);
      MARK_SET (f->unset_mask, 6);
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
  SpiALSASinkFormat *f = (SpiALSASinkFormat *) params;

  if (params == NULL || type == NULL || size == NULL || value == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  switch (id) {
    case SPI_PARAM_ID_MEDIA_TYPE:
      CHECK_UNSET (f->unset_mask, 0);
      *type = SPI_PARAM_TYPE_STRING;
      *value = f->media_type;
      *size = strlen (f->media_type)+1;
      break;
    case SPI_PARAM_ID_FORMAT:
      CHECK_UNSET (f->unset_mask, 1);
      *type = SPI_PARAM_TYPE_STRING;
      *value = f->format;
      *size = strlen (f->format)+1;
      break;
    case SPI_PARAM_ID_LAYOUT:
      CHECK_UNSET (f->unset_mask, 2);
      *type = SPI_PARAM_TYPE_UINT32;
      *value = &f->layout;
      *size = sizeof (uint32_t);
      break;
    case SPI_PARAM_ID_SAMPLERATE:
      CHECK_UNSET (f->unset_mask, 3);
      *type = SPI_PARAM_TYPE_UINT32;
      *value = &f->samplerate;
      *size = sizeof (uint32_t);
      break;
    case SPI_PARAM_ID_CHANNELS:
      CHECK_UNSET (f->unset_mask, 4);
      *type = SPI_PARAM_TYPE_UINT32;
      *value = &f->channels;
      *size = sizeof (uint32_t);
      break;
    case SPI_PARAM_ID_MPEG_VERSION:
      CHECK_UNSET (f->unset_mask, 5);
      *type = SPI_PARAM_TYPE_UINT32;
      *value = &f->mpegversion;
      *size = sizeof (uint32_t);
      break;
    case SPI_PARAM_ID_MPEG_AUDIO_VERSION:
      CHECK_UNSET (f->unset_mask, 6);
      *type = SPI_PARAM_TYPE_UINT32;
      *value = &f->mpegaudioversion;
      *size = sizeof (uint32_t);
      break;
    case SPI_PARAM_ID_PARSED:
      CHECK_UNSET (f->unset_mask, 7);
      *type = SPI_PARAM_TYPE_BOOL;
      *value = &f->parsed;
      *size = sizeof (bool);
      break;
    default:
      return SPI_RESULT_INVALID_PARAM_ID;
  }
  return SPI_RESULT_OK;
}


static SpiResult
spi_alsa_sink_node_enum_port_formats (SpiHandle       *node,
                                      uint32_t         port_id,
                                      unsigned int     index,
                                      SpiParams      **format)
{
  static SpiALSASinkFormat fmt;

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
    case 1:
      strcpy (fmt.media_type, "audio/mpeg");
      fmt.mpegversion = 1;
      fmt.parsed = 1;
      fmt.unset_mask = (1 << 6);
      fmt.param.enum_param_info = enum_mpeg_format_param_info;
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
spi_alsa_sink_node_set_port_format (SpiHandle       *node,
                                    uint32_t         port_id,
                                    int              test_only,
                                    const SpiParams *format)
{
  SpiALSASink *this = (SpiALSASink *) node;
  SpiParamType type;
  size_t size;
  const void *value;
  SpiALSASinkFormat *fmt;

  if (node == NULL || format == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPI_RESULT_INVALID_PORT;

  fmt = &this->current_format;

  if (format == NULL) {
    fmt->param.get_param = NULL;
    this->have_format = 0;
    return SPI_RESULT_OK;
  }

  if (format->get_param (format,
                         SPI_PARAM_ID_MEDIA_TYPE,
                         &type, &size, &value) < 0)
    return SPI_RESULT_INVALID_MEDIA_TYPE;
  CHECK_TYPE (type, SPI_PARAM_TYPE_STRING);
  CHECK_SIZE_MAX (size, 32);
  strncpy (fmt->media_type, value, 32);

  if (format->get_param (format,
                         SPI_PARAM_ID_FORMAT,
                         &type, &size, &value) < 0)
    return SPI_RESULT_INVALID_FORMAT_PARAMS;
  CHECK_TYPE (type, SPI_PARAM_TYPE_STRING);
  CHECK_SIZE_MAX (size, 16);
  strncpy (fmt->format, value, 16);

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
  this->have_format = 1;


  return SPI_RESULT_OK;
}

static SpiResult
spi_alsa_sink_node_get_port_format (SpiHandle        *node,
                                    uint32_t          port_id,
                                    const SpiParams **format)
{
  SpiALSASink *this = (SpiALSASink *) node;

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
spi_alsa_sink_node_get_port_info (SpiHandle     *node,
                                  uint32_t       port_id,
                                  SpiPortInfo   *info)
{
  if (node == NULL || info == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPI_RESULT_INVALID_PORT;

  info->flags = SPI_PORT_INFO_FLAG_NONE;

  return SPI_RESULT_OK;
}

static SpiResult
spi_alsa_sink_node_get_port_params (SpiHandle  *node,
                                    uint32_t    port_id,
                                    SpiParams **params)
{
  return SPI_RESULT_NOT_IMPLEMENTED;
}

static SpiResult
spi_alsa_sink_node_set_port_params (SpiHandle       *node,
                                    uint32_t         port_id,
                                    const SpiParams *params)
{
  return SPI_RESULT_NOT_IMPLEMENTED;
}

static SpiResult
spi_alsa_sink_node_get_port_status (SpiHandle      *node,
                                    uint32_t        port_id,
                                    SpiPortStatus  *status)
{
  if (node == NULL || status == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPI_RESULT_INVALID_PORT;

  status->flags = SPI_PORT_STATUS_FLAG_NEED_INPUT;

  return SPI_RESULT_OK;
}

static SpiResult
spi_alsa_sink_node_push_port_input (SpiHandle      *node,
                                    unsigned int    n_info,
                                    SpiInputInfo   *info)
{
  SpiALSASink *this = (SpiALSASink *) node;
  unsigned int i;
  bool have_error = false, have_enough = false;

  if (node == NULL || n_info == 0 || info == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  for (i = 0; i < n_info; i++) {
    if (info[i].port_id != 0) {
      info[i].status = SPI_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }

    if (info[i].buffer != NULL) {
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
      this->input_buffer = spi_buffer_ref (info[i].buffer);
    }
    info[i].status = SPI_RESULT_OK;
  }
  if (have_error)
    return SPI_RESULT_ERROR;
  if (have_enough)
    return SPI_RESULT_HAVE_ENOUGH_INPUT;

  return SPI_RESULT_OK;
}

static SpiResult
spi_alsa_sink_node_pull_port_output (SpiHandle      *node,
                                     unsigned int    n_info,
                                     SpiOutputInfo  *info)
{
  return SPI_RESULT_INVALID_PORT;
}

static SpiResult
spi_alsa_sink_get_interface (SpiHandle               *handle,
                             uint32_t                 interface_id,
                             void                   **interface)
{
  SpiALSASink *this = (SpiALSASink *) handle;

  if (handle == NULL || interface == NULL)
    return SPI_RESULT_INVALID_ARGUMENTS;

  switch (interface_id) {
    case SPI_INTERFACE_ID_NODE:
      *interface = &this->node;
      break;
    default:
      return SPI_RESULT_UNKNOWN_INTERFACE;
  }
  return SPI_RESULT_OK;
}


SpiHandle *
spi_alsa_sink_new (void)
{
  SpiHandle *handle;
  SpiNode *node;
  SpiALSASink *this;

  handle = calloc (1, sizeof (SpiALSASink));
  handle->get_interface = spi_alsa_sink_get_interface;

  this = (SpiALSASink *) handle;
  this->params.param.enum_param_info = enum_param_info;
  this->params.param.set_param = set_param;
  this->params.param.get_param = get_param;
  reset_alsa_sink_params (&this->params);

  node = &this->node;
  node->get_params = spi_alsa_sink_node_get_params;
  node->set_params = spi_alsa_sink_node_set_params;
  node->send_command = spi_alsa_sink_node_send_command;
  node->set_event_callback = spi_alsa_sink_node_set_event_callback;
  node->get_n_ports = spi_alsa_sink_node_get_n_ports;
  node->get_port_ids = spi_alsa_sink_node_get_port_ids;
  node->add_port = spi_alsa_sink_node_add_port;
  node->remove_port = spi_alsa_sink_node_remove_port;
  node->enum_port_formats = spi_alsa_sink_node_enum_port_formats;
  node->set_port_format = spi_alsa_sink_node_set_port_format;
  node->get_port_format = spi_alsa_sink_node_get_port_format;
  node->get_port_info = spi_alsa_sink_node_get_port_info;
  node->get_port_params = spi_alsa_sink_node_get_port_params;
  node->set_port_params = spi_alsa_sink_node_set_port_params;
  node->get_port_status = spi_alsa_sink_node_get_port_status;
  node->push_port_input = spi_alsa_sink_node_push_port_input;
  node->pull_port_output = spi_alsa_sink_node_pull_port_output;

  return handle;
}
