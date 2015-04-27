/* Pulsevideo
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

#include <gio/gunixfdlist.h>

#include "server/pv-daemon.h"
#include "client/pv-context.h"
#include "client/pv-stream.h"
#include "client/pv-enumtypes.h"

#include "dbus/org-pulsevideo.h"

struct _PvStreamPrivate
{
  PvContext *context;
  gchar *name;
  gchar *target;
  PvStreamState state;

  gchar *source_output_path;
  PvSource1 *source;
  PvSourceOutput1 *source_output;

  GSocket *socket;
  PvStreamMode mode;
  guint socket_id;

  PvBufferInfo info;
};

#define PV_STREAM_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PV_TYPE_STREAM, PvStreamPrivate))

G_DEFINE_TYPE (PvStream, pv_stream, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_CONTEXT,
  PROP_NAME,
  PROP_STATE,
  PROP_SOCKET
};

enum
{
  SIGNAL_NEW_BUFFER,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
pv_stream_get_property (GObject    *_object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  PvStream *stream = PV_STREAM (_object);
  PvStreamPrivate *priv = stream->priv;

  switch (prop_id) {
    case PROP_CONTEXT:
      g_value_set_object (value, priv->context);
      break;

    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    case PROP_STATE:
      g_value_set_enum (value, priv->state);
      break;

    case PROP_SOCKET:
      g_value_set_object (value, priv->socket);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (stream, prop_id, pspec);
      break;
    }
}

static void
pv_stream_set_property (GObject      *_object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  PvStream *stream = PV_STREAM (_object);
  PvStreamPrivate *priv = stream->priv;

  switch (prop_id) {
    case PROP_CONTEXT:
      priv->context = g_value_dup_object (value);
      break;

    case PROP_NAME:
      priv->name = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (stream, prop_id, pspec);
      break;
    }
}

static void
pv_stream_finalize (GObject * object)
{
  PvStream *stream = PV_STREAM (object);
  PvStreamPrivate *priv = stream->priv;

  g_free (priv->name);

  G_OBJECT_CLASS (pv_stream_parent_class)->finalize (object);
}

static void
pv_stream_class_init (PvStreamClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PvStreamPrivate));

  gobject_class->finalize = pv_stream_finalize;
  gobject_class->set_property = pv_stream_set_property;
  gobject_class->get_property = pv_stream_get_property;

  /**
   * PvStream:context
   *
   * The context of the stream.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_CONTEXT,
                                   g_param_spec_object ("context",
                                                        "Context",
                                                        "The context",
                                                        PV_TYPE_CONTEXT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
  /**
   * PvStream:name
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
   * PvStream:state
   *
   * The state of the stream. Use the notify::state signal to be notified
   * of state changes.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_STATE,
                                   g_param_spec_enum ("state",
                                                      "State",
                                                      "The stream state",
                                                      PV_TYPE_STREAM_STATE,
                                                      PV_STREAM_STATE_UNCONNECTED,
                                                      G_PARAM_READABLE |
                                                      G_PARAM_STATIC_STRINGS));

  /**
   * PvStream:socket
   *
   * The socket of the stream. When doing pv_stream_start() with
   * #PV_STREAM_MODE_SOCKET, the socket will contain a data stream with
   * meta data and anciliary data containing fds with the data.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_SOCKET,
                                   g_param_spec_object ("socket",
                                                        "Socket",
                                                        "The stream socket",
                                                        G_TYPE_SOCKET,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));


  /**
   * PvStream:new-buffer
   *
   * When doing pv_stream_start() with #PV_STREAM_MODE_BUFFER, this signal
   * will be fired whenever a new buffer can be obtained with
   * pv_stream_capture_buffer().
   */
  signals[SIGNAL_NEW_BUFFER] = g_signal_new ("new-buffer",
                                             G_TYPE_FROM_CLASS (klass),
                                             G_SIGNAL_RUN_LAST,
                                             0,
                                             NULL,
                                             NULL,
                                             g_cclosure_marshal_generic,
                                             G_TYPE_NONE,
                                             0,
                                             G_TYPE_NONE);
}

static void
pv_stream_init (PvStream * stream)
{
  PvStreamPrivate *priv = stream->priv = PV_STREAM_GET_PRIVATE (stream);

  priv->state = PV_STREAM_STATE_UNCONNECTED;
}

/**
 * pv_stream_new:
 * @context: a #PvContext
 * @name: a stream name
 *
 * Make a new unconnected #PvStream
 *
 * Returns: a new unconnected #PvStream
 */
