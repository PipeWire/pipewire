/* Pinos
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#include <unistd.h>
#include <sys/socket.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>
#include <time.h>

#include "spa/lib/debug.h"

#include "pinos/client/pinos.h"
#include "pinos/client/interfaces.h"
#include "pinos/client/protocol-native.h"
#include "pinos/client/array.h"
#include "pinos/client/connection.h"
#include "pinos/client/context.h"
#include "pinos/client/stream.h"
#include "pinos/client/transport.h"
#include "pinos/client/utils.h"

#define MAX_BUFFER_SIZE 4096
#define MAX_FDS         32
#define MAX_INPUTS      64
#define MAX_OUTPUTS     64

typedef struct {
  uint32_t id;
  int fd;
  uint32_t flags;
  void *ptr;
  uint32_t offset;
  uint32_t size;
} MemId;

typedef struct {
  uint32_t id;
  bool used;
  void *buf_ptr;
  SpaBuffer *buf;
} BufferId;

typedef struct
{
  PinosStream this;

  SpaNodeState node_state;

  uint32_t n_possible_formats;
  SpaFormat **possible_formats;

  SpaFormat *format;
  SpaPortInfo port_info;
  SpaDirection direction;
  uint32_t port_id;
  uint32_t pending_seq;

  PinosStreamFlags flags;

  PinosStreamMode mode;

  int rtfd;
  SpaSource *rtsocket_source;

  PinosProxy *node_proxy;
  bool disconnecting;
  PinosListener node_proxy_destroy;

  PinosTransport *trans;

  SpaSource *timeout_source;

  PinosArray mem_ids;
  PinosArray buffer_ids;
  bool in_order;

  int64_t last_ticks;
  int32_t last_rate;
  int64_t last_monotonic;
} PinosStreamImpl;

static void
clear_memid (MemId *mid)
{
  if (mid->ptr != NULL)
    munmap (mid->ptr, mid->size + mid->offset);
  mid->ptr = NULL;
  close (mid->fd);
}

static void
clear_mems (PinosStream *stream)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  MemId *mid;

  pinos_array_for_each (mid, &impl->mem_ids)
    clear_memid (mid);
  impl->mem_ids.size = 0;
}

static void
clear_buffers (PinosStream *stream)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  BufferId *bid;

  pinos_array_for_each (bid, &impl->buffer_ids) {
    pinos_signal_emit (&stream->remove_buffer, stream, bid->id);
    free (bid->buf);
    bid->buf = NULL;
    bid->used = false;
  }
  impl->buffer_ids.size = 0;
  impl->in_order = true;
}

static void
stream_set_state (PinosStream      *stream,
                  PinosStreamState  state,
                  char             *error)
{
  if (stream->state != state) {
    if (stream->error)
      free (stream->error);
    stream->error = error;
    stream->state = state;
    pinos_signal_emit (&stream->state_changed, stream);
  }
}

/**
 * pinos_stream_state_as_string:
 * @state: a #PinosStreamState
 *
 * Return the string representation of @state.
 *
 * Returns: the string representation of @state.
 */
const char *
pinos_stream_state_as_string (PinosStreamState state)
{
  switch (state) {
    case PINOS_STREAM_STATE_ERROR:
      return "error";
    case PINOS_STREAM_STATE_UNCONNECTED:
      return "unconnected";
    case PINOS_STREAM_STATE_CONNECTING:
      return "connecting";
    case PINOS_STREAM_STATE_CONFIGURE:
      return "configure";
    case PINOS_STREAM_STATE_READY:
      return "ready";
    case PINOS_STREAM_STATE_PAUSED:
      return "paused";
    case PINOS_STREAM_STATE_STREAMING:
      return "streaming";
  }
  return "invalid-state";
}

/**
 * pinos_stream_new:
 * @context: a #PinosContext
 * @name: a stream name
 * @properties: (transfer full): stream properties
 *
 * Make a new unconnected #PinosStream
 *
 * Returns: a new unconnected #PinosStream
 */
