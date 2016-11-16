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

#include <sys/socket.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>

#include "spa/lib/debug.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gio/gunixfdmessage.h>

#include "pinos/dbus/org-pinos.h"

#include "pinos/client/pinos.h"
#include "pinos/client/array.h"
#include "pinos/client/connection.h"
#include "pinos/client/context.h"
#include "pinos/client/stream.h"
#include "pinos/client/enumtypes.h"
#include "pinos/client/format.h"
#include "pinos/client/private.h"
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
  size_t size;
} MemId;

typedef struct {
  uint32_t id;
  bool used;
  void *buf_ptr;
  SpaBuffer *buf;
  SpaData *datas;
} BufferId;

struct _PinosStreamPrivate
{
  PinosContext *context;
  gchar *name;
  PinosProperties *properties;

  guint id;

  PinosStreamState state;
  GError *error;

  gchar *path;

  SpaNodeState node_state;
  GPtrArray *possible_formats;
  SpaFormat *format;
  SpaPortInfo port_info;
  SpaDirection direction;
  uint32_t port_id;
  uint32_t pending_seq;

  PinosStreamFlags flags;

  GDBusProxy *node;
  gboolean disconnecting;

  PinosStreamMode mode;
  GSocket *socket;
  GSource *socket_source;
  int fd;

  GSocket *rtsocket;
  GSource *rtsocket_source;
  int rtfd;

  PinosConnection *conn;
  PinosTransport *trans;

  GSource *timeout_source;

  PinosArray mem_ids;
  PinosArray buffer_ids;
  gboolean in_order;

  gint64 last_ticks;
  gint32 last_rate;
  gint64 last_monotonic;
};

#define PINOS_STREAM_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_STREAM, PinosStreamPrivate))

G_DEFINE_TYPE (PinosStream, pinos_stream, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_CONTEXT,
  PROP_NAME,
  PROP_PROPERTIES,
  PROP_STATE,
  PROP_POSSIBLE_FORMATS,
  PROP_FORMAT,
};

