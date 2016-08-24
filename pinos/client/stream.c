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

#include "spa/include/spa/control.h"
#include "spa/include/spa/debug.h"
#include "spa/include/spa/memory.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gio/gunixfdmessage.h>

#include "pinos/server/daemon.h"
#include "pinos/client/pinos.h"
#include "pinos/client/context.h"
#include "pinos/client/stream.h"
#include "pinos/client/enumtypes.h"
#include "pinos/client/format.h"
#include "pinos/client/private.h"

#define MAX_BUFFER_SIZE 4096
#define MAX_FDS         16

typedef struct {
  bool cleanup;
  uint32_t id;
  int fd;
  off_t offset;
  size_t size;
  bool used;
  SpaBuffer *buf;
} BufferId;

static void
clear_buffer_id (BufferId *id)
{
  munmap (id->buf, id->size);
  id->buf = NULL;
  close (id->fd);
  id->fd = -1;
}

struct _PinosStreamPrivate
{
  PinosContext *context;
  gchar *name;
  PinosProperties *properties;

  guint id;

  PinosStreamState state;
  GError *error;

  PinosDirection direction;
  gchar *path;

  GPtrArray *possible_formats;
  SpaFormat *format;
  SpaPortInfo port_info;

  PinosStreamFlags flags;

  GDBusProxy *node;
  gboolean disconnecting;

  PinosStreamMode mode;
  GSocket *socket;
  GSource *socket_source;
  int fd;

  SpaControl *control;
  SpaControl recv_control;
  guint8 recv_data[MAX_BUFFER_SIZE];
  int recv_fds[MAX_FDS];

  guint8 send_data[MAX_BUFFER_SIZE];
  int send_fds[MAX_FDS];

  GArray *buffer_ids;
  gboolean in_order;
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
        spa_format_unref (priv->format);
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

  g_debug ("free stream %p", stream);

  g_clear_object (&priv->node);

  if (priv->possible_formats)
    g_ptr_array_unref (priv->possible_formats);
  if (priv->format)
    spa_format_unref (priv->format);

  g_free (priv->path);
  g_clear_error (&priv->error);

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

  g_debug ("new stream %p", stream);

  priv->state = PINOS_STREAM_STATE_UNCONNECTED;
  priv->buffer_ids = g_array_sized_new (FALSE, FALSE, sizeof (BufferId), 64);
  g_array_set_clear_func (priv->buffer_ids, (GDestroyNotify) clear_buffer_id);
  priv->in_order = TRUE;
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
control_builder_init (PinosStream  *stream, SpaControlBuilder *builder)
{
  PinosStreamPrivate *priv = stream->priv;

  spa_control_builder_init_into (builder,
                                 priv->send_data,
                                 MAX_BUFFER_SIZE,
                                 priv->send_fds,
                                 MAX_FDS);
}

static void
add_node_update (PinosStream *stream, SpaControlBuilder *builder, uint32_t change_mask)
{
  PinosStreamPrivate *priv = stream->priv;
  SpaControlCmdNodeUpdate nu = { 0, };

  nu.change_mask = change_mask;
  if (change_mask & SPA_CONTROL_CMD_NODE_UPDATE_MAX_INPUTS)
    nu.max_input_ports = priv->direction == PINOS_DIRECTION_INPUT ? 1 : 0;
  if (change_mask & SPA_CONTROL_CMD_NODE_UPDATE_MAX_OUTPUTS)
    nu.max_output_ports = priv->direction == PINOS_DIRECTION_OUTPUT ? 1 : 0;
  nu.props = NULL;
  spa_control_builder_add_cmd (builder, SPA_CONTROL_CMD_NODE_UPDATE, &nu);
}

static void
add_port_update (PinosStream *stream, SpaControlBuilder *builder, uint32_t change_mask)
{
  PinosStreamPrivate *priv = stream->priv;
  SpaControlCmdPortUpdate pu = { 0, };;

  pu.port_id = 0;
  pu.change_mask = change_mask;
  if (change_mask & SPA_CONTROL_CMD_PORT_UPDATE_DIRECTION)
    pu.direction = priv->direction;
  if (change_mask & SPA_CONTROL_CMD_PORT_UPDATE_POSSIBLE_FORMATS) {
    pu.n_possible_formats = priv->possible_formats->len;
    pu.possible_formats = (SpaFormat **)priv->possible_formats->pdata;
  }
  pu.props = NULL;
  if (change_mask & SPA_CONTROL_CMD_PORT_UPDATE_INFO)
    pu.info = &priv->port_info;
  spa_control_builder_add_cmd (builder, SPA_CONTROL_CMD_PORT_UPDATE, &pu);
}

static void
add_state_change (PinosStream *stream, SpaControlBuilder *builder, SpaNodeState state)
{
  SpaControlCmdStateChange sc;

  sc.state = state;
  spa_control_builder_add_cmd (builder, SPA_CONTROL_CMD_STATE_CHANGE, &sc);
}

static void
add_need_input (PinosStream *stream, SpaControlBuilder *builder, uint32_t port_id)
{
  SpaControlCmdNeedInput ni;

  ni.port_id = port_id;
  spa_control_builder_add_cmd (builder, SPA_CONTROL_CMD_NEED_INPUT, &ni);
}

static void
send_need_input (PinosStream *stream, uint32_t port_id)
{
  PinosStreamPrivate *priv = stream->priv;
  SpaControlBuilder builder;
  SpaControl control;

  control_builder_init (stream, &builder);
  add_need_input (stream, &builder, port_id);
  spa_control_builder_end (&builder, &control);

  if (spa_control_write (&control, priv->fd) < 0)
    g_warning ("stream %p: error writing control", stream);

  spa_control_clear (&control);
}

static void
send_reuse_buffer (PinosStream *stream, uint32_t port_id, uint32_t buffer_id)
{
  PinosStreamPrivate *priv = stream->priv;
  SpaControlBuilder builder;
  SpaControl control;
  SpaControlCmdReuseBuffer rb;

  control_builder_init (stream, &builder);
  rb.port_id = port_id;
  rb.buffer_id = buffer_id;
  spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_REUSE_BUFFER, &rb);
  spa_control_builder_end (&builder, &control);