PinosStream *
pinos_stream_new (PinosContext    *context,
                  const char      *name,
                  PinosProperties *props)
{
  PinosStreamImpl *impl;
  PinosStream *this;

  impl = calloc (1, sizeof (PinosStreamImpl));
  if (impl == NULL)
    return NULL;

  this = &impl->this;
  pinos_log_debug ("stream %p: new", impl);

  if (props == NULL) {
    props = pinos_properties_new ("media.name", name, NULL);
  } else if (!pinos_properties_get (props, "media.name")) {
    pinos_properties_set (props, "media.name", name);
  }
  if (props == NULL)
    goto no_mem;

  this->properties = props;

  this->context = context;
  this->name = strdup (name);

  pinos_signal_init (&this->destroy_signal);
  pinos_signal_init (&this->state_changed);
  pinos_signal_init (&this->format_changed);
  pinos_signal_init (&this->add_buffer);
  pinos_signal_init (&this->remove_buffer);
  pinos_signal_init (&this->new_buffer);
  pinos_signal_init (&this->need_buffer);

  this->state = PINOS_STREAM_STATE_UNCONNECTED;

  impl->node_state = SPA_NODE_STATE_INIT;
  pinos_array_init (&impl->mem_ids, 64);
  pinos_array_ensure_size (&impl->mem_ids, sizeof (MemId) * 64);
  pinos_array_init (&impl->buffer_ids, 32);
  pinos_array_ensure_size (&impl->buffer_ids, sizeof (BufferId) * 64);
  impl->pending_seq = SPA_ID_INVALID;

  spa_list_insert (&context->stream_list, &this->link);

  return this;

no_mem:
  free (impl);
  return NULL;
}

static void
unhandle_socket (PinosStream *stream)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);

  if (impl->rtsocket_source) {
    pinos_loop_destroy_source (stream->context->loop, impl->rtsocket_source);
    impl->rtsocket_source = NULL;
  }
  if (impl->timeout_source) {
    pinos_loop_destroy_source (stream->context->loop, impl->timeout_source);
    impl->timeout_source = NULL;
  }
}

void
pinos_stream_destroy (PinosStream *stream)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  int i;

  pinos_log_debug ("stream %p: destroy", stream);

  pinos_signal_emit (&stream->destroy_signal, stream);

  unhandle_socket (stream);

  spa_list_remove (&stream->link);

  if (impl->node_proxy)
    pinos_signal_remove (&impl->node_proxy_destroy);

  if (impl->possible_formats) {
    for (i = 0; i < impl->n_possible_formats; i++) {
      free (impl->possible_formats[i]);
    }
    free (impl->possible_formats);
  }

  if (impl->format)
    free (impl->format);

  if (stream->error)
    free (stream->error);

  clear_buffers (stream);
  pinos_array_clear (&impl->buffer_ids);

  clear_mems (stream);
  pinos_array_clear (&impl->mem_ids);

  if (stream->properties)
    pinos_properties_free (stream->properties);

  if (impl->trans)
    pinos_transport_destroy (impl->trans);

  if (stream->name)
    free (stream->name);

  free (impl);
}

static void
add_node_update (PinosStream *stream, uint32_t change_mask, bool flush)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  uint32_t max_input_ports = 0, max_output_ports = 0;

  if (change_mask & PINOS_MESSAGE_NODE_UPDATE_MAX_INPUTS)
    max_input_ports = impl->direction == SPA_DIRECTION_INPUT ? 1 : 0;
  if (change_mask & PINOS_MESSAGE_NODE_UPDATE_MAX_OUTPUTS)
    max_output_ports = impl->direction == SPA_DIRECTION_OUTPUT ? 1 : 0;

  pinos_client_node_do_update (impl->node_proxy,
                               change_mask,
                               max_input_ports,
                               max_output_ports,
                               NULL);
}

static void
add_state_change (PinosStream *stream, SpaNodeState state, bool flush)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);

  if (impl->node_state != state) {
    impl->node_state = state;
    pinos_client_node_do_state_change (impl->node_proxy,
                                       state);
  }
}

static void
add_port_update (PinosStream *stream, uint32_t change_mask, bool flush)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);

  pinos_client_node_do_port_update (impl->node_proxy,
                                    impl->direction,
                                    impl->port_id,
                                    change_mask,
                                    impl->n_possible_formats,
                                    (const SpaFormat **) impl->possible_formats,
                                    (const SpaFormat *) impl->format,
                                    NULL,
                                    &impl->port_info);
}

