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
#include "pinos/client/array.h"
#include "pinos/client/connection.h"
#include "pinos/client/context.h"
#include "pinos/client/stream.h"
#include "pinos/client/serialize.h"
#include "pinos/client/transport.h"

#define MAX_BUFFER_SIZE 4096
#define MAX_FDS         32
#define MAX_INPUTS      64
#define MAX_OUTPUTS     64

typedef struct {
  uint32_t id;
  int fd;
  uint32_t flags;
  void *ptr;
  off_t offset;
  size_t size;
} MemId;

typedef struct {
  uint32_t id;
  bool used;
  void *buf_ptr;
  SpaBuffer *buf;
  SpaData *datas;
} BufferId;

typedef struct
{
  PinosStream this;

  SpaNodeState node_state;

  uint32_t seq;

  unsigned int n_possible_formats;
  SpaFormat **possible_formats;

  SpaFormat *format;
  SpaPortInfo port_info;
  SpaDirection direction;
  uint32_t port_id;
  uint32_t pending_seq;

  PinosStreamFlags flags;

  bool disconnecting;

  PinosStreamMode mode;

  int rtfd;
  SpaSource *rtsocket_source;

  PinosProxy *node_proxy;
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
clear_buffers (PinosStream *stream)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  BufferId *bid;

  pinos_array_for_each (bid, &impl->buffer_ids) {
    pinos_signal_emit (&stream->remove_buffer, stream, bid->id);
    bid->buf = NULL;
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
  this = &impl->this;
  pinos_log_debug ("stream %p: new", impl);

  this->context = context;
  this->name = strdup (name);

  if (props == NULL) {
    props = pinos_properties_new ("media.name", name, NULL);
  } else if (!pinos_properties_get (props, "media.name")) {
    pinos_properties_set (props, "media.name", name);
  }
  this->properties = props;

  pinos_signal_init (&this->destroy_signal);
  pinos_signal_init (&this->state_changed);
  pinos_signal_init (&this->format_changed);
  pinos_signal_init (&this->add_buffer);
  pinos_signal_init (&this->remove_buffer);
  pinos_signal_init (&this->new_buffer);

  this->state = PINOS_STREAM_STATE_UNCONNECTED;

  impl->node_state = SPA_NODE_STATE_INIT;
  pinos_array_init (&impl->mem_ids);
  pinos_array_ensure_size (&impl->mem_ids, sizeof (MemId) * 64);
  pinos_array_init (&impl->buffer_ids);
  pinos_array_ensure_size (&impl->buffer_ids, sizeof (BufferId) * 64);
  impl->pending_seq = SPA_ID_INVALID;

  spa_list_insert (&context->stream_list, &this->link);

  return this;
}

void
pinos_stream_destroy (PinosStream *stream)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);

  pinos_log_debug ("stream %p: destroy", stream);

  pinos_signal_emit (&stream->destroy_signal, stream);

  spa_list_remove (&stream->link);

  if (impl->format)
    free (impl->format);

  if (stream->error)
    free (stream->error);

  pinos_array_clear (&impl->buffer_ids);
  pinos_array_clear (&impl->mem_ids);

  if (stream->properties)
    pinos_properties_free (stream->properties);

  if (stream->name)
    free (stream->name);

  free (impl);
}

static void
add_node_update (PinosStream *stream, uint32_t change_mask, bool flush)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  PinosMessageNodeUpdate nu = { 0, };

  nu.change_mask = change_mask;
  if (change_mask & PINOS_MESSAGE_NODE_UPDATE_MAX_INPUTS)
    nu.max_input_ports = impl->direction == SPA_DIRECTION_INPUT ? 1 : 0;
  if (change_mask & PINOS_MESSAGE_NODE_UPDATE_MAX_OUTPUTS)
    nu.max_output_ports = impl->direction == SPA_DIRECTION_OUTPUT ? 1 : 0;
  nu.props = NULL;

  pinos_proxy_send_message (impl->node_proxy,
                            PINOS_MESSAGE_NODE_UPDATE,
                            &nu,
                            flush);
}

static void
add_state_change (PinosStream *stream, SpaNodeState state, bool flush)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  PinosMessageNodeStateChange sc;

  if (impl->node_state != state) {
    sc.state = impl->node_state = state;
    pinos_proxy_send_message (impl->node_proxy,
                              PINOS_MESSAGE_NODE_STATE_CHANGE,
                              &sc,
                              flush);
  }
}