  if (spa_control_write (&control, priv->fd) < 0)
    g_warning ("stream %p: error writing control", stream);

  spa_control_clear (&control);
}

static void
send_process_buffer (PinosStream *stream, uint32_t port_id, uint32_t buffer_id)
{
  PinosStreamPrivate *priv = stream->priv;
  SpaControlBuilder builder;
  SpaControl control;
  SpaControlCmdProcessBuffer pb;
  SpaControlCmdHaveOutput ho;

  control_builder_init (stream, &builder);
  pb.port_id = port_id;
  pb.buffer_id = buffer_id;
  spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_PROCESS_BUFFER, &pb);
  ho.port_id = port_id;
  spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_HAVE_OUTPUT, &ho);
  spa_control_builder_end (&builder, &control);

  if (spa_control_write (&control, priv->fd) < 0)
    g_warning ("stream %p: error writing control", stream);

  spa_control_clear (&control);
}

static BufferId *
find_buffer (PinosStream *stream, uint32_t id)
{
  PinosStreamPrivate *priv = stream->priv;
  guint i;

  if (priv->in_order && id < priv->buffer_ids->len) {
    return &g_array_index (priv->buffer_ids, BufferId, id);
  } else {
    for (i = 0; i < priv->buffer_ids->len; i++) {
      BufferId *bid = &g_array_index (priv->buffer_ids, BufferId, i);
      if (bid->id == id)
        return bid;
    }
  }
  return NULL;
}