static inline void
send_need_input (PinosStream *stream)
{
#if 0
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  SpaEventNodeNeedInput ni;
  uint64_t cmd = 1;

  pinos_log_debug ("stream %p: need input", stream);

  ni.event.type = SPA_EVENT_NODE_NEED_INPUT;
  ni.event.size = sizeof (ni);
  pinos_transport_add_event (impl->trans, &ni.event);
  write (impl->rtfd, &cmd, 8);
#endif
}

static inline void
send_have_output (PinosStream *stream)
{
#if 0
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  SpaEventNodeHaveOutput ho;
  uint64_t cmd = 1;

  pinos_log_debug ("stream %p: have output", stream);

  ho.event.type = SPA_EVENT_NODE_HAVE_OUTPUT;
  ho.event.size = sizeof (ho);
  pinos_transport_add_event (impl->trans, &ho.event);
  write (impl->rtfd, &cmd, 8);
#endif
}

static void
add_request_clock_update (PinosStream *stream, bool flush)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  SpaEventNodeRequestClockUpdate rcu =
    SPA_EVENT_NODE_REQUEST_CLOCK_UPDATE_INIT (stream->context->type.event_node.RequestClockUpdate,
                                              SPA_EVENT_NODE_REQUEST_CLOCK_UPDATE_TIME, 0, 0);

  pinos_client_node_do_event (impl->node_proxy, (SpaEvent*)&rcu);
}

static void
add_async_complete (PinosStream  *stream,
                    uint32_t      seq,
                    SpaResult     res,
                    bool          flush)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  SpaEventNodeAsyncComplete ac =
    SPA_EVENT_NODE_ASYNC_COMPLETE_INIT (stream->context->type.event_node.AsyncComplete,
                                        seq, res);
  pinos_client_node_do_event (impl->node_proxy, (SpaEvent*)&ac);
}

static void
do_node_init (PinosStream *stream)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);

  add_node_update (stream, PINOS_MESSAGE_NODE_UPDATE_MAX_INPUTS |
                           PINOS_MESSAGE_NODE_UPDATE_MAX_OUTPUTS,
                           false);

  impl->port_info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
  add_port_update (stream, PINOS_MESSAGE_PORT_UPDATE_POSSIBLE_FORMATS |
                           PINOS_MESSAGE_PORT_UPDATE_INFO,
                           false);
  add_state_change (stream, SPA_NODE_STATE_CONFIGURE, true);
}

static void
on_timeout (SpaSource *source,
            void      *data)
{
  PinosStream *stream = data;

  add_request_clock_update (stream, true);
}

static MemId *
find_mem (PinosStream *stream, uint32_t id)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  MemId *mid;

  pinos_array_for_each (mid, &impl->mem_ids) {
    if (mid->id == id)
      return mid;
  }
  return NULL;
}

static BufferId *
find_buffer (PinosStream *stream, uint32_t id)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);

  if (impl->in_order && pinos_array_check_index (&impl->buffer_ids, id, BufferId)) {
    return pinos_array_get_unchecked (&impl->buffer_ids, id, BufferId);
  } else {
    BufferId *bid;

    pinos_array_for_each (bid, &impl->buffer_ids) {
      if (bid->id == id)
        return bid;
    }
  }
  return NULL;
}

static void
handle_rtnode_event (PinosStream  *stream,
                     SpaEvent     *event)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  PinosContext *context = impl->this.context;

  if (SPA_EVENT_TYPE (event) == context->type.event_node.HaveOutput) {
    int i;

    //pinos_log_debug ("stream %p: have output", stream);

    for (i = 0; i < impl->trans->area->n_inputs; i++) {
      SpaPortInput *input = &impl->trans->inputs[i];

      if (input->buffer_id == SPA_ID_INVALID)
        continue;

      pinos_signal_emit (&stream->new_buffer, stream, input->buffer_id);
      input->buffer_id = SPA_ID_INVALID;
    }
    send_need_input (stream);
  }
  else if (SPA_EVENT_TYPE (event) == context->type.event_node.NeedInput) {
    //pinos_log_debug ("stream %p: need input", stream);
    pinos_signal_emit (&stream->need_buffer, stream);
  }
  else if (SPA_EVENT_TYPE (event) == context->type.event_node.ReuseBuffer) {
    SpaEventNodeReuseBuffer *p = (SpaEventNodeReuseBuffer *) event;
    BufferId *bid;

    if (p->body.port_id.value != impl->port_id)
      return;
    if (impl->direction != SPA_DIRECTION_OUTPUT)
      return;

    if ((bid = find_buffer (stream, p->body.buffer_id.value)) && bid->used) {
      bid->used = false;
      pinos_signal_emit (&stream->new_buffer, stream, p->body.buffer_id.value);
    }
  }
  else {
    pinos_log_warn ("unexpected node event %d", SPA_EVENT_TYPE (event));
  }
}

