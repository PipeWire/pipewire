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
#include <stdio.h>

#include <spa/node.h>
#include <spa/audio/format.h>

typedef struct _SpaAudioTestSrc SpaAudioTestSrc;

typedef struct {
  SpaProps props;
  uint32_t wave;
  double freq;
  double volume;
} SpaAudioTestSrcProps;

typedef struct _ATSBuffer ATSBuffer;

struct _ATSBuffer {
  SpaBuffer buffer;
  SpaMeta metas[1];
  SpaMetaHeader header;
  SpaData datas[1];
  SpaBuffer *outbuf;
  bool outstanding;
  ATSBuffer *next;
  void *ptr;
  size_t size;
};

struct _SpaAudioTestSrc {
  SpaHandle handle;
  SpaNode node;

  SpaAudioTestSrcProps props[2];

  SpaNodeEventCallback event_cb;
  void *user_data;

  SpaPollItem idle;

  SpaPortInfo info;
  SpaAllocParam *params[1];
  SpaAllocParamBuffers param_buffers;
  SpaPortStatus status;

  bool have_format;
  SpaFormatAudio query_format;
  SpaFormatAudio current_format;

  bool have_buffers;
  SpaMemory *alloc_mem;
  ATSBuffer *alloc_buffers;
  unsigned int n_buffers;

  ATSBuffer *empty;
  ATSBuffer *ready;
  unsigned int ready_count;
};

static const uint32_t default_wave = 1.0;
static const double default_volume = 1.0;
static const double min_volume = 0.0;
static const double max_volume = 10.0;
static const double default_freq = 440.0;
static const double min_freq = 0.0;
static const double max_freq = 50000000.0;

static const SpaPropRangeInfo volume_range[] = {
  { "min", "Minimum value", { sizeof (double), &min_volume } },
  { "max", "Maximum value", { sizeof (double), &max_volume } },
};

static const uint32_t wave_val_sine = 0;
static const uint32_t wave_val_square = 1;

static const SpaPropRangeInfo wave_range[] = {
  { "sine", "Sine", { sizeof (uint32_t), &wave_val_sine } },
  { "square", "Square", { sizeof (uint32_t), &wave_val_square } },
};

static const SpaPropRangeInfo freq_range[] = {
  { "min", "Minimum value", { sizeof (double), &min_freq } },
  { "max", "Maximum value", { sizeof (double), &max_freq } },
};

enum {
  PROP_ID_WAVE,
  PROP_ID_FREQ,
  PROP_ID_VOLUME,
  PROP_ID_LAST,
};