PvStream *
pv_stream_new (PvContext * context, const gchar *name)
{
  g_return_val_if_fail (PV_IS_CONTEXT (context), NULL);
  g_return_val_if_fail (name != NULL, NULL);

  return g_object_new (PV_TYPE_STREAM, "context", context, "name", name, NULL);
}

static void
stream_set_state (PvStream *stream, PvStreamState state)
{
  if (stream->priv->state != state) {
    stream->priv->state = state;
    g_object_notify (G_OBJECT (stream), "state");
  }
}

/**
 * pv_stream_get_state:
 * @stream: a #PvStream
 *
 * Get the state of @stream.
 *
 * Returns: the state of @stream
 */
PvStreamState
pv_stream_get_state (PvStream *stream)
{
  g_return_val_if_fail (PV_IS_STREAM (stream), PV_STREAM_STATE_ERROR);

  return stream->priv->state;
}

static void
on_request_reconfigure (PvSourceOutput1 *interface,
                        GVariant *props,
                        gpointer user_data)
{
  g_print ("on request reconfigure\n");
}

static void
on_source_output1_proxy (GObject *source_object,
                         GAsyncResult *res,
                         gpointer user_data)
{
  PvStream *stream = user_data;
  PvStreamPrivate *priv = stream->priv;
  GError *error = NULL;

  priv->source_output = pv_source_output1_proxy_new_finish (res, &error);
  if (priv->source_output == NULL)
    goto source_output_failed;

  g_signal_connect (priv->source_output, "request-reconfigure", (GCallback) on_request_reconfigure, stream);
  stream_set_state (stream, PV_STREAM_STATE_READY);
  return;

  /* ERRORS */
source_output_failed:
  {
    stream_set_state (stream, PV_STREAM_STATE_ERROR);
    g_error ("failed to get source output proxy: %s", error->message);
    g_clear_error (&error);
    return;
  }
}

static void
on_source_output_created (GObject *source_object,
                          GAsyncResult *res,
                          gpointer user_data)
{
  PvStream *stream = user_data;
  PvStreamPrivate *priv = stream->priv;
  PvContext *context = priv->context;
  GError *error = NULL;

  if (!pv_source1_call_create_source_output_finish (priv->source,
        &priv->source_output_path, res, &error))
    goto create_failed;

  g_print ("got source-output %s\n", priv->source_output_path);

  pv_source_output1_proxy_new (pv_context_get_connection (context),
                               G_DBUS_PROXY_FLAGS_NONE,
                               g_dbus_proxy_get_name (G_DBUS_PROXY (priv->source)),
                               priv->source_output_path,
                               NULL,
                               on_source_output1_proxy,
                               stream);
  return;

  /* ERRORS */
create_failed:
  {
    stream_set_state (stream, PV_STREAM_STATE_ERROR);
    g_print ("failed to get connect capture: %s", error->message);
    g_clear_error (&error);
    return;
  }
}

static void
on_source_output_removed (GObject *source_object,
                          GAsyncResult *res,
                          gpointer user_data)
{
  PvStream *stream = user_data;
  PvStreamPrivate *priv = stream->priv;
  GError *error = NULL;

  if (!pv_source_output1_call_remove_finish (priv->source_output,
        res, &error)) {
    stream_set_state (stream, PV_STREAM_STATE_ERROR);
    g_print ("failed to disconnect: %s", error->message);
    g_clear_error (&error);
    return;
  }
  g_clear_pointer (&priv->source_output_path, g_free);
  g_clear_object (&priv->source_output);
}

static gboolean
remove_source_output (PvStream *stream)
{
  PvStreamPrivate *priv = stream->priv;

  pv_source_output1_call_remove (priv->source_output,
                                 NULL,        /* GCancellable *cancellable */
                                 on_source_output_removed,
                                 stream);
  return TRUE;
}

/**
 * pv_stream_connect_capture:
 * @stream: a #PvStream
 * @source: the source name to connect to
 * @flags: a #PvStreamFlags
 *
 * Connect @stream for capturing from @source.
 *
 * Returns: %TRUE on success.
 */