static void
on_rtsocket_condition (SpaSource    *source,
                       int           fd,
                       SpaIO         mask,
                       void         *data)
{
  PinosStream *stream = data;
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);

  if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
    pinos_log_warn ("got error");
    unhandle_socket (stream);
    return;
  }

  if (mask & SPA_IO_IN) {
    SpaEvent event;
    uint64_t cmd;

    read (impl->rtfd, &cmd, 8);

    while (pinos_transport_next_event (impl->trans, &event) == SPA_RESULT_OK) {
      SpaEvent *ev = alloca (SPA_POD_SIZE (&event));
      pinos_transport_parse_event (impl->trans, ev);
      handle_rtnode_event (stream, ev);
    }
  }
}

static void
handle_socket (PinosStream *stream, int rtfd)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  struct timespec interval, start;

  impl->rtfd = rtfd;
  impl->rtsocket_source = pinos_loop_add_io (stream->context->loop,
                                             impl->rtfd,
                                             SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP,
                                             true,
                                             on_rtsocket_condition,
                                             stream);

  impl->timeout_source = pinos_loop_add_timer (stream->context->loop,
                                               on_timeout,
                                               stream);
  start.tv_sec = 0;
  start.tv_nsec = 100000000;
  interval.tv_sec = 0;
  interval.tv_nsec = 100000000;
  pinos_loop_update_timer (stream->context->loop,
                           impl->timeout_source,
                           &start,
                           &interval,
                           false);
  return;
}

static void
handle_node_event (PinosStream    *stream,
                   const SpaEvent *event)
{
  pinos_log_warn ("unhandled node event %d", SPA_EVENT_TYPE (event));
}

static bool
handle_node_command (PinosStream      *stream,
                     uint32_t          seq,
                     const SpaCommand *command)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  PinosContext *context = stream->context;

  if (SPA_COMMAND_TYPE (command) == context->type.command_node.Pause) {
    pinos_log_debug ("stream %p: pause %d", stream, seq);

    add_state_change (stream, SPA_NODE_STATE_PAUSED, false);
    add_async_complete (stream, seq, SPA_RESULT_OK, true);
    stream_set_state (stream, PINOS_STREAM_STATE_PAUSED, NULL);
  }
  else if (SPA_COMMAND_TYPE (command) == context->type.command_node.Start) {
    pinos_log_debug ("stream %p: start %d", stream, seq);
    add_state_change (stream, SPA_NODE_STATE_STREAMING, false);
    add_async_complete (stream, seq, SPA_RESULT_OK, true);

    if (impl->direction == SPA_DIRECTION_INPUT)
      send_need_input (stream);

    stream_set_state (stream, PINOS_STREAM_STATE_STREAMING, NULL);
  }
  else if (SPA_COMMAND_TYPE (command) == context->type.command_node.ClockUpdate) {
    SpaCommandNodeClockUpdate *cu = (SpaCommandNodeClockUpdate *) command;
    if (cu->body.flags.value & SPA_COMMAND_NODE_CLOCK_UPDATE_FLAG_LIVE) {
      pinos_properties_set (stream->properties,
                        "pinos.latency.is-live", "1");
      pinos_properties_setf (stream->properties,
                        "pinos.latency.min", "%"PRId64, cu->body.latency.value);
    }
    impl->last_ticks = cu->body.ticks.value;
    impl->last_rate = cu->body.rate.value;
    impl->last_monotonic = cu->body.monotonic_time.value;
  }
  else {
    pinos_log_warn ("unhandled node command %d", SPA_COMMAND_TYPE (command));
    add_async_complete (stream, seq, SPA_RESULT_NOT_IMPLEMENTED, true);
  }
  return true;
}