enum
{
  SIGNAL_ADD_BUFFER,
  SIGNAL_REMOVE_BUFFER,
  SIGNAL_NEW_BUFFER,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
clear_buffers (PinosStream *stream)
{
  PinosStreamPrivate *priv = stream->priv;
  BufferId *bid;

  pinos_array_for_each (bid, &priv->buffer_ids) {
    g_signal_emit (stream, signals[SIGNAL_REMOVE_BUFFER], 0, bid->id);
    bid->buf = NULL;
  }
  priv->buffer_ids.size = 0;
  priv->in_order = TRUE;
}

static void
pinos_stream_get_property (GObject    *_object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  PinosStream *stream = PINOS_STREAM (_object);
  PinosStreamPrivate *priv = stream->priv;

  switch (prop_id) {
    case PROP_CONTEXT:
      g_value_set_object (value, priv->context);
      break;

    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    case PROP_PROPERTIES:
      g_value_set_boxed (value, priv->properties);
      break;

    case PROP_STATE:
      g_value_set_enum (value, priv->state);
      break;

    case PROP_POSSIBLE_FORMATS:
      g_value_set_boxed (value, priv->possible_formats);
      break;

    case PROP_FORMAT:
      g_value_set_boxed (value, priv->format);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (stream, prop_id, pspec);
      break;
    }
}

static void
pinos_stream_set_property (GObject      *_object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  PinosStream *stream = PINOS_STREAM (_object);
  PinosStreamPrivate *priv = stream->priv;

  switch (prop_id) {
    case PROP_CONTEXT:
      priv->context = g_value_dup_object (value);
      break;

    case PROP_NAME:
      priv->name = g_value_dup_string (value);
      break;

    case PROP_PROPERTIES:
      if (priv->properties)
        pinos_properties_free (priv->properties);
      priv->properties = g_value_dup_boxed (value);
      break;

    case PROP_FORMAT:
      if (priv->format)
        g_boxed_free (SPA_TYPE_FORMAT, priv->format);
      priv->format = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (stream, prop_id, pspec);
      break;
    }
}

static gboolean
do_notify_state (PinosStream *stream)
{
  g_object_notify (G_OBJECT (stream), "state");
  g_object_unref (stream);
  return FALSE;
}

static void
stream_set_state (PinosStream      *stream,
                  PinosStreamState  state,
                  GError           *error)
{
  if (stream->priv->state != state) {
    if (error) {
      g_clear_error (&stream->priv->error);
      stream->priv->error = error;
    }
    stream->priv->state = state;
    g_main_context_invoke (stream->priv->context->priv->context,
                          (GSourceFunc) do_notify_state,
                          g_object_ref (stream));
  } else {
    if (error)
      g_error_free (error);
  }
}

static void
on_node_info (PinosContext        *c,
              const PinosNodeInfo *info,
              gpointer             user_data)
{
  PinosStream *stream = PINOS_STREAM (user_data);

  if (info->state == PINOS_NODE_STATE_ERROR) {
    pinos_log_debug ("stream %p: node %s in error", stream, info->node_path);
    stream_set_state (stream,
                      PINOS_STREAM_STATE_ERROR,
                      g_error_new (PINOS_ERROR,
                                   PINOS_ERROR_NODE_STATE,
                                   "node is in error"));
  }
}

static void
info_ready (GObject *o, GAsyncResult *res, gpointer user_data)
{
  GError *error = NULL;

  if (!pinos_context_info_finish (o, res, &error)) {
    pinos_log_error ("introspection failure: %s\n", error->message);
    g_clear_error (&error);
  }
}

static void
subscription_cb (PinosSubscribe         *subscribe,
                 PinosSubscriptionEvent  event,
                 PinosSubscriptionFlags  flags,
                 GDBusProxy             *object,
                 gpointer                user_data)
{
  PinosStream *stream = PINOS_STREAM (user_data);
  PinosStreamPrivate *priv = stream->priv;

  switch (flags) {
    case PINOS_SUBSCRIPTION_FLAG_NODE:
      if (event == PINOS_SUBSCRIPTION_EVENT_REMOVE) {
        if (object == priv->node && !priv->disconnecting) {
          stream_set_state (stream,
                            PINOS_STREAM_STATE_ERROR,
                            g_error_new_literal (G_IO_ERROR,
                                                 G_IO_ERROR_CLOSED,
                                                 "Node disappeared"));
        }
      } else if (event == PINOS_SUBSCRIPTION_EVENT_CHANGE) {
        if (object == priv->node && !priv->disconnecting) {
          pinos_context_get_node_info_by_id (priv->context,
                                             object,
                                             PINOS_NODE_INFO_FLAGS_NONE,
                                             on_node_info,
                                             NULL,
                                             info_ready,
                                             stream);
        }
      }
      break;

    default:
      break;
  }
}

static void
pinos_stream_constructed (GObject * object)
{
  PinosStream *stream = PINOS_STREAM (object);
  PinosStreamPrivate *priv = stream->priv;

  priv->id = g_signal_connect (priv->context->priv->subscribe,
                    "subscription-event",
                    (GCallback) subscription_cb,
                    stream);

  G_OBJECT_CLASS (pinos_stream_parent_class)->constructed (object);
}

static void
pinos_stream_finalize (GObject * object)
{
  PinosStream *stream = PINOS_STREAM (object);
  PinosStreamPrivate *priv = stream->priv;

  pinos_log_debug ("free stream %p", stream);

  g_clear_object (&priv->node);

  if (priv->possible_formats)
    g_ptr_array_unref (priv->possible_formats);
  if (priv->format)
    g_boxed_free (SPA_TYPE_FORMAT, priv->format);

  g_free (priv->path);
  g_clear_error (&priv->error);

  pinos_array_clear (&priv->buffer_ids);
  pinos_array_clear (&priv->mem_ids);

  if (priv->properties)
    pinos_properties_free (priv->properties);
  g_signal_handler_disconnect (priv->context->priv->subscribe, priv->id);
  g_clear_object (&priv->context);
  g_free (priv->name);

  G_OBJECT_CLASS (pinos_stream_parent_class)->finalize (object);
}

static void
pinos_stream_class_init (PinosStreamClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosStreamPrivate));

  gobject_class->constructed = pinos_stream_constructed;
  gobject_class->finalize = pinos_stream_finalize;
  gobject_class->set_property = pinos_stream_set_property;
  gobject_class->get_property = pinos_stream_get_property;

  /**
   * PinosStream:context
   *
   * The context of the stream.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_CONTEXT,
                                   g_param_spec_object ("context",
                                                        "Context",
                                                        "The context",
                                                        PINOS_TYPE_CONTEXT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
  /**
   * PinosStream:name
   *
   * The name of the stream as specified at construction time.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Name",
                                                        "The name of the stream",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
  /**
   * PinosStream:properties
   *
   * The properties of the stream as specified at construction time.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_PROPERTIES,
                                   g_param_spec_boxed ("properties",
                                                       "Properties",
                                                       "The properties of the stream",
                                                       PINOS_TYPE_PROPERTIES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_STATIC_STRINGS));
  /**
   * PinosStream:state
   *
   * The state of the stream. Use the notify::state signal to be notified
   * of state changes.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_STATE,
                                   g_param_spec_enum ("state",
                                                      "State",
                                                      "The stream state",
                                                      PINOS_TYPE_STREAM_STATE,
                                                      PINOS_STREAM_STATE_UNCONNECTED,
                                                      G_PARAM_READABLE |
                                                      G_PARAM_STATIC_STRINGS));
  /**
   * PinosStream:possible-formats
   *
   * The possible formats for the stream. this can only be used after connecting
   * the stream for capture or provide.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_POSSIBLE_FORMATS,
                                   g_param_spec_boxed ("possible-formats",
                                                       "Possible Formats",
                                                       "The possbile formats of the stream",
                                                       G_TYPE_PTR_ARRAY,
                                                       G_PARAM_READABLE |
                                                       G_PARAM_STATIC_STRINGS));
  /**
   * PinosStream:formats
   *
   * The format of the stream. This will be set after starting the stream.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_FORMAT,
                                   g_param_spec_boxed ("format",
                                                       "Format",
                                                       "The format of the stream",
                                                       SPA_TYPE_FORMAT,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_STRINGS));
  /**
   * PinosStream:new-buffer
   * @id: the buffer id
   *
   * this signal will be fired whenever a buffer is added to the pool of buffers.
   */
  signals[SIGNAL_ADD_BUFFER] = g_signal_new ("add-buffer",
                                             G_TYPE_FROM_CLASS (klass),
                                             G_SIGNAL_RUN_LAST,
                                             0,
                                             NULL,
                                             NULL,
                                             g_cclosure_marshal_generic,
                                             G_TYPE_NONE,
                                             1,
                                             G_TYPE_UINT);
  /**
   * PinosStream:remove-buffer
   * @id: the buffer id
   *
   * this signal will be fired whenever a buffer is removed from the pool of buffers.
   */
  signals[SIGNAL_REMOVE_BUFFER] = g_signal_new ("remove-buffer",
                                                 G_TYPE_FROM_CLASS (klass),
                                                 G_SIGNAL_RUN_LAST,
                                                 0,
                                                 NULL,
                                                 NULL,
                                                 g_cclosure_marshal_generic,
                                                 G_TYPE_NONE,
                                                 1,
                                                 G_TYPE_UINT);
  /**
   * PinosStream:new-buffer
   * @id: the buffer id
   *
   * this signal will be fired whenever a buffer is ready to be processed.
   */
  signals[SIGNAL_NEW_BUFFER] = g_signal_new ("new-buffer",
                                             G_TYPE_FROM_CLASS (klass),
                                             G_SIGNAL_RUN_LAST,
                                             0,
                                             NULL,
                                             NULL,
                                             g_cclosure_marshal_generic,
                                             G_TYPE_NONE,
                                             1,
                                             G_TYPE_UINT);
}