static gboolean
parse_control (PinosStream *stream,
               SpaControl  *ctrl)
{
  SpaControlIter it;
  PinosStreamPrivate *priv = stream->priv;

  spa_control_iter_init (&it, ctrl);
  while (spa_control_iter_next (&it) == SPA_RESULT_OK) {
    SpaControlCmd cmd = spa_control_iter_get_cmd (&it);

    switch (cmd) {
      case SPA_CONTROL_CMD_NODE_UPDATE:
      case SPA_CONTROL_CMD_PORT_UPDATE:
      case SPA_CONTROL_CMD_PORT_REMOVED:
      case SPA_CONTROL_CMD_STATE_CHANGE:
      case SPA_CONTROL_CMD_PORT_STATUS_CHANGE:
      case SPA_CONTROL_CMD_NEED_INPUT:
      case SPA_CONTROL_CMD_HAVE_OUTPUT:
        g_warning ("got unexpected control %d", cmd);
        break;

      case SPA_CONTROL_CMD_ADD_PORT:
      case SPA_CONTROL_CMD_REMOVE_PORT:
        g_warning ("add/remove port not supported");
        break;

      case SPA_CONTROL_CMD_SET_FORMAT:
      {
        SpaControlCmdSetFormat p;

        if (spa_control_iter_parse_cmd (&it, &p) < 0)
          break;

        if (priv->format)
          spa_format_unref (priv->format);
        priv->format = p.format;

        spa_debug_format (p.format);
        g_object_notify (G_OBJECT (stream), "format");

        if (priv->port_info.n_params != 0) {
          SpaControlBuilder builder;
          SpaControl control;

          control_builder_init (stream, &builder);
          add_state_change (stream, &builder, SPA_NODE_STATE_READY);
          spa_control_builder_end (&builder, &control);

          if (spa_control_write (&control, priv->fd) < 0)
            g_warning ("stream %p: error writing control", stream);

          spa_control_clear (&control);
        }
        break;
      }
      case SPA_CONTROL_CMD_SET_PROPERTY:
        g_warning ("set property not implemented");
        break;

      case SPA_CONTROL_CMD_START:
      {
        SpaControlBuilder builder;
        SpaControl control;

        g_debug ("stream %p: start", stream);

        control_builder_init (stream, &builder);
        if (priv->direction == PINOS_DIRECTION_INPUT)
          add_need_input (stream, &builder, 0);
        add_state_change (stream, &builder, SPA_NODE_STATE_STREAMING);
        spa_control_builder_end (&builder, &control);

        if (spa_control_write (&control, priv->fd) < 0)
          g_warning ("stream %p: error writing control", stream);

        spa_control_clear (&control);

        stream_set_state (stream, PINOS_STREAM_STATE_STREAMING, NULL);
        break;
      }
      case SPA_CONTROL_CMD_STOP:
      {
        SpaControlBuilder builder;
        SpaControl control;

        g_debug ("stream %p: stop", stream);

        control_builder_init (stream, &builder);
        add_state_change (stream, &builder, SPA_NODE_STATE_PAUSED);
        spa_control_builder_end (&builder, &control);

        if (spa_control_write (&control, priv->fd) < 0)
          g_warning ("stream %p: error writing control", stream);

        spa_control_clear (&control);

        stream_set_state (stream, PINOS_STREAM_STATE_READY, NULL);
        break;
      }
      case SPA_CONTROL_CMD_ADD_MEM:
      {
        SpaControlCmdAddMem p;
        SpaMemory *mem;
        int fd;

        if (spa_control_iter_parse_cmd (&it, &p) < 0)
          break;

        fd = spa_control_get_fd (ctrl, p.fd_index, false);
        if (fd == -1)
          break;

        mem = spa_memory_import (&p.mem);
        if (mem->fd == -1) {
          g_debug ("add mem %d,%d, %d, %d", p.mem.pool_id, p.mem.id, fd, p.flags);
          mem->flags = p.flags;
          mem->fd = fd;
          mem->ptr = NULL;
          mem->size = p.size;
        } else {
          g_debug ("duplicated mem %d,%d, %d, %d", p.mem.pool_id, p.mem.id, fd, p.flags);
          close (fd);
        }
        break;
      }
      case SPA_CONTROL_CMD_REMOVE_MEM:
      {
        SpaControlCmdRemoveMem p;

        if (spa_control_iter_parse_cmd (&it, &p) < 0)
          break;

        g_debug ("stream %p: remove mem", stream);
        spa_memory_unref (&p.mem);
        break;
      }
      case SPA_CONTROL_CMD_ADD_BUFFER:
      {
        SpaControlCmdAddBuffer p;
        BufferId bid;
        SpaMemory *mem;

        if (spa_control_iter_parse_cmd (&it, &p) < 0)
          break;

        g_debug ("add buffer %d, %d", p.buffer_id, p.mem.mem.id);
        mem = spa_memory_find (&p.mem.mem);
        bid.cleanup = false;
        bid.id = p.buffer_id;
        bid.offset = p.mem.offset;
        bid.size = p.mem.size;
        bid.buf = SPA_MEMBER (spa_memory_ensure_ptr (mem), p.mem.offset, SpaBuffer);

        if (bid.id != priv->buffer_ids->len) {
          g_warning ("unexpected id %u found, expected %u", bid.id, priv->buffer_ids->len);
          priv->in_order = FALSE;
        }
        g_array_append_val (priv->buffer_ids, bid);
        g_signal_emit (stream, signals[SIGNAL_ADD_BUFFER], 0, p.buffer_id);
        break;
      }
      case SPA_CONTROL_CMD_REMOVE_BUFFER:
      {
        SpaControlCmdRemoveBuffer p;
        BufferId *bid;

        if (spa_control_iter_parse_cmd (&it, &p) < 0)
          break;

        g_debug ("remove buffer %d", p.buffer_id);
        if ((bid = find_buffer (stream, p.buffer_id))) {
          bid->cleanup = TRUE;
          bid->used = TRUE;
          g_signal_emit (stream, signals[SIGNAL_REMOVE_BUFFER], 0, p.buffer_id);
        }
        break;
      }
      case SPA_CONTROL_CMD_PROCESS_BUFFER:
      {
        SpaControlCmdProcessBuffer p;

        if (priv->direction != PINOS_DIRECTION_INPUT)
          break;

        if (spa_control_iter_parse_cmd (&it, &p) < 0)
          break;

        g_signal_emit (stream, signals[SIGNAL_NEW_BUFFER], 0, p.buffer_id);

          send_need_input (stream, 0);
        break;
      }
      case SPA_CONTROL_CMD_REUSE_BUFFER:
      {
        SpaControlCmdReuseBuffer p;
        BufferId *bid;

        if (priv->direction != PINOS_DIRECTION_OUTPUT)
          break;

        if (spa_control_iter_parse_cmd (&it, &p) < 0)
          break;

        g_debug ("reuse buffer %d", p.buffer_id);
        if ((bid = find_buffer (stream, p.buffer_id))) {
          bid->used = FALSE;
          g_signal_emit (stream, signals[SIGNAL_NEW_BUFFER], 0, p.buffer_id);
        }
        break;
      }

      case SPA_CONTROL_CMD_INVALID:
        g_warning ("unhandled command %d", cmd);
        break;
    }
  }
  spa_control_iter_end (&it);

  return TRUE;
}