static void
client_node_done (void              *object,
                  int                datafd)
{
  PinosProxy *proxy = object;
  PinosStream *stream = proxy->user_data;

  pinos_log_info ("strean %p: create client node done with fd %d", stream, datafd);
  handle_socket (stream, datafd);
  do_node_init (stream);
}

static void
client_node_event (void           *object,
                   const SpaEvent *event)
{
  PinosProxy *proxy = object;
  PinosStream *stream = proxy->user_data;
  handle_node_event (stream, event);
}

static void
client_node_add_port (void              *object,
                      uint32_t           seq,
                      SpaDirection       direction,
                      uint32_t           port_id)
{
  pinos_log_warn ("add port not supported");
}

static void
client_node_remove_port (void              *object,
                         uint32_t           seq,
                         SpaDirection       direction,
                         uint32_t           port_id)
{
  pinos_log_warn ("remove port not supported");
}

static void
client_node_set_format (void              *object,
                        uint32_t           seq,
                        SpaDirection       direction,
                        uint32_t           port_id,
                        SpaPortFormatFlags flags,
                        const SpaFormat   *format)
{
  PinosProxy *proxy = object;
  PinosStream *stream = proxy->user_data;
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);

  if (impl->format)
    free (impl->format);
  impl->format = spa_format_copy (format);
  impl->pending_seq = seq;

  pinos_signal_emit (&stream->format_changed, stream, impl->format);
  stream_set_state (stream, PINOS_STREAM_STATE_READY, NULL);
}

static void
client_node_set_property (void       *object,
                          uint32_t    seq,
                          uint32_t    id,
                          uint32_t    size,
                          const void *value)
{
  pinos_log_warn ("set property not implemented");
}

static void
client_node_add_mem (void              *object,
                     SpaDirection       direction,
                     uint32_t           port_id,
                     uint32_t           mem_id,
                     SpaDataType        type,
                     int                memfd,
                     uint32_t           flags,
                     uint32_t           offset,
                     uint32_t           size)
{
  PinosProxy *proxy = object;
  PinosStream *stream = proxy->user_data;
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  MemId *m;

  m = find_mem (stream, mem_id);
  if (m) {
    pinos_log_debug ("update mem %u, fd %d, flags %d, off %d, size %d",
        mem_id, memfd, flags, offset, size);
    clear_memid (m);
  } else {
    m = pinos_array_add (&impl->mem_ids, sizeof (MemId));
    pinos_log_debug ("add mem %u, fd %d, flags %d, off %d, size %d",
        mem_id, memfd, flags, offset, size);
  }
  m->id = mem_id;
  m->fd = memfd;
  m->flags = flags;
  m->ptr = NULL;
  m->offset = offset;
  m->size = size;
}