static void
pinos_stream_init (PinosStream * stream)
{
  PinosStreamPrivate *priv = stream->priv = PINOS_STREAM_GET_PRIVATE (stream);

  pinos_log_debug ("new stream %p", stream);

  priv->state = PINOS_STREAM_STATE_UNCONNECTED;
  priv->node_state = SPA_NODE_STATE_INIT;
  pinos_array_init (&priv->mem_ids);
  pinos_array_ensure_size (&priv->mem_ids, sizeof (MemId) * 64);
  pinos_array_init (&priv->buffer_ids);
  pinos_array_ensure_size (&priv->buffer_ids, sizeof (BufferId) * 64);
  priv->pending_seq = SPA_ID_INVALID;
}

/**
 * pinos_stream_state_as_string:
 * @state: a #PinosStreamState
 *
 * Return the string representation of @state.
 *
 * Returns: the string representation of @state.
 */
const gchar *
pinos_stream_state_as_string (PinosStreamState state)
{
  GEnumValue *val;

  val = g_enum_get_value (G_ENUM_CLASS (g_type_class_ref (PINOS_TYPE_STREAM_STATE)),
                          state);

  return val == NULL ? "invalid-state" : val->value_nick;
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
                  const gchar     *name,
                  PinosProperties *props)
{
  PinosStream *stream;

  g_return_val_if_fail (PINOS_IS_CONTEXT (context), NULL);
  g_return_val_if_fail (name != NULL, NULL);

  if (props == NULL) {
    props = pinos_properties_new ("media.name", name, NULL);
  } else if (!pinos_properties_get (props, "media.name")) {
    pinos_properties_set (props, "media.name", name);
  }

  stream = g_object_new (PINOS_TYPE_STREAM,
                       "context", context,
                       "name", name,
                       "properties", props,
                       NULL);

  pinos_properties_free (props);

  return stream;
}

/**
 * pinos_stream_get_state:
 * @stream: a #PinosStream
 *
 * Get the state of @stream.
 *
 * Returns: the state of @stream
 */
PinosStreamState
pinos_stream_get_state (PinosStream *stream)
{
  g_return_val_if_fail (PINOS_IS_STREAM (stream), PINOS_STREAM_STATE_ERROR);

  return stream->priv->state;
}

/**
 * pinos_stream_get_error:
 * @stream: a #PinosStream
 *
 * Get the error of @stream.
 *
 * Returns: the error of @stream or %NULL when there is no error
 */
const GError *
pinos_stream_get_error (PinosStream *stream)
{
  g_return_val_if_fail (PINOS_IS_STREAM (stream), NULL);

  return stream->priv->error;
}

static void
add_node_update (PinosStream *stream, uint32_t change_mask)
{
  PinosStreamPrivate *priv = stream->priv;
  PinosMessageNodeUpdate nu = { 0, };

  nu.change_mask = change_mask;
  if (change_mask & PINOS_MESSAGE_NODE_UPDATE_MAX_INPUTS)
    nu.max_input_ports = priv->direction == SPA_DIRECTION_INPUT ? 1 : 0;
  if (change_mask & PINOS_MESSAGE_NODE_UPDATE_MAX_OUTPUTS)
    nu.max_output_ports = priv->direction == SPA_DIRECTION_OUTPUT ? 1 : 0;
  nu.props = NULL;
  pinos_connection_add_message (priv->conn, PINOS_MESSAGE_NODE_UPDATE, &nu);
}

static void
add_state_change (PinosStream *stream, SpaNodeState state)
{
  PinosStreamPrivate *priv = stream->priv;
  PinosMessageNodeStateChange sc;

  if (priv->node_state != state) {
    sc.state = priv->node_state = state;
    pinos_connection_add_message (priv->conn, PINOS_MESSAGE_NODE_STATE_CHANGE, &sc);
  }
}

static void
add_port_update (PinosStream *stream, uint32_t change_mask)
{
  PinosStreamPrivate *priv = stream->priv;
  PinosMessagePortUpdate pu = { 0, };;

  pu.direction = priv->direction;
  pu.port_id = priv->port_id;
  pu.change_mask = change_mask;
  if (change_mask & PINOS_MESSAGE_PORT_UPDATE_POSSIBLE_FORMATS) {
    pu.n_possible_formats = priv->possible_formats->len;
    pu.possible_formats = (SpaFormat **)priv->possible_formats->pdata;
  }
  if (change_mask & PINOS_MESSAGE_PORT_UPDATE_FORMAT) {
    pu.format = priv->format;
  }
  pu.props = NULL;
  if (change_mask & PINOS_MESSAGE_PORT_UPDATE_INFO) {
    pu.info = &priv->port_info;
  }
  pinos_connection_add_message (priv->conn, PINOS_MESSAGE_PORT_UPDATE, &pu);
}

static inline void
send_need_input (PinosStream *stream)
{
  PinosStreamPrivate *priv = stream->priv;
  uint8_t cmd = PINOS_TRANSPORT_CMD_NEED_DATA;
  write (priv->rtfd, &cmd, 1);

}

static void
add_request_clock_update (PinosStream *stream)
{
  PinosStreamPrivate *priv = stream->priv;
  PinosMessageNodeEvent cne;
  SpaNodeEventRequestClockUpdate rcu;

  cne.event = &rcu.event;
  rcu.event.type = SPA_NODE_EVENT_TYPE_REQUEST_CLOCK_UPDATE;
  rcu.event.size = sizeof (rcu);
  rcu.update_mask = SPA_NODE_EVENT_REQUEST_CLOCK_UPDATE_TIME;
  rcu.timestamp = 0;
  rcu.offset = 0;
  pinos_connection_add_message (priv->conn, PINOS_MESSAGE_NODE_EVENT, &cne);
}

static void
add_async_complete (PinosStream       *stream,
                    uint32_t           seq,
                    SpaResult          res)
{
  PinosStreamPrivate *priv = stream->priv;
  PinosMessageNodeEvent cne;
  SpaNodeEventAsyncComplete ac;

  cne.event = &ac.event;
  ac.event.type = SPA_NODE_EVENT_TYPE_ASYNC_COMPLETE;
  ac.event.size = sizeof (ac);
  ac.seq = seq;
  ac.res = res;
  pinos_connection_add_message (priv->conn, PINOS_MESSAGE_NODE_EVENT, &cne);
}