gboolean
pv_stream_connect_capture (PvStream      *stream,
                           const gchar   *source,
                           PvStreamFlags  flags)
{
  PvStreamPrivate *priv;
  GVariantBuilder builder;
  PvContext *context;

  g_return_val_if_fail (PV_IS_STREAM (stream), FALSE);
  priv = stream->priv;
  context = priv->context;
  g_return_val_if_fail (pv_context_get_state (context) == PV_CONTEXT_STATE_READY, FALSE);

  priv->target = g_strdup (source);

  stream_set_state (stream, PV_STREAM_STATE_CONNECTING);

  priv->source = PV_SOURCE1 (pv_context_find_source (context, priv->target, NULL));
  if (priv->source == NULL) {
    g_warning ("can't find source");
    return FALSE;
  }

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&builder, "{sv}", "name", g_variant_new_string ("hello"));

  pv_source1_call_create_source_output (priv->source,
                                        g_variant_builder_end (&builder), /* GVariant *arg_props */
                                        NULL,        /* GCancellable *cancellable */
                                        on_source_output_created,
                                        stream);
  return TRUE;
}

/**
 * pv_stream_disconnect:
 * @stream: a #PvStream
 *
 * Disconnect @stream.
 *
 * Returns: %TRUE on success
 */
gboolean
pv_stream_disconnect (PvStream *stream)
{
  PvStreamPrivate *priv;
  PvContext *context;

  g_return_val_if_fail (PV_IS_STREAM (stream), FALSE);
  priv = stream->priv;
  g_return_val_if_fail (priv->state >= PV_STREAM_STATE_READY, FALSE);
  g_return_val_if_fail (priv->source_output != NULL, FALSE);
  context = priv->context;
  g_return_val_if_fail (pv_context_get_state (context) == PV_CONTEXT_STATE_READY, FALSE);

  remove_source_output (stream);

  stream_set_state (stream, PV_STREAM_STATE_UNCONNECTED);

  return TRUE;
}

#include <gst/wire-protocol.h>

static gboolean
on_socket_data (GSocket       *socket,
                GIOCondition   condition,
                gpointer       user_data)
{
  PvStream *stream = user_data;
  PvStreamPrivate *priv = stream->priv;

  switch (condition) {
    case G_IO_IN:
    {
      gssize len;
      GInputVector ivec;
      FDMessage msg;
      GSocketControlMessage **messages = NULL;
      gint num_messages = 0;
      gint flags = 0;
      GError *error = NULL;

      ivec.buffer = &msg;
      ivec.size = sizeof (msg);

      len = g_socket_receive_message (socket,
                                      NULL,
                                      &ivec,
                                      1,
                                      &messages,
                                      &num_messages,
                                      &flags,
                                      NULL,
                                      &error);

      g_assert (len == sizeof (msg));

      if (priv->info.message)
        g_object_unref (priv->info.message);

      priv->info.flags = msg.flags;
      priv->info.seq = msg.seq;
      priv->info.pts = msg.pts;
      priv->info.dts_offset = msg.dts_offset;
      priv->info.offset = msg.offset;
      priv->info.size = msg.size;
      priv->info.message = num_messages > 0 ? messages[0] : NULL;

      g_signal_emit (stream, SIGNAL_NEW_BUFFER, 0, NULL);
      break;
    }

    default:
      break;
  }
  return TRUE;
}


static void
handle_socket (PvStream *stream, gint fd)
{
  PvStreamPrivate *priv = stream->priv;
  GError *error = NULL;

  g_print ("got fd %d\n", fd);
  priv->socket = g_socket_new_from_fd (fd, &error);
  if (priv->socket == NULL)
    goto socket_failed;

  switch (priv->mode) {
    case PV_STREAM_MODE_SOCKET:
      g_object_notify (G_OBJECT (stream), "socket");
      break;

    case PV_STREAM_MODE_BUFFER:
    {
      GSource *source;

      source = g_socket_create_source (priv->socket, G_IO_IN, NULL);
      g_source_set_callback (source, (GSourceFunc) on_socket_data, stream, NULL);
      priv->socket_id = g_source_attach (source, NULL);
      g_source_unref (source);
      break;
    }

    default:
      break;
  }
  return;

  /* ERRORS */
socket_failed:
  {
    stream_set_state (stream, PV_STREAM_STATE_ERROR);
    g_error ("failed to create socket: %s", error->message);
    g_clear_error (&error);
    return;
  }
}

static void
unhandle_socket (PvStream *stream)
{
  PvStreamPrivate *priv = stream->priv;

  switch (priv->mode) {
    case PV_STREAM_MODE_SOCKET:
      g_clear_object (&priv->socket);
      g_object_notify (G_OBJECT (stream), "socket");
      break;

    default:
      break;
  }
}