static void
client_node_use_buffers (void                  *object,
                         uint32_t               seq,
                         SpaDirection           direction,
                         uint32_t               port_id,
                         uint32_t               n_buffers,
                         PinosClientNodeBuffer *buffers)
{
  PinosProxy *proxy = object;
  PinosStream *stream = proxy->user_data;
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  BufferId *bid;
  uint32_t i, j, len;
  SpaBuffer *b;

  /* clear previous buffers */
  clear_buffers (stream);

  for (i = 0; i < n_buffers; i++) {
    off_t offset;

    MemId *mid = find_mem (stream, buffers[i].mem_id);
    if (mid == NULL) {
      pinos_log_warn ("unknown memory id %u", buffers[i].mem_id);
      continue;
    }

    if (mid->ptr == NULL) {
      mid->ptr = mmap (NULL, mid->size + mid->offset, PROT_READ | PROT_WRITE, MAP_SHARED, mid->fd, 0);
      if (mid->ptr == MAP_FAILED) {
        mid->ptr = NULL;
        pinos_log_warn ("Failed to mmap memory %d %p: %s", mid->size, mid, strerror (errno));
        continue;
      }
      mid->ptr = SPA_MEMBER (mid->ptr, mid->offset, void);
    }
    len = pinos_array_get_len (&impl->buffer_ids, BufferId);
    bid = pinos_array_add (&impl->buffer_ids, sizeof (BufferId));
    bid->used = false;

    b = buffers[i].buffer;

    bid->buf_ptr = SPA_MEMBER (mid->ptr, buffers[i].offset, void);
    {
      size_t size;

      size = sizeof (SpaBuffer);
      for (j = 0; j < buffers[i].buffer->n_metas; j++)
        size += sizeof (SpaMeta);
      for (j = 0; j < buffers[i].buffer->n_datas; j++)
        size += sizeof (SpaData);

      b = bid->buf = malloc (size);
      memcpy (b, buffers[i].buffer, sizeof (SpaBuffer));

      b->metas = SPA_MEMBER (b, sizeof (SpaBuffer), SpaMeta);
      b->datas = SPA_MEMBER (b->metas, sizeof(SpaMeta) * b->n_metas, SpaData);
    }
    bid->id = b->id;

    if (bid->id != len) {
      pinos_log_warn ("unexpected id %u found, expected %u", bid->id, len);
      impl->in_order = false;
    }
    pinos_log_debug ("add buffer %d %d %u", mid->id, bid->id, buffers[i].offset);

    offset = 0;
    for (j = 0; j < b->n_metas; j++) {
      SpaMeta *m = &b->metas[j];
      memcpy (m, &buffers[i].buffer->metas[j], sizeof (SpaMeta));
      m->data = SPA_MEMBER (bid->buf_ptr, offset, void);
      offset += m->size;
    }

    for (j = 0; j < b->n_datas; j++) {
      SpaData *d = &b->datas[j];

      memcpy (d, &buffers[i].buffer->datas[j], sizeof (SpaData));
      d->chunk = SPA_MEMBER (bid->buf_ptr, offset + sizeof (SpaChunk) * j, SpaChunk);

      switch (d->type) {
        case SPA_DATA_TYPE_ID:
        {
          MemId *bmid = find_mem (stream, SPA_PTR_TO_UINT32 (d->data));
          d->type = SPA_DATA_TYPE_MEMFD;
          d->data = NULL;
          d->fd = bmid->fd;
          pinos_log_debug (" data %d %u -> fd %d", j, bmid->id, bmid->fd);
          break;
        }
        case SPA_DATA_TYPE_MEMPTR:
        {
          d->data = SPA_MEMBER (bid->buf_ptr, SPA_PTR_TO_INT (d->data), void);
          d->fd = -1;
          pinos_log_debug (" data %d %u -> mem %p", j, bid->id, d->data);
          break;
        }
        default:
          pinos_log_warn ("unknown buffer data type %d", d->type);
          break;
      }
    }
    pinos_signal_emit (&stream->add_buffer, stream, bid->id);
  }

  if (n_buffers) {
    add_state_change (stream, SPA_NODE_STATE_PAUSED, false);
  } else {
    clear_mems (stream);
    add_state_change (stream, SPA_NODE_STATE_READY, false);
  }
  add_async_complete (stream, seq, SPA_RESULT_OK, true);

  if (n_buffers)
    stream_set_state (stream, PINOS_STREAM_STATE_PAUSED, NULL);
  else
    stream_set_state (stream, PINOS_STREAM_STATE_READY, NULL);
}

static void
client_node_node_command (void             *object,
                          uint32_t          seq,
                          const SpaCommand *command)
{
  PinosProxy *proxy = object;
  PinosStream *stream = proxy->user_data;
  handle_node_command (stream, seq, command);
}

static void
client_node_port_command (void             *object,
                          uint32_t          port_id,
                          const SpaCommand *command)
{
  pinos_log_warn ("port command not supported");
}

static void
client_node_transport (void              *object,
                       int                memfd,
                       uint32_t           offset,
                       uint32_t           size)
{
  PinosProxy *proxy = object;
  PinosStream *stream = proxy->user_data;
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  PinosTransportInfo info;

  info.memfd =  memfd;
  if (info.memfd == -1)
    return;
  info.offset = offset;
  info.size = size;

  if (impl->trans)
    pinos_transport_destroy (impl->trans);
  impl->trans = pinos_transport_new_from_info (&info);

  pinos_log_debug ("transport update %d %p", impl->rtfd, impl->trans);
}