static void
add_port_update (PinosStream *stream, uint32_t change_mask, bool flush)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  PinosMessagePortUpdate pu = { 0, };;

  pu.direction = impl->direction;
  pu.port_id = impl->port_id;
  pu.change_mask = change_mask;
  if (change_mask & PINOS_MESSAGE_PORT_UPDATE_POSSIBLE_FORMATS) {
    pu.n_possible_formats = impl->n_possible_formats;
    pu.possible_formats = impl->possible_formats;
  }
  if (change_mask & PINOS_MESSAGE_PORT_UPDATE_FORMAT) {
    pu.format = impl->format;
  }
  pu.props = NULL;
  if (change_mask & PINOS_MESSAGE_PORT_UPDATE_INFO) {
    pu.info = &impl->port_info;
  }
  pinos_proxy_send_message (impl->node_proxy,
                            PINOS_MESSAGE_PORT_UPDATE,
                            &pu,
                            flush);
}

static inline void
send_need_input (PinosStream *stream)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  uint8_t cmd = PINOS_TRANSPORT_CMD_NEED_DATA;
  write (impl->rtfd, &cmd, 1);
}

static void
add_request_clock_update (PinosStream *stream, bool flush)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  PinosMessageNodeEvent cne;
  SpaNodeEventRequestClockUpdate rcu;

  cne.event = &rcu.event;
  rcu.event.type = SPA_NODE_EVENT_TYPE_REQUEST_CLOCK_UPDATE;
  rcu.event.size = sizeof (rcu);
  rcu.update_mask = SPA_NODE_EVENT_REQUEST_CLOCK_UPDATE_TIME;
  rcu.timestamp = 0;
  rcu.offset = 0;
  pinos_proxy_send_message (impl->node_proxy,
                            PINOS_MESSAGE_NODE_EVENT,
                            &cne,
                            flush);
}

static void
add_async_complete (PinosStream  *stream,
                    uint32_t      seq,
                    SpaResult     res,
                    bool          flush)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  PinosMessageNodeEvent cne;
  SpaNodeEventAsyncComplete ac;

  cne.event = &ac.event;
  ac.event.type = SPA_NODE_EVENT_TYPE_ASYNC_COMPLETE;
  ac.event.size = sizeof (ac);
  ac.seq = seq;
  ac.res = res;
  pinos_proxy_send_message (impl->node_proxy,
                            PINOS_MESSAGE_NODE_EVENT,
                            &cne,
                            flush);
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
                     SpaNodeEvent *event)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);

  switch (event->type) {
    case SPA_NODE_EVENT_TYPE_HAVE_OUTPUT:
    case SPA_NODE_EVENT_TYPE_NEED_INPUT:
      pinos_log_warn ("unhandled node event %d", event->type);
      break;

    case SPA_NODE_EVENT_TYPE_REUSE_BUFFER:
    {
      SpaNodeEventReuseBuffer *p = (SpaNodeEventReuseBuffer *) event;
      BufferId *bid;

      if (p->port_id != impl->port_id)
        break;
      if (impl->direction != SPA_DIRECTION_OUTPUT)
        break;

      if ((bid = find_buffer (stream, p->buffer_id))) {
        bid->used = false;
        pinos_signal_emit (&stream->new_buffer, stream, p->buffer_id);
      }
      break;
    }
    default:
      pinos_log_warn ("unexpected node event %d", event->type);
      break;
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
  }

  if (mask & SPA_IO_IN) {
    uint8_t cmd;
    int i;

    read (impl->rtfd, &cmd, 1);

    if (cmd & PINOS_TRANSPORT_CMD_HAVE_DATA) {
      for (i = 0; i < impl->trans->area->n_inputs; i++) {
        SpaPortInput *input = &impl->trans->inputs[i];

        if (input->buffer_id == SPA_ID_INVALID)
          continue;

        pinos_signal_emit (&stream->new_buffer, stream, input->buffer_id);
        input->buffer_id = SPA_ID_INVALID;
      }
      send_need_input (stream);
    }
    if (cmd & PINOS_TRANSPORT_CMD_HAVE_EVENT) {
      SpaNodeEvent event;
      while (pinos_transport_next_event (impl->trans, &event) == SPA_RESULT_OK) {
        SpaNodeEvent *ev = alloca (event.size);
        pinos_transport_parse_event (impl->trans, ev);
        handle_rtnode_event (stream, ev);
      }
    }
  }
}