static void
on_stream_started (GObject *source_object,
                   GAsyncResult *res,
                   gpointer user_data)
{
  PvStream *stream = user_data;
  PvStreamPrivate *priv = stream->priv;
  GUnixFDList *out_fd_list;
  gint fd_idx, fd;
  GVariant *out_props;
  GError *error = NULL;
  GVariant *result;

  result = g_dbus_proxy_call_with_unix_fd_list_finish (G_DBUS_PROXY (priv->source_output),
                                                       &out_fd_list,
                                                       res,
                                                       &error);
  if (result == NULL)
    goto start_failed;

  g_variant_get (result,
                 "(h@a{sv})",
                 &fd_idx,
                 &out_props);

  g_variant_unref (result);
  g_variant_unref (out_props);

  if ((fd = g_unix_fd_list_get (out_fd_list, fd_idx, &error)) < 0)
    goto fd_failed;

  handle_socket (stream, fd);

  stream_set_state (stream, PV_STREAM_STATE_STREAMING);

  return;

  /* ERRORS */
start_failed:
  {
    g_error ("failed to start: %s", error->message);
    goto exit_error;
  }
fd_failed:
  {
    g_error ("failed to get FD: %s", error->message);
    goto exit_error;
  }
exit_error:
  {
    stream_set_state (stream, PV_STREAM_STATE_ERROR);
    g_clear_error (&error);
    return;
  }
}

/**
 * pv_stream_start:
 * @stream: a #PvStream
 * @mode: a #PvStreamMode
 *
 * Start capturing from @stream.
 *
 * When @mode is #PV_STREAM_MODE_SOCKET, you should connect to the notify::socket
 * signal to obtain a readable socket with metadata and data.
 *
 * When @mode is #PV_STREAM_MODE_BUFFER, you should connect to the new-buffer
 * signal and use pv_stream_capture_buffer() to get the latest metadata and
 * data.
 *
 * Returns: %TRUE on success.
 */
gboolean
pv_stream_start (PvStream *stream, PvStreamMode mode)
{
  PvStreamPrivate *priv;

  g_return_val_if_fail (PV_IS_STREAM (stream), FALSE);

  priv = stream->priv;
  g_return_val_if_fail (priv->state == PV_STREAM_STATE_READY, FALSE);

  priv->mode = mode;

  stream_set_state (stream, PV_STREAM_STATE_STARTING);
  {
    GVariantBuilder builder;

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&builder, "{sv}", "name", g_variant_new_string ("hello"));

    pv_source_output1_call_start (priv->source_output,
                                  g_variant_builder_end (&builder), /* GVariant *arg_properties */
                                  NULL,        /* GCancellable *cancellable */
                                  on_stream_started,
                                  stream);
  }
  return TRUE;
}

static void
on_stream_stopped (GObject *source_object,
                   GAsyncResult *res,
                   gpointer user_data)
{
  PvStream *stream = user_data;
  PvStreamPrivate *priv = stream->priv;
  GError *error = NULL;

  if (!pv_source_output1_call_stop_finish (priv->source_output,
               res, &error))
    goto call_failed;

  unhandle_socket (stream);

  stream_set_state (stream, PV_STREAM_STATE_READY);

  return;

  /* ERRORS */
call_failed:
  {
    stream_set_state (stream, PV_STREAM_STATE_ERROR);
    g_error ("failed to release: %s", error->message);
    g_clear_error (&error);
    return;
  }
}

/**
 * pv_stream_stop:
 * @stream: a #PvStream
 *
 * Stop capturing from @stream.
 *
 * Returns: %TRUE on success.
 */
gboolean
pv_stream_stop (PvStream *stream)
{
  PvStreamPrivate *priv;

  g_return_val_if_fail (PV_IS_STREAM (stream), FALSE);

  priv = stream->priv;
  g_return_val_if_fail (priv->state == PV_STREAM_STATE_STREAMING, FALSE);

  pv_source_output1_call_stop (priv->source_output,
                               NULL,        /* GCancellable *cancellable */
                               on_stream_stopped,
                               stream);

  return TRUE;
}

/**
 * pv_stream_capture_buffer:
 * @stream: a #PvStream
 * @info: a #PvBufferInfo
 *
 * Capture the next buffer from @stream. This function should be called every
 * time after the new-buffer callback has been emitted.
 *
 * Returns: %TRUE when @info contains valid information
 */
gboolean
pv_stream_capture_buffer (PvStream *stream, PvBufferInfo *info)
{
  PvStreamPrivate *priv;

  g_return_val_if_fail (PV_IS_STREAM (stream), FALSE);
  g_return_val_if_fail (info != NULL, FALSE);

  priv = stream->priv;
  g_return_val_if_fail (priv->state == PV_STREAM_STATE_STREAMING, FALSE);

  *info = priv->info;

  return TRUE;
}