static const PinosClientNodeEvents client_node_events = {
  &client_node_done,
  &client_node_event,
  &client_node_add_port,
  &client_node_remove_port,
  &client_node_set_format,
  &client_node_set_property,
  &client_node_add_mem,
  &client_node_use_buffers,
  &client_node_node_command,
  &client_node_port_command,
  &client_node_transport
};

static void
on_node_proxy_destroy (PinosListener *listener,
                       PinosProxy    *proxy)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (listener, PinosStreamImpl, node_proxy_destroy);
  PinosStream *this = &impl->this;

  impl->disconnecting = false;
  impl->node_proxy = NULL;
  pinos_signal_remove (&impl->node_proxy_destroy);
  stream_set_state (this, PINOS_STREAM_STATE_UNCONNECTED, NULL);
}

/**
 * pinos_stream_connect:
 * @stream: a #PinosStream
 * @direction: the stream direction
 * @mode: a #PinosStreamMode
 * @port_path: the port path to connect to or %NULL to get the default port
 * @flags: a #PinosStreamFlags
 * @possible_formats: (transfer full): a #GPtrArray with possible accepted formats
 *
 * Connect @stream for input or output on @port_path.
 *
 * When @mode is #PINOS_STREAM_MODE_BUFFER, you should connect to the new-buffer
 * signal and use pinos_stream_capture_buffer() to get the latest metadata and
 * data.
 *
 * Returns: %true on success.
 */
bool
pinos_stream_connect (PinosStream      *stream,
                      PinosDirection    direction,
                      PinosStreamMode   mode,
                      const char       *port_path,
                      PinosStreamFlags  flags,
                      uint32_t          n_possible_formats,
                      SpaFormat       **possible_formats)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);

  impl->direction = direction == PINOS_DIRECTION_INPUT ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT;
  impl->port_id = 0;
  impl->mode = mode;

  impl->flags = flags;

  impl->n_possible_formats = n_possible_formats;
  impl->possible_formats = possible_formats;

  stream_set_state (stream, PINOS_STREAM_STATE_CONNECTING, NULL);

  if (stream->properties == NULL)
    stream->properties = pinos_properties_new (NULL, NULL);
  if (port_path)
    pinos_properties_set (stream->properties,
                          "pinos.target.node", port_path);

  impl->node_proxy = pinos_proxy_new (stream->context,
                                      SPA_ID_INVALID,
                                      stream->context->type.client_node);
  if (impl->node_proxy == NULL)
    return false;

  pinos_signal_add (&impl->node_proxy->destroy_signal,
                    &impl->node_proxy_destroy,
                    on_node_proxy_destroy);

  impl->node_proxy->user_data = stream;
  impl->node_proxy->implementation = &client_node_events;

  pinos_core_do_create_client_node (stream->context->core_proxy,
                                    "client-node",
                                    &stream->properties->dict,
                                    impl->node_proxy->id);
  return true;
}

/**
 * pinos_stream_finish_format:
 * @stream: a #PinosStream
 * @res: a #SpaResult
 * @params: an array of pointers to #SpaAllocParam
 * @n_params: number of elements in @params
 *
 * Complete the negotiation process with result code @res.
 *
 * This function should be called after notification of the format.

 * When @res indicates success, @params contain the parameters for the
 * allocation state.
 *
 * Returns: %true on success
 */
bool
pinos_stream_finish_format (PinosStream     *stream,
                            SpaResult        res,
                            SpaAllocParam  **params,
                            uint32_t         n_params)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);

  impl->port_info.params = params;
  impl->port_info.n_params = n_params;

  if (SPA_RESULT_IS_OK (res)) {
    add_port_update (stream, PINOS_MESSAGE_PORT_UPDATE_INFO |
                             PINOS_MESSAGE_PORT_UPDATE_FORMAT,
                             false);
    if (impl->format) {
      add_state_change (stream, SPA_NODE_STATE_READY, false);
    } else {
      clear_buffers (stream);
      add_state_change (stream, SPA_NODE_STATE_CONFIGURE, false);
    }
  }
  impl->port_info.params = NULL;
  impl->port_info.n_params = 0;

  add_async_complete (stream, impl->pending_seq, res, true);

  impl->pending_seq = SPA_ID_INVALID;

  return true;
}