static void
handle_socket (PinosStream *stream, int rtfd)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  struct timespec interval;

  impl->rtfd = rtfd;
  impl->rtsocket_source = pinos_loop_add_io (stream->context->loop,
                                             impl->rtfd,
                                             SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP,
                                             false,
                                             on_rtsocket_condition,
                                             stream);

  impl->timeout_source = pinos_loop_add_timer (stream->context->loop,
                                               on_timeout,
                                               stream);
  interval.tv_sec = 0;
  interval.tv_nsec = 100000000;
  pinos_loop_update_timer (stream->context->loop,
                           impl->timeout_source,
                           NULL,
                           &interval,
                           false);
  return;
}

static void
unhandle_socket (PinosStream *stream)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);

  if (impl->rtsocket_source) {
    pinos_loop_destroy_source (stream->context->loop, impl->rtsocket_source);
    impl->rtsocket_source = NULL;
  }
}

static void
handle_node_event (PinosStream  *stream,
                   SpaNodeEvent *event)
{
  switch (event->type) {
    case SPA_NODE_EVENT_TYPE_INVALID:
    case SPA_NODE_EVENT_TYPE_HAVE_OUTPUT:
    case SPA_NODE_EVENT_TYPE_NEED_INPUT:
    case SPA_NODE_EVENT_TYPE_ASYNC_COMPLETE:
    case SPA_NODE_EVENT_TYPE_REUSE_BUFFER:
    case SPA_NODE_EVENT_TYPE_ERROR:
    case SPA_NODE_EVENT_TYPE_BUFFERING:
    case SPA_NODE_EVENT_TYPE_REQUEST_REFRESH:
    case SPA_NODE_EVENT_TYPE_REQUEST_CLOCK_UPDATE:
      pinos_log_warn ("unhandled node event %d", event->type);
      break;
  }
}

static bool
handle_node_command (PinosStream    *stream,
                     uint32_t        seq,
                     SpaNodeCommand *command)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);

  switch (command->type) {
    case SPA_NODE_COMMAND_INVALID:
      break;
    case SPA_NODE_COMMAND_PAUSE:
    {
      pinos_log_debug ("stream %p: pause %d", stream, seq);

      add_state_change (stream, SPA_NODE_STATE_PAUSED, false);
      add_async_complete (stream, seq, SPA_RESULT_OK, true);
      stream_set_state (stream, PINOS_STREAM_STATE_PAUSED, NULL);
      break;
    }
    case SPA_NODE_COMMAND_START:
    {
      pinos_log_debug ("stream %p: start %d", stream, seq);
      add_state_change (stream, SPA_NODE_STATE_STREAMING, false);
      add_async_complete (stream, seq, SPA_RESULT_OK, true);

      if (impl->direction == SPA_DIRECTION_INPUT)
        send_need_input (stream);

      stream_set_state (stream, PINOS_STREAM_STATE_STREAMING, NULL);
      break;
    }
    case SPA_NODE_COMMAND_FLUSH:
    case SPA_NODE_COMMAND_DRAIN:
    case SPA_NODE_COMMAND_MARKER:
    {
      pinos_log_warn ("unhandled node command %d", command->type);
      add_async_complete (stream, seq, SPA_RESULT_NOT_IMPLEMENTED, true);
      break;
    }
    case SPA_NODE_COMMAND_CLOCK_UPDATE:
    {
      SpaNodeCommandClockUpdate *cu = (SpaNodeCommandClockUpdate *) command;
      if (cu->flags & SPA_NODE_COMMAND_CLOCK_UPDATE_FLAG_LIVE) {
        pinos_properties_set (stream->properties,
                          "pinos.latency.is-live", "1");
        pinos_properties_setf (stream->properties,
                          "pinos.latency.min", "%"PRId64, cu->latency);
      }
      impl->last_ticks = cu->ticks;
      impl->last_rate = cu->rate;
      impl->last_monotonic = cu->monotonic_time;
      break;
    }
  }
  return true;
}

