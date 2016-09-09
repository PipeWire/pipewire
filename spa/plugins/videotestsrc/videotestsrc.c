/* Spa Video Test Source
 * Copyright (C) 2016 Axis Communications AB
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

#include <spa/node.h>
#include <spa/memory.h>
#include <spa/video/format.h>
#include <spa/debug.h>

#include <stdio.h>
#include <pthread.h>

#define MAX_BUFFERS     256
#define PIXEL_SIZE      3

#define CLEAR(x) memset(&(x), 0, sizeof(x))

#define STATE_GET_IMAGE_WIDTH(state)   state->current_format.info.raw.size.width
#define STATE_GET_IMAGE_HEIGHT(state)  state->current_format.info.raw.size.height

#define STATE_GET_IMAGE_SIZE(state)  \
  (PIXEL_SIZE * STATE_GET_IMAGE_WIDTH(state) * STATE_GET_IMAGE_HEIGHT(state))

#define STATE_GET_FRAME_INTERVAL_NS(state)  \
  ((uint64_t)state->current_format.info.raw.framerate.denom * 1000000000lu / \
   (uint64_t)state->current_format.info.raw.framerate.num)

typedef struct _SpaVideoTestSrc SpaVideoTestSrc;

typedef struct _VTSBuffer VTSBuffer;

struct _VTSBuffer {
  SpaBuffer buffer;
  SpaMeta metas[1];
  SpaMetaHeader header;
  SpaData datas[1];
  SpaBuffer *outbuf;
  bool outstanding;
  bool queued;
  VTSBuffer *next;
};

typedef struct {
  bool have_buffers;
  bool started;

  void *cookie;

  bool have_format;
  SpaFormatVideo query_format;
  SpaFormatVideo current_format;

  pthread_t thread;
  int next_sequence;

  int n_buffers;
  SpaMemory *alloc_mem;
  VTSBuffer *alloc_buffers;
  VTSBuffer *ready;
  uint32_t ready_count;
  pthread_mutex_t queue_mutex;
  pthread_cond_t queue_cond;

  SpaPortInfo info;
  SpaAllocParam *params[1];
  SpaAllocParamBuffers param_buffers;
  SpaPortStatus status;

} SpaVideoTestSrcState;

struct _SpaVideoTestSrc {
  SpaHandle handle;
  SpaNode node;

  SpaNodeEventCallback event_cb;
  void *user_data;

  SpaPollItem idle;

  SpaVideoTestSrcState state[1];
};

static int
spa_videotestsrc_draw (SpaVideoTestSrcState *state, VTSBuffer *b)
{
  SpaMemoryRef *mem_ref;
  SpaMemory *mem;
  char *ptr;

  mem_ref = &b->datas[0].mem.mem;
  spa_memory_ref (mem_ref);
  mem = spa_memory_find (mem_ref);
  ptr = spa_memory_ensure_ptr (mem);
  if (ptr == NULL) {
    return -1;
  }

  /* draw light gray */
  memset (ptr, 0xDF, STATE_GET_IMAGE_SIZE (state));

  spa_memory_unref (mem_ref);

  return 0;
}

static VTSBuffer *
find_queued_buffer (SpaVideoTestSrcState *state)
{
  int i;

  for (i = 0; i < state->n_buffers; i++) {
    VTSBuffer *b = &state->alloc_buffers[i];

    if (b->queued) {
      return b;
    }
  }

  return NULL;
}

static int
spa_videotestsrc_idle (SpaPollNotifyData *data)
{
  SpaVideoTestSrc *this = data->user_data;
  SpaNodeEvent event;
  SpaNodeEventHaveOutput ho;

  event.type = SPA_NODE_EVENT_TYPE_HAVE_OUTPUT;
  event.size = sizeof (ho);
  event.data = &ho;
  ho.port_id = 0;
  this->event_cb (&this->node, &event, this->user_data);

  return 0;
}

static SpaResult
update_idle_enabled (SpaVideoTestSrc *this, bool enabled)
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