static void
do_node_init (PinosStream *stream)
{
  PinosStreamPrivate *priv = stream->priv;

  add_node_update (stream, PINOS_MESSAGE_NODE_UPDATE_MAX_INPUTS |
                           PINOS_MESSAGE_NODE_UPDATE_MAX_OUTPUTS);

  priv->port_info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
  add_port_update (stream, PINOS_MESSAGE_PORT_UPDATE_POSSIBLE_FORMATS |
                           PINOS_MESSAGE_PORT_UPDATE_INFO);

  add_state_change (stream, SPA_NODE_STATE_CONFIGURE);

  if (!pinos_connection_flush (priv->conn))
    pinos_log_warn ("stream %p: error writing connection", stream);
}

static MemId *
find_mem (PinosStream *stream, uint32_t id)
{
  PinosStreamPrivate *priv = stream->priv;
  MemId *mid;

  pinos_array_for_each (mid, &priv->mem_ids) {
    if (mid->id == id)
      return mid;
  }
  return NULL;
}

static BufferId *
find_buffer (PinosStream *stream, uint32_t id)
{
  PinosStreamPrivate *priv = stream->priv;

  if (priv->in_order && pinos_array_check_index (&priv->buffer_ids, id, BufferId)) {
    return pinos_array_get_unchecked (&priv->buffer_ids, id, BufferId);
  } else {
    BufferId *bid;

    pinos_array_for_each (bid, &priv->buffer_ids) {
      if (bid->id == id)
        return bid;
    }
  }
  return NULL;
}

static gboolean
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
  return TRUE;
}

static gboolean
handle_node_command (PinosStream    *stream,
                     uint32_t        seq,
                     SpaNodeCommand *command)
{
  PinosStreamPrivate *priv = stream->priv;

  switch (command->type) {
    case SPA_NODE_COMMAND_INVALID:
      break;
    case SPA_NODE_COMMAND_PAUSE:
    {
      pinos_log_debug ("stream %p: pause %d", stream, seq);

      add_state_change (stream, SPA_NODE_STATE_PAUSED);
      add_async_complete (stream, seq, SPA_RESULT_OK);

      if (!pinos_connection_flush (priv->conn))
        pinos_log_warn ("stream %p: error writing connection", stream);

      stream_set_state (stream, PINOS_STREAM_STATE_PAUSED, NULL);
      break;
    }
    case SPA_NODE_COMMAND_START:
    {
      pinos_log_debug ("stream %p: start %d", stream, seq);
      add_state_change (stream, SPA_NODE_STATE_STREAMING);
      add_async_complete (stream, seq, SPA_RESULT_OK);

      if (!pinos_connection_flush (priv->conn))
        pinos_log_warn ("stream %p: error writing connection", stream);

      if (priv->direction == SPA_DIRECTION_INPUT)
        send_need_input (stream);

      stream_set_state (stream, PINOS_STREAM_STATE_STREAMING, NULL);
      break;
    }
    case SPA_NODE_COMMAND_FLUSH:
    case SPA_NODE_COMMAND_DRAIN:
    case SPA_NODE_COMMAND_MARKER:
    {
      pinos_log_warn ("unhandled node command %d", command->type);
      add_async_complete (stream, seq, SPA_RESULT_NOT_IMPLEMENTED);

      if (!pinos_connection_flush (priv->conn))
        pinos_log_warn ("stream %p: error writing connection", stream);
      break;
    }
    case SPA_NODE_COMMAND_CLOCK_UPDATE:
    {
      SpaNodeCommandClockUpdate *cu = (SpaNodeCommandClockUpdate *) command;
      if (cu->flags & SPA_NODE_COMMAND_CLOCK_UPDATE_FLAG_LIVE) {
        pinos_properties_set (priv->properties,
                          "pinos.latency.is-live", "1");
        pinos_properties_setf (priv->properties,
                          "pinos.latency.min", "%"PRId64, cu->latency);
      }
      priv->last_ticks = cu->ticks;
      priv->last_rate = cu->rate;
      priv->last_monotonic = cu->monotonic_time;
      break;
    }
  }
  return TRUE;
}