static SpaResult
stream_dispatch_func (void             *object,
                      PinosMessageType  type,
                      void             *message,
                      void             *data)
{
  PinosStream *stream = data;
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);

  switch (type) {
    case PINOS_MESSAGE_CREATE_NODE_DONE:
      pinos_log_warn ("create node done %d", type);
      break;

    case PINOS_MESSAGE_CREATE_CLIENT_NODE_DONE:
    {
      PinosMessageCreateClientNodeDone *cnd = message;

      pinos_log_warn ("create client node done %d", type);
      handle_socket (stream, cnd->datafd);
      do_node_init (stream);
      break;
    }

    case PINOS_MESSAGE_ADD_PORT:
    case PINOS_MESSAGE_REMOVE_PORT:
      pinos_log_warn ("add/remove port not supported");
      break;

    case PINOS_MESSAGE_SET_FORMAT:
    {
      PinosMessageSetFormat *p = message;
      void *mem;

      if (impl->format)
        free (impl->format);
      mem = malloc (pinos_serialize_format_get_size (p->format));
      impl->format = pinos_serialize_format_copy_into (mem, p->format);

      impl->pending_seq = p->seq;

      pinos_signal_emit (&stream->format_changed, stream, impl->format);
      stream_set_state (stream, PINOS_STREAM_STATE_READY, NULL);
      break;
    }
    case PINOS_MESSAGE_SET_PROPERTY:
      pinos_log_warn ("set property not implemented");
      break;

    case PINOS_MESSAGE_ADD_MEM:
    {
      PinosMessageAddMem *p = message;
      MemId *m;

      m = find_mem (stream, p->mem_id);
      if (m) {
        pinos_log_debug ("update mem %u, fd %d, flags %d, off %zd, size %zd",
            p->mem_id, p->memfd, p->flags, p->offset, p->size);
      } else {
        m = pinos_array_add (&impl->mem_ids, sizeof (MemId));
        pinos_log_debug ("add mem %u, fd %d, flags %d, off %zd, size %zd",
            p->mem_id, p->memfd, p->flags, p->offset, p->size);
      }
      m->id = p->mem_id;
      m->fd = p->memfd;
      m->flags = p->flags;
      m->ptr = NULL;
      m->offset = p->offset;
      m->size = p->size;
      break;
    }
    case PINOS_MESSAGE_USE_BUFFERS:
    {
      PinosMessageUseBuffers *p = message;
      BufferId *bid;
      unsigned int i, j, len;
      SpaBuffer *b;

      /* clear previous buffers */
      clear_buffers (stream);

      for (i = 0; i < p->n_buffers; i++) {
        off_t offset = 0;

        MemId *mid = find_mem (stream, p->buffers[i].mem_id);
        if (mid == NULL) {
          pinos_log_warn ("unknown memory id %u", mid->id);
          continue;
        }

        if (mid->ptr == NULL) {
          //mid->ptr = mmap (NULL, mid->size, PROT_READ | PROT_WRITE, MAP_SHARED, mid->fd, mid->offset);
          mid->ptr = mmap (NULL, mid->size + mid->offset, PROT_READ | PROT_WRITE, MAP_SHARED, mid->fd, 0);
          if (mid->ptr == MAP_FAILED) {
            mid->ptr = NULL;
            pinos_log_warn ("Failed to mmap memory %zd %p: %s", mid->size, mid, strerror (errno));
            continue;
          }
          mid->ptr = SPA_MEMBER (mid->ptr, mid->offset, void);
        }
        len = pinos_array_get_len (&impl->buffer_ids, BufferId);
        bid = pinos_array_add (&impl->buffer_ids, sizeof (BufferId));

        b = p->buffers[i].buffer;

        bid->buf_ptr = SPA_MEMBER (mid->ptr, p->buffers[i].offset, void);
        {
          size_t size;

          size = pinos_serialize_buffer_get_size (p->buffers[i].buffer);
          b = bid->buf = malloc (size);
          pinos_serialize_buffer_copy_into (b, p->buffers[i].buffer);
        }
        bid->id = b->id;

        if (bid->id != len) {
          pinos_log_warn ("unexpected id %u found, expected %u", bid->id, len);
          impl->in_order = false;
        }
        pinos_log_debug ("add buffer %d %d %zd", mid->id, bid->id, p->buffers[i].offset);

        for (j = 0; j < b->n_metas; j++) {
          SpaMeta *m = &b->metas[j];
          m->data = SPA_MEMBER (bid->buf_ptr, offset, void);
          offset += m->size;
        }

        for (j = 0; j < b->n_datas; j++) {
          SpaData *d = &b->datas[j];

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

        spa_debug_buffer (b);

        pinos_signal_emit (&stream->add_buffer, stream, bid->id);
      }

      if (p->n_buffers) {
        add_state_change (stream, SPA_NODE_STATE_PAUSED, false);
      } else {
        add_state_change (stream, SPA_NODE_STATE_READY, false);
      }
      add_async_complete (stream, p->seq, SPA_RESULT_OK, true);

      if (p->n_buffers)
        stream_set_state (stream, PINOS_STREAM_STATE_PAUSED, NULL);
      else
        stream_set_state (stream, PINOS_STREAM_STATE_READY, NULL);
      break;
    }
    case PINOS_MESSAGE_NODE_EVENT:
    {
      PinosMessageNodeEvent *p = message;
      handle_node_event (stream, p->event);
      break;
    }
    case PINOS_MESSAGE_NODE_COMMAND:
    {
      PinosMessageNodeCommand *p = message;
      handle_node_command (stream, p->seq, p->command);
      break;
    }
    case PINOS_MESSAGE_PORT_COMMAND:
    {
      break;
    }
    case PINOS_MESSAGE_TRANSPORT_UPDATE:
    {
      PinosMessageTransportUpdate *p = message;
      PinosTransportInfo info;

      info.memfd =  p->memfd;
      if (info.memfd == -1)
        break;
      info.offset = p->offset;
      info.size = p->size;

      if (impl->trans)
        pinos_transport_destroy (impl->trans);
      impl->trans = pinos_transport_new_from_info (&info);

      pinos_log_debug ("transport update %d %p", impl->rtfd, impl->trans);
      break;
    }
    default:
    case PINOS_MESSAGE_INVALID:
      pinos_log_warn ("unhandled message %d", type);
      break;
  }
  return SPA_RESULT_OK;
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
                      unsigned int      n_possible_formats,
                      SpaFormat       **possible_formats)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  PinosMessageCreateClientNode ccn;
  SpaDict dict;
  SpaDictItem items[1];

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
                                      0);

  impl->node_proxy->dispatch_func = stream_dispatch_func;
  impl->node_proxy->dispatch_data = impl;

  ccn.seq = ++impl->seq;
  ccn.name = "client-node";
  dict.n_items = 1;
  dict.items = items;
  items[0].key = "pinos.target.node";
  items[0].value = port_path;
  ccn.props = &dict;
  ccn.new_id = impl->node_proxy->id;

  pinos_proxy_send_message (stream->context->core_proxy,
                            PINOS_MESSAGE_CREATE_CLIENT_NODE,
                            &ccn,
                            true);

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
                            unsigned int     n_params)
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

  return true;
}

bool
pinos_stream_get_time (PinosStream *stream,
                       PinosTime   *time)
{
  PinosStreamImpl *impl = SPA_CONTAINER_OF (stream, PinosStreamImpl, this);
  int64_t now, elapsed;
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  now = SPA_TIMESPEC_TO_TIME (&ts);
  elapsed = (now - impl->last_monotonic) / 1000;

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
  SpaNodeEventReuseBuffer rb;
  uint8_t cmd = PINOS_TRANSPORT_CMD_HAVE_EVENT;

  rb.event.type = SPA_NODE_EVENT_TYPE_REUSE_BUFFER;
  rb.event.size = sizeof (rb);
  rb.port_id = impl->port_id;
  rb.buffer_id = id;
  pinos_transport_add_event (impl->trans, &rb.event);
  write (impl->rtfd, &cmd, 1);

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

  if ((bid = find_buffer (stream, id))) {
    uint8_t cmd = PINOS_TRANSPORT_CMD_HAVE_DATA;

    bid->used = true;
    impl->trans->outputs[0].buffer_id = id;
    impl->trans->outputs[0].status = SPA_RESULT_OK;
    write (impl->rtfd, &cmd, 1);
    return true;
  } else {
    return true;
  }
}