/**
 * pinos_stream_start:
 * @stream: a #PinosStream
 *
 * Start capturing from @stream.
 *
 * Returns: %true on success.
 */
bool
pinos_stream_start (PinosStream     *stream)
{
  return true;
}

/**
 * pinos_stream_stop:
 * @stream: a #PinosStream
 *
 * Stop capturing from @stream.
 *
 * Returns: %true on success.
 */
bool
pinos_stream_stop (PinosStream *stream)
{
  return true;
}

/**
 * pinos_stream_disconnect:
 * @stream: a #PinosStream
 *
 * Disconnect @stream.
 *
 * Returns: %true on success
 */
bool
pinos_stream_disconnect (PinosStream *stream)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);

  impl->disconnecting = true;

  unhandle_socket (stream);

  pinos_client_node_do_destroy (impl->node_proxy);

  return true;
}

bool
pinos_stream_get_time (PinosStream *stream,
                       PinosTime   *time)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  int64_t elapsed;
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  time->now = SPA_TIMESPEC_TO_TIME (&ts);
  elapsed = (time->now - impl->last_monotonic) / 1000;

  time->ticks = impl->last_ticks + (elapsed * impl->last_rate) / SPA_USEC_PER_SEC;
  time->rate = impl->last_rate;

  return true;
}

/**
 * pinos_stream_get_empty_buffer:
 * @stream: a #PinosStream
 *
 * Get the id of an empty buffer that can be filled
 *
 * Returns: the id of an empty buffer or #SPA_ID_INVALID when no buffer is
 * available.
 */
uint32_t
pinos_stream_get_empty_buffer (PinosStream *stream)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  BufferId *bid;

  pinos_array_for_each (bid, &impl->buffer_ids) {
    if (!bid->used)
      return bid->id;
  }
  return SPA_ID_INVALID;
}

/**
 * pinos_stream_recycle_buffer:
 * @stream: a #PinosStream
 * @id: a buffer id
 *
 * Recycle the buffer with @id.
 *
 * Returns: %true on success.
 */
bool
pinos_stream_recycle_buffer (PinosStream *stream,
                             uint32_t     id)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  SpaEventNodeReuseBuffer rb = SPA_EVENT_NODE_REUSE_BUFFER_INIT (stream->context->type.event_node.ReuseBuffer,
                                                                 impl->port_id, id);
  uint64_t cmd = 1;

  pinos_transport_add_event (impl->trans, (SpaEvent *)&rb);
  write (impl->rtfd, &cmd, 8);

  return true;
}

/**
 * pinos_stream_peek_buffer:
 * @stream: a #PinosStream
 * @id: the buffer id
 *
 * Get the buffer with @id from @stream. This function should be called from
 * the new-buffer signal callback.
 *
 * Returns: a #SpaBuffer or %NULL when there is no buffer.
 */
SpaBuffer *
pinos_stream_peek_buffer (PinosStream *stream,
                          uint32_t     id)
{
  BufferId *bid;

  if ((bid = find_buffer (stream, id)))
    return bid->buf;

  return NULL;
}

/**
 * pinos_stream_send_buffer:
 * @stream: a #PinosStream
 * @id: a buffer id
 * @offset: the offset in the buffer
 * @size: the size in the buffer
 *
 * Send a buffer with @id to @stream.
 *
 * For provider streams, this function should be called whenever there is a new frame
 * available.
 *
 * Returns: %true when @id was handled
 */
bool
pinos_stream_send_buffer (PinosStream     *stream,
                          uint32_t         id)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  BufferId *bid;

  if (impl->trans->outputs[0].buffer_id != SPA_ID_INVALID) {
    pinos_log_debug ("can't send %u, pending buffer %u", id, impl->trans->outputs[0].buffer_id);
    return false;
  }

  if ((bid = find_buffer (stream, id)) && !bid->used) {
    bid->used = true;
    impl->trans->outputs[0].buffer_id = id;
    impl->trans->outputs[0].status = SPA_RESULT_OK;
    send_have_output (stream);
  } else {
    pinos_log_debug ("stream %p: output %u was used", stream, id);
  }

  return true;
}