static void *
loop (void *user_data)
{
  SpaVideoTestSrc *this = user_data;
  SpaVideoTestSrcState *state = &this->state[0];
  uint64_t last_pts = -1;

  while (state->started) {
    VTSBuffer *b;
    struct timespec ts;
    uint64_t now;
    uint64_t pts;

    pthread_mutex_lock (&state->queue_mutex);
    b = find_queued_buffer (state);
    while (b == NULL && state->started) {
      pthread_cond_wait (&state->queue_cond, &state->queue_mutex);
      b = find_queued_buffer (state);
    }
    pthread_mutex_unlock (&state->queue_mutex);

    if (!state->started)
      goto done;

    if (spa_videotestsrc_draw (state, b) < 0)
      goto done;

    if (!state->started)
      goto done;

    clock_gettime (CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000000000lu + (uint64_t)ts.tv_nsec;

    if (last_pts == -1) {
      pts = now;
    } else {
      uint64_t next_pts = last_pts + STATE_GET_FRAME_INTERVAL_NS (state);
      if (next_pts > now) {
        uint64_t diff = next_pts - now;

        ts.tv_sec = diff / 1000000000lu;
        ts.tv_nsec = diff % 1000000000lu;
        nanosleep (&ts, NULL);

        pts = next_pts;
      } else {
        pts = now;
      }
    }

    b->header.pts = pts;
    last_pts = pts;

    b->header.seq = state->next_sequence;
    state->next_sequence++;

    pthread_mutex_lock (&state->queue_mutex);
    b->queued = false;
    pthread_mutex_unlock (&state->queue_mutex);

    b->next = state->ready;
    state->ready = b;
    state->ready_count++;

    update_idle_enabled (this, true);
  }

done:
  return NULL;
}

static SpaResult
spa_videotestsrc_buffer_recycle (SpaVideoTestSrc *this, uint32_t buffer_id)
{
  SpaVideoTestSrcState *state = &this->state[0];
  VTSBuffer *b = &state->alloc_buffers[buffer_id];

  if (!b->outstanding)
    return SPA_RESULT_OK;

  pthread_mutex_lock (&state->queue_mutex);
  b->queued = true;
  pthread_cond_signal (&state->queue_cond);
  pthread_mutex_unlock (&state->queue_mutex);

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_clear_buffers (SpaVideoTestSrc *this)
{
  SpaVideoTestSrcState *state = &this->state[0];
  int i;

  if (!state->have_buffers)
    return SPA_RESULT_OK;

  for (i = 0; i < state->n_buffers; i++) {
    VTSBuffer *b;
    SpaMemory *mem;

    b = &state->alloc_buffers[i];
    if (b->outstanding) {
      fprintf (stderr, "queueing outstanding buffer %p\n", b);
      spa_videotestsrc_buffer_recycle (this, i);
    }
    mem = spa_memory_find (&b->datas[0].mem.mem);
    spa_memory_unref (&mem->mem);
  }
  if (state->alloc_mem)
    spa_memory_unref (&state->alloc_mem->mem);

  state->have_buffers = false;

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_use_buffers (SpaVideoTestSrc *this, SpaBuffer **buffers, uint32_t n_buffers)
{
  SpaVideoTestSrcState *state = &this->state[0];
  int i;

  state->n_buffers = n_buffers;

  if (state->alloc_mem)
    spa_memory_unref (&state->alloc_mem->mem);
  state->alloc_mem = spa_memory_alloc_size (SPA_MEMORY_POOL_LOCAL,
                                            NULL,
                                            sizeof (VTSBuffer) * state->n_buffers);
  state->alloc_buffers = spa_memory_ensure_ptr (state->alloc_mem);

  for (i = 0; i < state->n_buffers; i++) {
    VTSBuffer *b;
    SpaMemoryRef *mem_ref;
    SpaMemory *mem;
    SpaData *d = SPA_BUFFER_DATAS (buffers[i]);

    b = &state->alloc_buffers[i];
    b->buffer.mem.mem = state->alloc_mem->mem;
    b->buffer.mem.offset = sizeof (VTSBuffer) * i;
    b->buffer.mem.size = sizeof (VTSBuffer);
    b->buffer.id = SPA_ID_INVALID;
    b->outbuf = buffers[i];
    b->outstanding = true;

    fprintf (stderr, "import buffer %p\n", buffers[i]);

    mem_ref = &d[0].mem.mem;
    if (!(mem = spa_memory_find (mem_ref))) {
      fprintf (stderr, "invalid memory on buffer %p\n", buffers[i]);
      continue;
    }

    spa_videotestsrc_buffer_recycle (this, buffers[i]->id);
  }
  state->have_buffers = true;

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_alloc_buffers (SpaVideoTestSrc   *this,
                        SpaAllocParam  **params,
                        unsigned int     n_params,
                        SpaBuffer      **buffers,
                        unsigned int    *n_buffers)
{
  SpaVideoTestSrcState *state = &this->state[0];
  unsigned int i;

  if (state->have_buffers) {
    if (*n_buffers < state->n_buffers)
      return SPA_RESULT_NO_BUFFERS;

    *n_buffers = state->n_buffers;
    for (i = 0; i < state->n_buffers; i++) {
      buffers[i] = &state->alloc_buffers[i].buffer;
    }
    return SPA_RESULT_OK;
  }

  state->n_buffers = *n_buffers;

  if (state->alloc_mem)
    spa_memory_unref (&state->alloc_mem->mem);
  state->alloc_mem = spa_memory_alloc_with_fd (SPA_MEMORY_POOL_SHARED,
                                               NULL,
                                               sizeof (VTSBuffer) * state->n_buffers);
  state->alloc_buffers = spa_memory_ensure_ptr (state->alloc_mem);

  for (i = 0; i < state->n_buffers; i++) {
    VTSBuffer *b;
    SpaMemory *mem;

    b = &state->alloc_buffers[i];
    b->buffer.id = i;
    b->buffer.mem.mem = state->alloc_mem->mem;
    b->buffer.mem.offset = sizeof (VTSBuffer) * i;
    b->buffer.mem.size = sizeof (VTSBuffer);

    buffers[i] = &b->buffer;

    b->buffer.n_metas = 1;
    b->buffer.metas = offsetof (VTSBuffer, metas);
    b->buffer.n_datas = 1;
    b->buffer.datas = offsetof (VTSBuffer, datas);

    b->header.flags = 0;
    b->header.seq = 0;
    b->header.pts = 0;
    b->header.dts_offset = 0;

    b->metas[0].type = SPA_META_TYPE_HEADER;
    b->metas[0].offset = offsetof (VTSBuffer, header);
    b->metas[0].size = sizeof (b->header);

    mem = spa_memory_alloc_with_fd (SPA_MEMORY_POOL_SHARED, NULL,
        STATE_GET_IMAGE_SIZE (state));
    b->datas[0].mem.mem = mem->mem;
    b->datas[0].mem.offset = 0;
    b->datas[0].mem.size = STATE_GET_IMAGE_SIZE (state);
    b->datas[0].stride = PIXEL_SIZE * STATE_GET_IMAGE_WIDTH (state);

    b->outbuf = &b->buffer;
    b->outstanding = true;

    spa_videotestsrc_buffer_recycle (this, i);
  }

  state->have_buffers = true;

  return SPA_RESULT_OK;
}

static void
update_state (SpaVideoTestSrc *this, SpaNodeState state)
{
  SpaNodeEvent event;
  SpaNodeEventStateChange sc;

  if (this->node.state == state)
    return;

  this->node.state = state;

  event.type = SPA_NODE_EVENT_TYPE_STATE_CHANGE;
  event.data = &sc;
  event.size = sizeof (sc);
  sc.state = state;
  this->event_cb (&this->node, &event, this->user_data);
}

static SpaResult
spa_videotestsrc_start (SpaVideoTestSrc *this)
{
  SpaVideoTestSrcState *state = &this->state[0];

  if (state->started)
    return SPA_RESULT_OK;

  state->started = true;

  if (pthread_create (&state->thread, NULL, loop, this) != 0) {
    return SPA_RESULT_ERROR;
  }
  state->started = true;
  update_state (this, SPA_NODE_STATE_STREAMING);

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_pause (SpaVideoTestSrc *this)
{
  SpaVideoTestSrcState *state = &this->state[0];

  if (!state->started)
    return SPA_RESULT_OK;

  update_idle_enabled (this, false);

  state->started = false;
  pthread_join (state->thread, NULL);

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_get_props (SpaNode       *node,
                                 SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_node_set_props (SpaNode         *node,
                                 const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_node_send_command (SpaNode        *node,
                                    SpaNodeCommand *command)
{
  SpaVideoTestSrc *this;
  SpaResult res;

  if (node == NULL || node->handle == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVideoTestSrc *) node->handle;

  switch (command->type) {
    case SPA_NODE_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_NODE_COMMAND_START:
    {
      SpaVideoTestSrcState *state = &this->state[0];

      if (!state->have_format)
        return SPA_RESULT_NO_FORMAT;

      if (!state->have_buffers)
        return SPA_RESULT_NO_BUFFERS;

      if ((res = spa_videotestsrc_start (this)) < 0)
        return res;

      break;
    }
    case SPA_NODE_COMMAND_PAUSE:
    {
      SpaVideoTestSrcState *state = &this->state[0];

      if (!state->have_format)
        return SPA_RESULT_NO_FORMAT;

      if (!state->have_buffers)
        return SPA_RESULT_NO_BUFFERS;

      if ((res = spa_videotestsrc_pause (this)) < 0)
        return res;

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
spa_videotestsrc_node_set_event_callback (SpaNode              *node,
                                          SpaNodeEventCallback  event_cb,
                                          void                 *user_data)
{
  SpaVideoTestSrc *this;
  SpaNodeEvent event;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVideoTestSrc *) node->handle;

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

  update_state (this, SPA_NODE_STATE_CONFIGURE);

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_get_n_ports (SpaNode       *node,
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
spa_videotestsrc_node_get_port_ids (SpaNode       *node,
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
spa_videotestsrc_node_add_port (SpaNode        *node,
                                uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_node_remove_port (SpaNode        *node,
                                   uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_node_port_enum_formats (SpaNode          *node,
                                         uint32_t          port_id,
                                         SpaFormat       **format,
                                         const SpaFormat  *filter,
                                         void            **state)
{
  SpaVideoTestSrc *this;
  SpaVideoTestSrcState *src_state;
  int index;
  unsigned int idx;
  SpaVideoFormat video_format;
  SpaPropValue val;
  SpaProps *props;

  if (node == NULL || node->handle == NULL || format == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVideoTestSrc *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  src_state = &this->state[port_id];

  index = (*state == NULL ? 0 : *(int*)state);
  if (index != 0)
    return SPA_RESULT_ENUM_END;

  if (filter) {
    SpaResult res;
    const SpaPropInfo *pi;

    idx = spa_props_index_for_id (&filter->props, SPA_PROP_ID_VIDEO_FORMAT);
    if (idx == SPA_IDX_INVALID)
      return SPA_RESULT_ENUM_END;

    pi = &filter->props.prop_info[idx];
    if (pi->type != SPA_PROP_TYPE_UINT32)
      return SPA_RESULT_ENUM_END;

    res = spa_props_get_prop (&filter->props, idx, &val);
    if (res >= 0) {
      video_format = *((SpaVideoFormat *)val.value);
      if (video_format != SPA_VIDEO_FORMAT_RGB)
        return SPA_RESULT_ENUM_END;
    }

    spa_format_video_parse (filter, &src_state->query_format);
  } else {
    spa_format_video_init (SPA_MEDIA_TYPE_VIDEO,
                           SPA_MEDIA_SUBTYPE_RAW,
                           &src_state->query_format);
  }

  props = &src_state->query_format.format.props;
  idx = spa_props_index_for_id (props, SPA_PROP_ID_VIDEO_FORMAT);
  if (idx == SPA_IDX_INVALID)
    return SPA_RESULT_ENUM_END;
  video_format = SPA_VIDEO_FORMAT_RGB;
  val.value = &video_format;
  val.size = sizeof video_format;
  spa_props_set_prop (props, idx, &val);

  *format = &src_state->query_format.format;
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_port_set_format (SpaNode            *node,
                                       uint32_t            port_id,
                                       SpaPortFormatFlags  flags,
                                       const SpaFormat    *format)
{
  SpaVideoTestSrc *this;
  SpaVideoTestSrcState *state;
  SpaResult res;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVideoTestSrc *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (format == NULL) {
    state->have_format = false;
    state->have_buffers = false;
  } else {
    if ((res = spa_format_video_parse (format, &state->current_format)) < 0)
      return res;

    state->have_format = true;
  }

  if (state->have_format) {
    state->info.flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS |
                        SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
    state->info.maxbuffering = -1;
    state->info.latency = -1;
    state->info.n_params = 1;
    state->info.params = state->params;
    state->params[0] = &state->param_buffers.param;
    state->param_buffers.param.type = SPA_ALLOC_PARAM_TYPE_BUFFERS;
    state->param_buffers.param.size = sizeof (state->param_buffers);
    state->param_buffers.minsize = STATE_GET_IMAGE_SIZE (state);
    state->param_buffers.stride = PIXEL_SIZE * STATE_GET_IMAGE_WIDTH (state);
    state->param_buffers.min_buffers = 2;
    state->param_buffers.max_buffers = MAX_BUFFERS;
    state->param_buffers.align = 16;
    state->info.features = NULL;
    update_state (this, SPA_NODE_STATE_READY);
  }
  else
    update_state (this, SPA_NODE_STATE_CONFIGURE);

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_port_get_format (SpaNode          *node,
                                       uint32_t          port_id,
                                       const SpaFormat **format)
{
  SpaVideoTestSrc *this;
  SpaVideoTestSrcState *state;

  if (node == NULL || node->handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVideoTestSrc *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (!state->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = &state->current_format.format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_port_get_info (SpaNode            *node,
                                     uint32_t            port_id,
                                     const SpaPortInfo **info)
{
  SpaVideoTestSrc *this;

  if (node == NULL || node->handle == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVideoTestSrc *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  *info = &this->state[port_id].info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_port_get_props (SpaNode    *node,
                                      uint32_t    port_id,
                                      SpaProps  **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_node_port_set_props (SpaNode         *node,
                                      uint32_t         port_id,
                                      const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_videotestsrc_node_port_use_buffers (SpaNode         *node,
                                        uint32_t         port_id,
                                        SpaBuffer      **buffers,
                                        uint32_t         n_buffers)
{
  SpaVideoTestSrc *this;
  SpaVideoTestSrcState *state;
  SpaResult res;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVideoTestSrc *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (!state->have_format)
    return SPA_RESULT_NO_FORMAT;

  if (state->have_buffers) {
    if ((res = spa_videotestsrc_clear_buffers (this)) < 0)
      return res;
  }
  if (buffers != NULL) {
    if ((res = spa_videotestsrc_use_buffers (this, buffers, n_buffers)) < 0)
      return res;
  }

  if (state->have_buffers)
    update_state (this, SPA_NODE_STATE_PAUSED);
  else
    update_state (this, SPA_NODE_STATE_READY);

  return res;
}

static SpaResult
spa_videotestsrc_node_port_alloc_buffers (SpaNode         *node,
                                          uint32_t         port_id,
                                          SpaAllocParam  **params,
                                          unsigned int     n_params,
                                          SpaBuffer      **buffers,
                                          unsigned int    *n_buffers)
{
  SpaVideoTestSrc *this;
  SpaVideoTestSrcState *state;
  SpaResult res;

  if (node == NULL || node->handle == NULL || buffers == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVideoTestSrc *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (!state->have_format)
    return SPA_RESULT_NO_FORMAT;

  res = spa_videotestsrc_alloc_buffers (this, params, n_params, buffers, n_buffers);

  if (state->have_buffers)
    update_state (this, SPA_NODE_STATE_PAUSED);

  return res;
}

static SpaResult
spa_videotestsrc_node_port_reuse_buffer (SpaNode         *node,
                                         uint32_t         port_id,
                                         uint32_t         buffer_id)
{
  SpaVideoTestSrc *this;
  SpaVideoTestSrcState *state;
  SpaResult res;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVideoTestSrc *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  state = &this->state[port_id];

  if (!state->have_buffers)
    return SPA_RESULT_NO_BUFFERS;

  if (buffer_id >= state->n_buffers)
    return SPA_RESULT_INVALID_BUFFER_ID;

  res = spa_videotestsrc_buffer_recycle (this, buffer_id);

  return res;
}

static SpaResult
spa_videotestsrc_node_port_get_status (SpaNode              *node,
                                       uint32_t              port_id,
                                       const SpaPortStatus **status)
{
  SpaVideoTestSrc *this;

  if (node == NULL || node->handle == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVideoTestSrc *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  *status = &this->state[port_id].status;

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_port_push_input (SpaNode          *node,
                                       unsigned int      n_info,
                                       SpaPortInputInfo *info)
{
  return SPA_RESULT_INVALID_PORT;
}

static SpaResult
spa_videotestsrc_node_port_pull_output (SpaNode           *node,
                                        unsigned int       n_info,
                                        SpaPortOutputInfo *info)
{
  SpaVideoTestSrc *this;
  SpaVideoTestSrcState *state;
  unsigned int i;
  bool have_error = false;

  if (node == NULL || node->handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVideoTestSrc *) node->handle;

  for (i = 0; i < n_info; i++) {
    VTSBuffer *b;

    if (info[i].port_id != 0) {
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }
    state = &this->state[info[i].port_id];

    if (!state->have_format) {
      info[i].status = SPA_RESULT_NO_FORMAT;
      have_error = true;
      continue;
    }
    if (state->ready_count == 0) {
      info[i].status = SPA_RESULT_UNEXPECTED;
      have_error = true;
      continue;
    }

    b = state->ready;
    state->ready = b->next;
    state->ready_count--;

    b->outstanding = true;

    update_idle_enabled (this, false);

    info[i].buffer_id = b->outbuf->id;
    info[i].status = SPA_RESULT_OK;
  }
  if (have_error)
    return SPA_RESULT_ERROR;

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_node_port_push_event (SpaNode      *node,
                                       uint32_t      port_id,
                                       SpaNodeEvent *event)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}


static const SpaNode videotestsrc_node = {
  NULL,
  sizeof (SpaNode),
  NULL,
  SPA_NODE_STATE_INIT,
  spa_videotestsrc_node_get_props,
  spa_videotestsrc_node_set_props,
  spa_videotestsrc_node_send_command,
  spa_videotestsrc_node_set_event_callback,
  spa_videotestsrc_node_get_n_ports,
  spa_videotestsrc_node_get_port_ids,
  spa_videotestsrc_node_add_port,
  spa_videotestsrc_node_remove_port,
  spa_videotestsrc_node_port_enum_formats,
  spa_videotestsrc_node_port_set_format,
  spa_videotestsrc_node_port_get_format,
  spa_videotestsrc_node_port_get_info,
  spa_videotestsrc_node_port_get_props,
  spa_videotestsrc_node_port_set_props,
  spa_videotestsrc_node_port_use_buffers,
  spa_videotestsrc_node_port_alloc_buffers,
  spa_videotestsrc_node_port_reuse_buffer,
  spa_videotestsrc_node_port_get_status,
  spa_videotestsrc_node_port_push_input,
  spa_videotestsrc_node_port_pull_output,
  spa_videotestsrc_node_port_push_event,
};

static SpaResult
spa_videotestsrc_get_interface (SpaHandle               *handle,
                                uint32_t                 interface_id,
                                void                   **interface)
{
  SpaVideoTestSrc *this;

  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaVideoTestSrc *) handle;

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
spa_videotestsrc_clear (SpaHandle *handle)
{
  SpaVideoTestSrc *this = (SpaVideoTestSrc *) handle;

  pthread_mutex_destroy (&this->state[0].queue_mutex);
  pthread_cond_destroy (&this->state[0].queue_cond);

  return SPA_RESULT_OK;
}

static SpaResult
spa_videotestsrc_init (const SpaHandleFactory  *factory,
                       SpaHandle               *handle,
                       const SpaDict           *info)
{
  SpaVideoTestSrc *this;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_videotestsrc_get_interface;
  handle->clear = spa_videotestsrc_clear;

  this = (SpaVideoTestSrc *) handle;
  this->node = videotestsrc_node;
  this->node.handle = handle;

  this->idle.id = 0;
  this->idle.enabled = false;
  this->idle.fds = NULL;
  this->idle.n_fds = 0;
  this->idle.idle_cb = spa_videotestsrc_idle;
  this->idle.before_cb = NULL;
  this->idle.after_cb = NULL;
  this->idle.user_data = this;

  this->state[0].info.flags = SPA_PORT_INFO_FLAG_NONE;
  this->state[0].status.flags = SPA_PORT_STATUS_FLAG_NONE;

  pthread_mutex_init (&this->state[0].queue_mutex, NULL);
  pthread_cond_init (&this->state[0].queue_cond, NULL);

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo spa_videotestsrc_interfaces[] =
{
  { SPA_INTERFACE_ID_NODE,
    SPA_INTERFACE_ID_NODE_NAME,
    SPA_INTERFACE_ID_NODE_DESCRIPTION,
  },
};

static SpaResult
spa_videotestsrc_enum_interface_info (const SpaHandleFactory  *factory,
                                      const SpaInterfaceInfo **info,
                                      void                   **state)
{
  int index;

  if (factory == NULL || info == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      *info = &spa_videotestsrc_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *(int*)state = ++index;
  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_videotestsrc_factory =
{ "videotestsrc",
  NULL,
  sizeof (SpaVideoTestSrc),
  spa_videotestsrc_init,
  spa_videotestsrc_enum_interface_info,
};