static gboolean
parse_connection (PinosStream *stream)
{
  PinosStreamPrivate *priv = stream->priv;
  PinosConnection *conn = priv->conn;

  while (pinos_connection_has_next (conn)) {
    PinosMessageType type = pinos_connection_get_type (conn);

    switch (type) {
      case PINOS_MESSAGE_NODE_UPDATE:
      case PINOS_MESSAGE_PORT_UPDATE:
      case PINOS_MESSAGE_PORT_STATUS_CHANGE:
      case PINOS_MESSAGE_NODE_STATE_CHANGE:
      case PINOS_MESSAGE_PROCESS_BUFFER:
        pinos_log_warn ("got unexpected message %d", type);
        break;

      case PINOS_MESSAGE_ADD_PORT:
      case PINOS_MESSAGE_REMOVE_PORT:
        pinos_log_warn ("add/remove port not supported");
        break;

      case PINOS_MESSAGE_SET_FORMAT:
      {
        PinosMessageSetFormat p;
        gpointer mem;

        if (!pinos_connection_parse_message (conn, &p))
          break;

        if (priv->format)
          g_free (priv->format);
        mem = malloc (pinos_serialize_format_get_size (p.format));
        priv->format = pinos_serialize_format_copy_into (mem, p.format);

        priv->pending_seq = p.seq;
        g_object_notify (G_OBJECT (stream), "format");
        stream_set_state (stream, PINOS_STREAM_STATE_READY, NULL);
        break;
      }
      case PINOS_MESSAGE_SET_PROPERTY:
        pinos_log_warn ("set property not implemented");
        break;

      case PINOS_MESSAGE_ADD_MEM:
      {
        PinosMessageAddMem p;
        int fd;
        MemId *m;

        if (!pinos_connection_parse_message (conn, &p))
          break;

        fd = pinos_connection_get_fd (conn, p.fd_index, false);
        if (fd == -1)
          break;

        m = find_mem (stream, p.mem_id);
        if (m) {
          pinos_log_debug ("update mem %u, fd %d, flags %d, size %zd", p.mem_id, fd, p.flags, p.size);
        } else {
          m = pinos_array_add (&priv->mem_ids, sizeof (MemId));
          pinos_log_debug ("add mem %u, fd %d, flags %d, size %zd", p.mem_id, fd, p.flags, p.size);
        }
        m->id = p.mem_id;
        m->fd = fd;
        m->flags = p.flags;
        m->ptr = NULL;
        m->size = p.size;
        break;
      }
      case PINOS_MESSAGE_USE_BUFFERS:
      {
        PinosMessageUseBuffers p;
        BufferId *bid;
        unsigned int i, j, len;
        SpaBuffer *b;

        if (!pinos_connection_parse_message (conn, &p))
          break;

        /* clear previous buffers */
        clear_buffers (stream);

        for (i = 0; i < p.n_buffers; i++) {
          MemId *mid = find_mem (stream, p.buffers[i].mem_id);
          if (mid == NULL) {
            pinos_log_warn ("unknown memory id %u", mid->id);
            continue;
          }

          if (mid->ptr == NULL) {
            mid->ptr = mmap (NULL, mid->size, PROT_READ | PROT_WRITE, MAP_SHARED, mid->fd, 0);
            if (mid->ptr == MAP_FAILED) {
              mid->ptr = NULL;
              pinos_log_warn ("Failed to mmap memory %zd %p: %s", mid->size, mid, strerror (errno));
              continue;
            }
          }
          len = pinos_array_get_len (&priv->buffer_ids, BufferId);
          bid = pinos_array_add (&priv->buffer_ids, sizeof (BufferId));

          bid->buf_ptr = SPA_MEMBER (mid->ptr, p.buffers[i].offset, void);
          {
            size_t size;
            unsigned int i;
            SpaMeta *m;

            b = bid->buf_ptr;
            size = sizeof (SpaBuffer);
            m = SPA_MEMBER (b, SPA_PTR_TO_INT (b->metas), SpaMeta);
            for (i = 0; i < b->n_metas; i++)
              size += sizeof (SpaMeta) + m[i].size;
            for (i = 0; i < b->n_datas; i++)
              size += sizeof (SpaData);

            b = bid->buf = g_memdup (bid->buf_ptr, size);

            if (b->metas)
              b->metas = SPA_MEMBER (b, SPA_PTR_TO_INT (b->metas), SpaMeta);
            if (b->datas) {
              bid->datas = SPA_MEMBER (bid->buf_ptr, SPA_PTR_TO_INT (b->datas), SpaData);
              b->datas = SPA_MEMBER (b, SPA_PTR_TO_INT (b->datas), SpaData);
            }
          }

          bid->id = b->id;

          if (bid->id != len) {
            pinos_log_warn ("unexpected id %u found, expected %u", bid->id, len);
            priv->in_order = FALSE;
          }
          pinos_log_debug ("add buffer %d %d %zd", mid->id, bid->id, p.buffers[i].offset);

          for (j = 0; j < b->n_metas; j++) {
            SpaMeta *m = &b->metas[j];
            if (m->data)
              m->data = SPA_MEMBER (bid->buf_ptr, SPA_PTR_TO_INT (m->data), void);
          }

          for (j = 0; j < b->n_datas; j++) {
            SpaData *d = &b->datas[j];

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

          g_signal_emit (stream, signals[SIGNAL_ADD_BUFFER], 0, bid->id);
        }

        if (p.n_buffers) {
          add_state_change (stream, SPA_NODE_STATE_PAUSED);
        } else {
          add_state_change (stream, SPA_NODE_STATE_READY);
        }
        add_async_complete (stream, p.seq, SPA_RESULT_OK);

        if (!pinos_connection_flush (conn))
          pinos_log_warn ("stream %p: error writing connection", stream);

        if (p.n_buffers)
          stream_set_state (stream, PINOS_STREAM_STATE_PAUSED, NULL);
        else
          stream_set_state (stream, PINOS_STREAM_STATE_READY, NULL);
        break;
      }
      case PINOS_MESSAGE_NODE_EVENT:
      {
        PinosMessageNodeEvent p;

        if (!pinos_connection_parse_message (conn, &p))
          break;

        handle_node_event (stream, p.event);
        break;
      }
      case PINOS_MESSAGE_NODE_COMMAND:
      {
        PinosMessageNodeCommand p;

        if (!pinos_connection_parse_message (conn, &p))
          break;

        handle_node_command (stream, p.seq, p.command);
        break;
      }
      case PINOS_MESSAGE_PORT_COMMAND:
      {
        PinosMessagePortCommand p;

        if (!pinos_connection_parse_message (conn, &p))
          break;

        break;
      }
      case PINOS_MESSAGE_TRANSPORT_UPDATE:
      {
        PinosMessageTransportUpdate p;
        PinosTransportInfo info;

        if (!pinos_connection_parse_message (conn, &p))
          break;

        info.memfd = pinos_connection_get_fd (conn, p.memfd_index, false);
        if (info.memfd == -1)
          break;
        info.offset = p.offset;
        info.size = p.size;

        pinos_log_debug ("transport update %d %p", priv->rtfd, priv->trans);

        if (priv->trans)
          pinos_transport_free (priv->trans);
        priv->trans = pinos_transport_new_from_info (&info);

        break;
      }
      case PINOS_MESSAGE_INVALID:
        pinos_log_warn ("unhandled message %d", type);
        break;
    }
  }
  return TRUE;
}

static gboolean
on_socket_condition (GSocket      *socket,
                     GIOCondition  condition,
                     gpointer      user_data)
{
  PinosStream *stream = user_data;

  switch (condition) {
    case G_IO_IN:
    {
      parse_connection (stream);
      break;
    }

    case G_IO_OUT:
      pinos_log_warn ("can do IO\n");
      break;

    default:
      break;
  }
  return TRUE;
}

static gboolean
handle_rtnode_event (PinosStream  *stream,
                     SpaNodeEvent *event)
{
  PinosStreamPrivate *priv = stream->priv;

  switch (event->type) {
    case SPA_NODE_EVENT_TYPE_INVALID:
    case SPA_NODE_EVENT_TYPE_ASYNC_COMPLETE:
    case SPA_NODE_EVENT_TYPE_ERROR:
    case SPA_NODE_EVENT_TYPE_BUFFERING:
    case SPA_NODE_EVENT_TYPE_REQUEST_REFRESH:
    case SPA_NODE_EVENT_TYPE_REQUEST_CLOCK_UPDATE:
      pinos_log_warn ("unexpected node event %d", event->type);
      break;

    case SPA_NODE_EVENT_TYPE_HAVE_OUTPUT:
    case SPA_NODE_EVENT_TYPE_NEED_INPUT:
      pinos_log_warn ("unhandled node event %d", event->type);
      break;

    case SPA_NODE_EVENT_TYPE_REUSE_BUFFER:
    {
      SpaNodeEventReuseBuffer *p = (SpaNodeEventReuseBuffer *) event;
      BufferId *bid;

      if (p->port_id != priv->port_id)
        break;
      if (priv->direction != SPA_DIRECTION_OUTPUT)
        break;

      if ((bid = find_buffer (stream, p->buffer_id))) {
        bid->used = FALSE;
        g_signal_emit (stream, signals[SIGNAL_NEW_BUFFER], 0, p->buffer_id);
      }
      break;
    }
  }
  return TRUE;
}

static gboolean
on_rtsocket_condition (GSocket      *socket,
                       GIOCondition  condition,
                       gpointer      user_data)
{
  PinosStream *stream = user_data;
  PinosStreamPrivate *priv = stream->priv;

  switch (condition) {
    case G_IO_IN:
    {
      uint8_t cmd;
      int i;

      read (priv->rtfd, &cmd, 1);

      if (cmd & PINOS_TRANSPORT_CMD_HAVE_DATA) {
        BufferId *bid;

        for (i = 0; i < priv->trans->area->n_inputs; i++) {
          SpaPortInput *input = &priv->trans->inputs[i];

          if (input->buffer_id == SPA_ID_INVALID)
            continue;

          if ((bid = find_buffer (stream, input->buffer_id))) {
            for (i = 0; i < bid->buf->n_datas; i++) {
              bid->buf->datas[i].size = bid->datas[i].size;
            }
            g_signal_emit (stream, signals[SIGNAL_NEW_BUFFER], 0, bid->id);
          }
          input->buffer_id = SPA_ID_INVALID;
        }
        send_need_input (stream);
      }
      if (cmd & PINOS_TRANSPORT_CMD_HAVE_EVENT) {
        SpaNodeEvent event;
        while (pinos_transport_next_event (priv->trans, &event) == SPA_RESULT_OK) {
          SpaNodeEvent *ev = alloca (event.size);
          pinos_transport_parse_event (priv->trans, ev);
          handle_rtnode_event (stream, ev);
        }
      }
      break;
    }

    case G_IO_OUT:
      pinos_log_warn ("can do IO");
      break;

    default:
      break;
  }
  return TRUE;
}

static gboolean
on_timeout (gpointer user_data)
{
  PinosStream *stream = user_data;
  PinosStreamPrivate *priv = stream->priv;

  add_request_clock_update (stream);

  if (!pinos_connection_flush (priv->conn))
    pinos_log_warn ("stream %p: error writing connection", stream);

  return G_SOURCE_CONTINUE;
}

static void
handle_socket (PinosStream *stream, gint fd, gint rtfd)
{
  PinosStreamPrivate *priv = stream->priv;
  GError *error = NULL;

  priv->socket = g_socket_new_from_fd (fd, &error);
  if (priv->socket == NULL)
    goto socket_failed;
  priv->rtsocket = g_socket_new_from_fd (rtfd, &error);
  if (priv->rtsocket == NULL)
    goto socket_failed;

  priv->fd = g_socket_get_fd (priv->socket);
  priv->socket_source = g_socket_create_source (priv->socket, G_IO_IN, NULL);
  g_source_set_callback (priv->socket_source, (GSourceFunc) on_socket_condition, stream, NULL);
  g_source_attach (priv->socket_source, priv->context->priv->context);
  priv->conn = pinos_connection_new (priv->fd);

  priv->rtfd = g_socket_get_fd (priv->rtsocket);
  priv->rtsocket_source = g_socket_create_source (priv->rtsocket, G_IO_IN, NULL);
  g_source_set_callback (priv->rtsocket_source, (GSourceFunc) on_rtsocket_condition, stream, NULL);
  g_source_attach (priv->rtsocket_source, priv->context->priv->context);

  pinos_log_debug ("sockets %d %d", priv->fd, priv->rtfd);

  priv->timeout_source = g_timeout_source_new (100);
  g_source_set_callback (priv->timeout_source, (GSourceFunc) on_timeout, stream, NULL);
  g_source_attach (priv->timeout_source, priv->context->priv->context);

  return;

  /* ERRORS */
socket_failed:
  {
    pinos_log_warn ("failed to create socket: %s", error->message);
    stream_set_state (stream, PINOS_STREAM_STATE_ERROR, error);
    return;
  }
}

static void
unhandle_socket (PinosStream *stream)
{
  PinosStreamPrivate *priv = stream->priv;

  if (priv->socket_source) {
    g_source_destroy (priv->socket_source);
    g_clear_pointer (&priv->socket_source, g_source_unref);
  }
  if (priv->rtsocket_source) {
    g_source_destroy (priv->rtsocket_source);
    g_clear_pointer (&priv->rtsocket_source, g_source_unref);
  }
}

static void
on_node_proxy (GObject      *source_object,
               GAsyncResult *res,
               gpointer      user_data)
{
  PinosStream *stream = user_data;
  PinosStreamPrivate *priv = stream->priv;
  PinosContext *context = priv->context;

  GError *error = NULL;

  priv->node = pinos_subscribe_get_proxy_finish (context->priv->subscribe,
                                                 res,
                                                 &error);
  if (priv->node == NULL)
    goto node_failed;

  do_node_init (stream);

  stream_set_state (stream, PINOS_STREAM_STATE_CONFIGURE, NULL);
  g_object_unref (stream);

  return;

node_failed:
  {
    pinos_log_warn ("failed to get node proxy: %s", error->message);
    stream_set_state (stream, PINOS_STREAM_STATE_ERROR, error);
    g_object_unref (stream);
    return;
  }
}

static void
on_node_created (GObject      *source_object,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  PinosStream *stream = user_data;
  PinosStreamPrivate *priv = stream->priv;
  PinosContext *context = priv->context;
  GVariant *ret;
  GError *error = NULL;
  const gchar *node_path;
  GUnixFDList *fd_list;
  gint fd_idx, fd;
  gint rtfd_idx, rtfd;

  g_assert (context->priv->daemon == G_DBUS_PROXY (source_object));

  ret = g_dbus_proxy_call_with_unix_fd_list_finish (context->priv->daemon,
                                                    &fd_list,
                                                    res, &error);
  if (ret == NULL)
    goto create_failed;

  g_variant_get (ret, "(&ohh)", &node_path, &fd_idx, &rtfd_idx);
  g_variant_unref (ret);

  if ((fd = g_unix_fd_list_get (fd_list, fd_idx, &error)) < 0)
    goto fd_failed;
  if ((rtfd = g_unix_fd_list_get (fd_list, rtfd_idx, &error)) < 0)
    goto fd_failed;

  priv->fd = fd;
  priv->rtfd = rtfd;
  g_object_unref (fd_list);

  handle_socket (stream, priv->fd, priv->rtfd);

  pinos_subscribe_get_proxy (context->priv->subscribe,
                             PINOS_DBUS_SERVICE,
                             node_path,
                             "org.pinos.Node1",
                             NULL,
                             on_node_proxy,
                             stream);

  return;

  /* ERRORS */
create_failed:
  {
    pinos_log_warn ("failed to connect: %s", error->message);
    goto exit_error;
  }
fd_failed:
  {
    pinos_log_warn ("failed to get FD: %s", error->message);
    g_object_unref (fd_list);
    goto exit_error;
  }
exit_error:
  {
    stream_set_state (stream, PINOS_STREAM_STATE_ERROR, error);
    g_object_unref (stream);
    return;
  }
}

static gboolean
do_connect (PinosStream *stream)
{
  PinosStreamPrivate *priv = stream->priv;
  PinosContext *context = priv->context;

  if (priv->properties == NULL)
    priv->properties = pinos_properties_new (NULL, NULL);
  if (priv->path)
    pinos_properties_set (priv->properties,
                          "pinos.target.node", priv->path);

  g_dbus_proxy_call (context->priv->daemon,
                     "CreateClientNode",
                     g_variant_new ("(s@a{sv})",
                       "client-node",
                       pinos_properties_to_variant (priv->properties)),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     NULL, /* GCancellable *cancellable */
                     on_node_created,
                     stream);
  return FALSE;
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
 * Returns: %TRUE on success.
 */
gboolean
pinos_stream_connect (PinosStream      *stream,
                      PinosDirection    direction,
                      PinosStreamMode   mode,
                      const gchar      *port_path,
                      PinosStreamFlags  flags,
                      GPtrArray        *possible_formats)
{
  PinosStreamPrivate *priv;
  PinosContext *context;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);
  g_return_val_if_fail (possible_formats != NULL, FALSE);

  priv = stream->priv;
  context = priv->context;
  g_return_val_if_fail (pinos_context_get_state (context) == PINOS_CONTEXT_STATE_CONNECTED, FALSE);
  g_return_val_if_fail (pinos_stream_get_state (stream) == PINOS_STREAM_STATE_UNCONNECTED, FALSE);

  priv->direction = direction == PINOS_DIRECTION_INPUT ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT;
  priv->port_id = 0;
  priv->mode = mode;
  g_free (priv->path);
  priv->path = g_strdup (port_path);
  priv->flags = flags;
  if (priv->possible_formats)
    g_ptr_array_unref (priv->possible_formats);
  priv->possible_formats = possible_formats;

  stream_set_state (stream, PINOS_STREAM_STATE_CONNECTING, NULL);

  g_main_context_invoke (context->priv->context,
                         (GSourceFunc) do_connect,
                         g_object_ref (stream));

  return TRUE;
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
 * Returns: %TRUE on success
 */
gboolean
pinos_stream_finish_format (PinosStream     *stream,
                            SpaResult        res,
                            SpaAllocParam  **params,
                            unsigned int     n_params)
{
  PinosStreamPrivate *priv;
  PinosContext *context;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);
  priv = stream->priv;
  g_return_val_if_fail (priv->pending_seq != SPA_ID_INVALID, FALSE);
  context = priv->context;

  g_return_val_if_fail (pinos_context_get_state (context) == PINOS_CONTEXT_STATE_CONNECTED, FALSE);

  priv->port_info.params = params;
  priv->port_info.n_params = n_params;

  if (SPA_RESULT_IS_OK (res)) {
    add_port_update (stream, PINOS_MESSAGE_PORT_UPDATE_INFO |
                             PINOS_MESSAGE_PORT_UPDATE_FORMAT);
    if (priv->format) {
      add_state_change (stream, SPA_NODE_STATE_READY);
    } else {
      clear_buffers (stream);
      add_state_change (stream, SPA_NODE_STATE_CONFIGURE);
    }
  }
  priv->port_info.params = NULL;
  priv->port_info.n_params = 0;

  add_async_complete (stream, priv->pending_seq, res);

  priv->pending_seq = SPA_ID_INVALID;

  if (!pinos_connection_flush (priv->conn))
    pinos_log_warn ("stream %p: error writing connection", stream);

  return TRUE;
}

static gboolean
do_start (PinosStream *stream)
{
  g_object_unref (stream);
  return FALSE;
}

/**
 * pinos_stream_start:
 * @stream: a #PinosStream
 *
 * Start capturing from @stream.
 *
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_stream_start (PinosStream     *stream)
{
  PinosStreamPrivate *priv;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);

  priv = stream->priv;
  //g_return_val_if_fail (priv->state == PINOS_STREAM_STATE_PAUSED, FALSE);

  g_main_context_invoke (priv->context->priv->context,
                         (GSourceFunc) do_start,
                         g_object_ref (stream));

  return TRUE;
}

static gboolean
do_stop (PinosStream *stream)
{
  g_object_unref (stream);
  return FALSE;
}

/**
 * pinos_stream_stop:
 * @stream: a #PinosStream
 *
 * Stop capturing from @stream.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_stream_stop (PinosStream *stream)
{
  PinosStreamPrivate *priv;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);

  priv = stream->priv;
  g_return_val_if_fail (priv->state == PINOS_STREAM_STATE_STREAMING, FALSE);

  g_main_context_invoke (priv->context->priv->context,
                         (GSourceFunc) do_stop,
                         g_object_ref (stream));

  return TRUE;
}

static void
on_node_removed (GObject      *source_object,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  PinosStream *stream = user_data;
  PinosStreamPrivate *priv = stream->priv;
  GVariant *ret;
  GError *error = NULL;

  g_assert (priv->node == G_DBUS_PROXY (source_object));

  priv->disconnecting = FALSE;
  g_clear_object (&priv->node);

  ret = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &error);
  if (ret == NULL)
    goto proxy_failed;

  g_variant_unref (ret);

  unhandle_socket (stream);

  stream_set_state (stream, PINOS_STREAM_STATE_UNCONNECTED, NULL);
  g_object_unref (stream);
  return;

  /* ERRORS */
proxy_failed:
  {
    pinos_log_warn ("failed to disconnect: %s", error->message);
    stream_set_state (stream, PINOS_STREAM_STATE_ERROR, error);
    g_object_unref (stream);
    return;
  }
}