static const SpaPropInfo prop_info[] =
{
  { PROP_ID_WAVE,              offsetof (SpaAudioTestSrcProps, wave),
                               "wave", "Oscillator waveform",
                               SPA_PROP_FLAG_READWRITE,
                               SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                               SPA_PROP_RANGE_TYPE_ENUM, SPA_N_ELEMENTS (wave_range), wave_range,
                               NULL },
  { PROP_ID_FREQ,              offsetof (SpaAudioTestSrcProps, freq),
                               "freq", "Frequency of test signal. The sample rate needs to be at least 4 times higher",
                               SPA_PROP_FLAG_READWRITE,
                               SPA_PROP_TYPE_DOUBLE, sizeof (double),
                               SPA_PROP_RANGE_TYPE_MIN_MAX, 2, freq_range,
                               NULL },
  { PROP_ID_VOLUME,            offsetof (SpaAudioTestSrcProps, volume),
                               "volume", "The Volume factor",
                               SPA_PROP_FLAG_READWRITE,
                               SPA_PROP_TYPE_DOUBLE, sizeof (double),
                               SPA_PROP_RANGE_TYPE_MIN_MAX, 2, volume_range,
                               NULL },
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
send_have_output (SpaAudioTestSrc *this)
{
  SpaNodeEvent event;
  SpaNodeEventHaveOutput ho;

  if (this->event_cb) {
    event.type = SPA_NODE_EVENT_TYPE_HAVE_OUTPUT;
    event.size = sizeof (ho);
    event.data = &ho;
    ho.port_id = 0;
    this->event_cb (&this->node, &event, this->user_data);
  }

  return SPA_RESULT_OK;
}

static void
update_state (SpaAudioTestSrc *this, SpaNodeState state)
{
  SpaNodeEvent event;
  SpaNodeEventStateChange sc;

  if (this->node.state == state)
    return;

  this->node.state = state;

  if (this->event_cb) {
    event.type = SPA_NODE_EVENT_TYPE_STATE_CHANGE;
    event.data = &sc;
    event.size = sizeof (sc);
    sc.state = state;
    this->event_cb (&this->node, &event, this->user_data);
  }
}

static SpaResult
update_idle_enabled (SpaAudioTestSrc *this, bool enabled)
{
  SpaNodeEvent event;

  if (this->event_cb) {
    this->idle.enabled = enabled;
    event.type = SPA_NODE_EVENT_TYPE_UPDATE_POLL;
    event.data = &this->idle;
    event.size = sizeof (this->idle);
    this->event_cb (&this->node, &event, this->user_data);
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_send_command (SpaNode        *node,
                                    SpaNodeCommand *command)
{
  SpaAudioTestSrc *this;

  if (node == NULL || node->handle == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) node->handle;

  switch (command->type) {
    case SPA_NODE_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_NODE_COMMAND_START:
    {
      if (!this->have_format)
        return SPA_RESULT_NO_FORMAT;

      if (!this->have_buffers)
        return SPA_RESULT_NO_BUFFERS;

      update_idle_enabled (this, true);

      update_state (this, SPA_NODE_STATE_STREAMING);
      break;
    }
    case SPA_NODE_COMMAND_PAUSE:
    {
      if (!this->have_format)
        return SPA_RESULT_NO_FORMAT;

      if (!this->have_buffers)
        return SPA_RESULT_NO_BUFFERS;

      update_idle_enabled (this, false);

      update_state (this, SPA_NODE_STATE_PAUSED);
      break;
    }
    case SPA_NODE_COMMAND_FLUSH:
    case SPA_NODE_COMMAND_DRAIN:
    case SPA_NODE_COMMAND_MARKER:
    case SPA_NODE_COMMAND_CLOCK_UPDATE:
      return SPA_RESULT_NOT_IMPLEMENTED;
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_set_event_callback (SpaNode              *node,
                                          SpaNodeEventCallback  event_cb,
                                          void                 *user_data)
{
  SpaAudioTestSrc *this;
  SpaNodeEvent event;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) node->handle;

  if (event_cb == NULL && this->event_cb) {
    event.type = SPA_NODE_EVENT_TYPE_REMOVE_POLL;
    event.data = &this->idle;
    event.size = sizeof (this->idle);
    this->event_cb (&this->node, &event, this->user_data);
  }

  this->event_cb = event_cb;
  this->user_data = user_data;

  if (this->event_cb) {
    event.type = SPA_NODE_EVENT_TYPE_ADD_POLL;
    event.data = &this->idle;
    event.size = sizeof (this->idle);
    this->event_cb (&this->node, &event, this->user_data);
  }
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
  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_output_ports > 0 && output_ids != NULL)
    output_ids[0] = 0;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_add_port (SpaNode        *node,
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
      if (filter)
        spa_format_audio_parse (filter, &this->query_format);
      else
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
clear_buffers (SpaAudioTestSrc *this)
{
  if (this->have_buffers) {
    fprintf (stderr, "audiotestsrc %p: clear buffers\n", this);
    if (this->alloc_mem)
      spa_memory_unref (&this->alloc_mem->mem);
    this->alloc_mem = NULL;
    this->n_buffers = 0;
    this->have_buffers = false;
  }
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
    clear_buffers (this);
  } else {
    if ((res = spa_format_audio_parse (format, &this->current_format)) < 0)
      return res;

    this->have_format = true;
  }

  if (this->have_format) {
    this->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
    this->info.maxbuffering = -1;
    this->info.latency = -1;

    this->info.n_params = 1;
    this->info.params = this->params;
    this->params[0] = &this->param_buffers.param;
    this->param_buffers.param.type = SPA_ALLOC_PARAM_TYPE_BUFFERS;
    this->param_buffers.param.size = sizeof (this->param_buffers);
    this->param_buffers.minsize = 1024;
    this->param_buffers.stride = 1024;
    this->param_buffers.min_buffers = 2;
    this->param_buffers.max_buffers = 32;
    this->param_buffers.align = 16;
    this->info.features = NULL;
    update_state (this, SPA_NODE_STATE_READY);
  }
  else
    update_state (this, SPA_NODE_STATE_CONFIGURE);

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
  SpaAudioTestSrc *this;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  clear_buffers (this);
  if (buffers != NULL && n_buffers != 0) {
    unsigned int i;

    this->alloc_mem = spa_memory_alloc_size (SPA_MEMORY_POOL_LOCAL,
                                             NULL,
                                             sizeof (ATSBuffer) * n_buffers);
    this->alloc_buffers = spa_memory_ensure_ptr (this->alloc_mem);

    for (i = 0; i < n_buffers; i++) {
      ATSBuffer *b;
      SpaMemoryRef *mem_ref;
      SpaMemory *mem;
      SpaData *d = SPA_BUFFER_DATAS (buffers[i]);

      b = &this->alloc_buffers[i];
      if (i + 1 == n_buffers)
        b->next = NULL;
      else
        b->next = &this->alloc_buffers[i + 1];

      b->buffer.mem.mem = this->alloc_mem->mem;
      b->buffer.mem.offset = sizeof (ATSBuffer) * i;
      b->buffer.mem.size = sizeof (ATSBuffer);
      b->buffer.id = SPA_ID_INVALID;
      b->outbuf = buffers[i];
      b->outstanding = true;

      mem_ref = &d[0].mem.mem;
      if (!(mem = spa_memory_find (mem_ref))) {
        fprintf (stderr, "audiotestsrc %p: invalid memory on buffer %p\n", this, buffers[i]);
        continue;
      }
      b->ptr = SPA_MEMBER (spa_memory_ensure_ptr (mem), d[0].mem.offset, void);
      b->size = d[0].mem.size;
    }
    this->n_buffers = n_buffers;
    this->have_buffers = true;
    this->empty = this->alloc_buffers;
  }

  if (this->have_buffers) {
    update_state (this, SPA_NODE_STATE_PAUSED);
  } else {
    update_state (this, SPA_NODE_STATE_READY);
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_alloc_buffers (SpaNode         *node,
                                          uint32_t         port_id,
                                          SpaAllocParam  **params,
                                          uint32_t         n_params,
                                          SpaBuffer      **buffers,
                                          uint32_t        *n_buffers)
{
  SpaAudioTestSrc *this;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  if (!this->have_buffers)
    return SPA_RESULT_NO_BUFFERS;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_audiotestsrc_node_port_reuse_buffer (SpaNode         *node,
                                         uint32_t         port_id,
                                         uint32_t         buffer_id)
{
  SpaAudioTestSrc *this;
  ATSBuffer *b;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_buffers)
    return SPA_RESULT_NO_BUFFERS;

  if (buffer_id >= this->n_buffers)
    return SPA_RESULT_INVALID_BUFFER_ID;

  b = &this->alloc_buffers[buffer_id];
  if (!b->outstanding)
    return SPA_RESULT_OK;

  b->outstanding = false;
  b->next = this->empty;
  this->empty = b;

  if (b->next == NULL)
    update_idle_enabled (this, true);

  return SPA_RESULT_OK;
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
spa_audiotestsrc_node_port_push_input (SpaNode          *node,
                                       unsigned int      n_info,
                                       SpaPortInputInfo *info)
{
  return SPA_RESULT_INVALID_PORT;
}

static SpaResult
fill_buffer (SpaAudioTestSrc *this, ATSBuffer *b)
{
  uint8_t *p = b->ptr;
  size_t i;

  for (i = 0; i < b->size; i++) {
    p[i] = rand();
  }

  return SPA_RESULT_OK;
}

static int
audiotestsrc_idle (SpaPollNotifyData *data)
{
  SpaAudioTestSrc *this = data->user_data;
  ATSBuffer *empty;

  empty = this->empty;
  if (!empty) {
    update_idle_enabled (this, false);
    return 0;
  }

  fill_buffer (this, empty);

  this->empty = empty->next;
  empty->next = this->ready;
  this->ready = empty;
  this->ready_count++;
  send_have_output (this);

  return 0;
}

static SpaResult
spa_audiotestsrc_node_port_pull_output (SpaNode           *node,
                                        unsigned int       n_info,
                                        SpaPortOutputInfo *info)
{
  SpaAudioTestSrc *this;
  unsigned int i;
  bool have_error = false;

  if (node == NULL || node->handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaAudioTestSrc *) node->handle;

  for (i = 0; i < n_info; i++) {
    ATSBuffer *b;

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
    if (this->ready_count == 0) {
      info[i].status = SPA_RESULT_UNEXPECTED;
      have_error = true;
      continue;
    }

    b = this->ready;
    this->ready = b->next;
    this->ready_count--;
    b->outstanding = true;

    info[i].buffer_id = b->outbuf->id;
    info[i].status = SPA_RESULT_OK;
  }
  if (have_error)
    return SPA_RESULT_ERROR;

  return SPA_RESULT_OK;
}

static SpaResult
spa_audiotestsrc_node_port_push_event (SpaNode      *node,
                                       uint32_t      port_id,
                                       SpaNodeEvent *event)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static const SpaNode audiotestsrc_node = {
  NULL,
  sizeof (SpaNode),
  NULL,
  SPA_NODE_STATE_INIT,
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
audiotestsrc_clear (SpaHandle *handle)
{
  return SPA_RESULT_OK;
}

static SpaResult
audiotestsrc_init (const SpaHandleFactory  *factory,
                   SpaHandle               *handle,
                   const SpaDict           *info)
{
  SpaAudioTestSrc *this;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_audiotestsrc_get_interface;
  handle->clear = audiotestsrc_clear;

  this = (SpaAudioTestSrc *) handle;
  this->node = audiotestsrc_node;
  this->node.handle = handle;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  reset_audiotestsrc_props (&this->props[1]);

  this->idle.id = 0;
  this->idle.enabled = false;
  this->idle.fds = NULL;
  this->idle.n_fds = 0;
  this->idle.idle_cb = audiotestsrc_idle;
  this->idle.before_cb = NULL;
  this->idle.after_cb = NULL;
  this->idle.user_data = this;

  this->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                     SPA_PORT_INFO_FLAG_NO_REF;
  this->status.flags = SPA_PORT_STATUS_FLAG_NONE;

  this->node.state = SPA_NODE_STATE_CONFIGURE;

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