static gboolean
on_socket_condition (GSocket      *socket,
                     GIOCondition  condition,
                     gpointer      user_data)
{
  PinosStream *stream = user_data;
  PinosStreamPrivate *priv = stream->priv;

  switch (condition) {
    case G_IO_IN:
    {
      SpaControl *control = &priv->recv_control;
      guint i;

      if (spa_control_read (control,
                            priv->fd,
                            priv->recv_data,
                            MAX_BUFFER_SIZE,
                            priv->recv_fds,
                            MAX_FDS) < 0) {
        g_warning ("stream %p: failed to read buffer", stream);
        return TRUE;
      }

      parse_control (stream, control);

      for (i = 0; i < priv->buffer_ids->len; i++) {
        BufferId *bid = &g_array_index (priv->buffer_ids, BufferId, i);
        if (bid->cleanup) {
          g_array_remove_index_fast (priv->buffer_ids, i);
          i--;
          priv->in_order = FALSE;
        }
      }
      if (!priv->in_order && priv->buffer_ids->len == 0)
        priv->in_order = TRUE;

      spa_control_clear (control);
      break;
    }

    case G_IO_OUT:
      g_warning ("can do IO\n");
      break;

    default:
      break;
  }
  return TRUE;
}

static void
handle_socket (PinosStream *stream, gint fd)
{
  PinosStreamPrivate *priv = stream->priv;
  GError *error = NULL;

  priv->socket = g_socket_new_from_fd (fd, &error);
  if (priv->socket == NULL)
    goto socket_failed;

  priv->fd = g_socket_get_fd (priv->socket);
  priv->socket_source = g_socket_create_source (priv->socket, G_IO_IN, NULL);
  g_source_set_callback (priv->socket_source, (GSourceFunc) on_socket_condition, stream, NULL);
  g_source_attach (priv->socket_source, priv->context->priv->context);

  return;

  /* ERRORS */
socket_failed:
  {
    g_warning ("failed to create socket: %s", error->message);
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
}

static void
on_node_proxy (GObject      *source_object,
               GAsyncResult *res,
               gpointer      user_data)
{
  PinosStream *stream = user_data;
  PinosStreamPrivate *priv = stream->priv;
  PinosContext *context = priv->context;
  SpaControlBuilder builder;
  SpaControl control;

  GError *error = NULL;

  priv->node = pinos_subscribe_get_proxy_finish (context->priv->subscribe,
                                                 res,
                                                 &error);
  if (priv->node == NULL)
    goto node_failed;

  control_builder_init (stream, &builder);
  add_node_update (stream, &builder, SPA_CONTROL_CMD_NODE_UPDATE_MAX_INPUTS |
                                     SPA_CONTROL_CMD_NODE_UPDATE_MAX_OUTPUTS);

  priv->port_info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
  add_port_update (stream, &builder, SPA_CONTROL_CMD_PORT_UPDATE_DIRECTION |
                                     SPA_CONTROL_CMD_PORT_UPDATE_POSSIBLE_FORMATS |
                                     SPA_CONTROL_CMD_PORT_UPDATE_INFO);

  add_state_change (stream, &builder, SPA_NODE_STATE_CONFIGURE);

  spa_control_builder_end (&builder, &control);

  if (spa_control_write (&control, priv->fd) < 0)
    g_warning ("stream %p: error writing control", stream);

  spa_control_clear (&control);

  stream_set_state (stream, PINOS_STREAM_STATE_READY, NULL);
  g_object_unref (stream);

  return;

node_failed:
  {
    g_warning ("failed to get node proxy: %s", error->message);
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

  g_assert (context->priv->daemon == G_DBUS_PROXY (source_object));

  ret = g_dbus_proxy_call_with_unix_fd_list_finish (context->priv->daemon,
                                                    &fd_list,
                                                    res, &error);
  if (ret == NULL)
    goto create_failed;

  g_variant_get (ret, "(&oh)", &node_path, &fd_idx);
  g_variant_unref (ret);

  if ((fd = g_unix_fd_list_get (fd_list, fd_idx, &error)) < 0)
    goto fd_failed;

  priv->fd = fd;
  g_object_unref (fd_list);

  handle_socket (stream, priv->fd);

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
    g_warning ("failed to connect: %s", error->message);
    goto exit_error;
  }
fd_failed:
  {
    g_warning ("failed to get FD: %s", error->message);
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

  priv->direction = direction;
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
 * pinos_stream_start_allocation:
 * @stream: a #PinosStream
 * @props: a #PinosProperties
 *
 * Returns: %TRUE on success
 */
gboolean
pinos_stream_start_allocation (PinosStream     *stream,
                               SpaAllocParam  **params,
                               unsigned int     n_params)
{
  PinosStreamPrivate *priv;
  PinosContext *context;
  SpaControlBuilder builder;
  SpaControl control;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);
  priv = stream->priv;
  context = priv->context;

  g_return_val_if_fail (pinos_context_get_state (context) == PINOS_CONTEXT_STATE_CONNECTED, FALSE);

  control_builder_init (stream, &builder);

  priv->port_info.params = params;
  priv->port_info.n_params = n_params;

  /* send update port status */
  add_port_update (stream, &builder, SPA_CONTROL_CMD_PORT_UPDATE_INFO);

  /* send state-change */
  if (priv->format)
    add_state_change (stream, &builder, SPA_NODE_STATE_READY);

  spa_control_builder_end (&builder, &control);

  if (spa_control_write (&control, priv->fd) < 0)
    g_warning ("stream %p: error writing control", stream);

  spa_control_clear (&control);

  return TRUE;
}

static gboolean
do_start (PinosStream *stream)
{
  PinosStreamPrivate *priv = stream->priv;
  SpaControlBuilder builder;
  SpaControl control;

  control_builder_init (stream, &builder);
  add_state_change (stream, &builder, SPA_NODE_STATE_CONFIGURE);
  spa_control_builder_end (&builder, &control);

  if (spa_control_write (&control, priv->fd) < 0)
    g_warning ("stream %p: failed to write control", stream);

  spa_control_clear (&control);

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
  g_return_val_if_fail (priv->state == PINOS_STREAM_STATE_READY, FALSE);

  stream_set_state (stream, PINOS_STREAM_STATE_STARTING, NULL);

  g_main_context_invoke (priv->context->priv->context,
                         (GSourceFunc) do_start,
                         g_object_ref (stream));

  return TRUE;
}

static gboolean
do_stop (PinosStream *stream)
{
  SpaControlBuilder builder;

  control_builder_init (stream, &builder);
  spa_control_builder_add_cmd (&builder, SPA_CONTROL_CMD_STOP, NULL);
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
    g_warning ("failed to disconnect: %s", error->message);
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
  guint i;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);
  priv = stream->priv;
  g_return_val_if_fail (priv->direction == PINOS_DIRECTION_OUTPUT, FALSE);

  for (i = 0; i < priv->buffer_ids->len; i++) {
    BufferId *bid = &g_array_index (priv->buffer_ids, BufferId, i);
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

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);
  g_return_val_if_fail (id != SPA_ID_INVALID, FALSE);
  priv = stream->priv;
  g_return_val_if_fail (priv->direction == PINOS_DIRECTION_INPUT, FALSE);

  send_reuse_buffer (stream, 0, id);

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

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);
  g_return_val_if_fail (id != SPA_ID_INVALID, FALSE);
  priv = stream->priv;
  g_return_val_if_fail (priv->direction == PINOS_DIRECTION_OUTPUT, FALSE);

  if ((bid = find_buffer (stream, id))) {
    bid->used = TRUE;
    send_process_buffer (stream, 0, id);
    return TRUE;
  } else {
    return FALSE;
  }
}