static gboolean
do_disconnect (PinosStream *stream)
{
  PinosStreamPrivate *priv = stream->priv;

  g_dbus_proxy_call (priv->node,
                     "Remove",
                     g_variant_new ("()"),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     NULL, /* GCancellable *cancellable */
                     on_node_removed,
                     stream);

  return FALSE;
}

/**
 * pinos_stream_disconnect:
 * @stream: a #PinosStream
 *
 * Disconnect @stream.
 *
 * Returns: %TRUE on success
 */
gboolean
pinos_stream_disconnect (PinosStream *stream)
{
  PinosStreamPrivate *priv;
  PinosContext *context;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);
  priv = stream->priv;
  g_return_val_if_fail (priv->state >= PINOS_STREAM_STATE_READY, FALSE);
  g_return_val_if_fail (priv->node != NULL, FALSE);
  context = priv->context;
  g_return_val_if_fail (pinos_context_get_state (context) >= PINOS_CONTEXT_STATE_CONNECTED, FALSE);
  g_return_val_if_fail (!priv->disconnecting, FALSE);

  priv->disconnecting = TRUE;

  g_main_context_invoke (context->priv->context,
                         (GSourceFunc) do_disconnect,
                         g_object_ref (stream));

  return TRUE;
}

gboolean
pinos_stream_get_time (PinosStream     *stream,
                       PinosTime       *time)
{
  PinosStreamPrivate *priv;
  gint64 now, elapsed;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);
  priv = stream->priv;
  g_return_val_if_fail (time, FALSE);

  now = g_get_monotonic_time ();
  elapsed = now - (priv->last_monotonic / 1000);

  time->ticks = priv->last_ticks + (elapsed * priv->last_rate) / G_USEC_PER_SEC;
  time->rate = priv->last_rate;

  return TRUE;
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
guint
pinos_stream_get_empty_buffer (PinosStream *stream)
{
  PinosStreamPrivate *priv;
  BufferId *bid;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);
  priv = stream->priv;
  g_return_val_if_fail (priv->direction == SPA_DIRECTION_OUTPUT, FALSE);

  pinos_array_for_each (bid, &priv->buffer_ids) {
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
 * Returns: %TRUE on success.
 */
gboolean
pinos_stream_recycle_buffer (PinosStream     *stream,
                             guint            id)
{
  PinosStreamPrivate *priv;
  SpaNodeEventReuseBuffer rb;
  uint8_t cmd = PINOS_TRANSPORT_CMD_HAVE_EVENT;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);
  g_return_val_if_fail (id != SPA_ID_INVALID, FALSE);
  priv = stream->priv;
  g_return_val_if_fail (priv->direction == SPA_DIRECTION_INPUT, FALSE);

  rb.event.type = SPA_NODE_EVENT_TYPE_REUSE_BUFFER;
  rb.event.size = sizeof (rb);
  rb.port_id = priv->port_id;
  rb.buffer_id = id;
  pinos_transport_add_event (priv->trans, &rb.event);
  write (priv->rtfd, &cmd, 1);

  return TRUE;
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
pinos_stream_peek_buffer (PinosStream  *stream, guint id)
{
  BufferId *bid;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), NULL);

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
 * Returns: %TRUE when @id was handled
 */
gboolean
pinos_stream_send_buffer (PinosStream     *stream,
                          guint            id)
{
  PinosStreamPrivate *priv;
  BufferId *bid;
  guint i;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);
  g_return_val_if_fail (id != SPA_ID_INVALID, FALSE);
  priv = stream->priv;
  g_return_val_if_fail (priv->direction == SPA_DIRECTION_OUTPUT, FALSE);

  if ((bid = find_buffer (stream, id))) {
    uint8_t cmd = PINOS_TRANSPORT_CMD_HAVE_DATA;

    bid->used = TRUE;
    for (i = 0; i < bid->buf->n_datas; i++) {
      bid->datas[i].size = bid->buf->datas[i].size;
    }
    priv->trans->outputs[0].buffer_id = id;
    priv->trans->outputs[0].status = SPA_RESULT_OK;
    write (priv->rtfd, &cmd, 1);
    return TRUE;
  } else {
    return FALSE;
  }
}
