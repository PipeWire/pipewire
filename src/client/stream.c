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

#include <string.h>
#include <gio/gunixfdlist.h>

#include "server/daemon.h"
#include "client/pinos.h"
#include "client/context.h"
#include "client/stream.h"
#include "client/enumtypes.h"

#include "client/private.h"

struct _PinosStreamPrivate
{
  PinosContext *context;
  gchar *name;
  PinosProperties *properties;

  guint id;

  PinosStreamState state;
  GError *error;

  gchar *source_path;
  GBytes *accepted_formats;
  gboolean provide;

  GBytes *possible_formats;
  GBytes *format;

  GDBusProxy *source_output;
  gboolean disconnecting;

  PinosStreamMode mode;
  GSocket *socket;
  GSource *socket_source;

  PinosStackBuffer buffer;
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
  PROP_SOCKET,
};

enum
{
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

    case PROP_SOCKET:
      g_value_set_object (value, priv->socket);
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
                  PinosStreamState  state)
{
  if (stream->priv->state != state) {
    stream->priv->state = state;
    g_main_context_invoke (stream->priv->context->priv->context,
                          (GSourceFunc) do_notify_state,
                          g_object_ref (stream));
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
    case PINOS_SUBSCRIPTION_FLAG_SOURCE_OUTPUT:
      if (event == PINOS_SUBSCRIPTION_EVENT_REMOVE) {
        if (object == priv->source_output && !priv->disconnecting) {
          priv->error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CLOSED, "output disappeared");
          stream_set_state (stream, PINOS_STREAM_STATE_ERROR);
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

  g_clear_object (&priv->source_output);

  if (priv->possible_formats)
    g_bytes_unref (priv->possible_formats);
  if (priv->format)
    g_bytes_unref (priv->format);

  g_free (priv->source_path);
  if (priv->accepted_formats)
    g_bytes_unref (priv->accepted_formats);

  g_clear_error (&priv->error);

  if (priv->properties)
    pinos_properties_free (priv->properties);
  g_signal_handler_disconnect (priv->context->priv->subscribe, priv->id);
  g_clear_object (&priv->context);
  g_free (priv->name);

  g_free (priv->buffer.data);
  if (priv->buffer.message)
    g_object_unref (priv->buffer.message);

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
                                                       G_TYPE_BYTES,
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
                                                       G_TYPE_BYTES,
                                                       G_PARAM_READABLE |
                                                       G_PARAM_STATIC_STRINGS));
  /**
   * PinosStream:socket
   *
   * The socket of the stream. When doing pinos_stream_start() with
   * #PINOS_STREAM_MODE_SOCKET, the socket will contain a data stream with
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
   * PinosStream:new-buffer
   *
   * When doing pinos_stream_start() with #PINOS_STREAM_MODE_BUFFER, this signal
   * will be fired whenever a new buffer can be obtained with
   * pinos_stream_capture_buffer().
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
pinos_stream_init (PinosStream * stream)
{
  PinosStreamPrivate *priv = stream->priv = PINOS_STREAM_GET_PRIVATE (stream);

  priv->state = PINOS_STREAM_STATE_UNCONNECTED;
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
on_source_output_proxy (GObject      *source_object,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  PinosStream *stream = user_data;
  PinosStreamPrivate *priv = stream->priv;
  PinosContext *context = priv->context;
  GVariant *v;
  gchar *str;
  GError *error = NULL;

  priv->source_output = pinos_subscribe_get_proxy_finish (context->priv->subscribe,
                                                       res,
                                                       &error);
  if (priv->source_output == NULL)
    goto source_output_failed;

  v = g_dbus_proxy_get_cached_property (priv->source_output, "PossibleFormats");
  if (v) {
    gsize len;
    str = g_variant_dup_string (v, &len);
    g_variant_unref (v);

    if (priv->possible_formats)
      g_bytes_unref (priv->possible_formats);
    priv->possible_formats = g_bytes_new_take (str, len + 1);

    g_object_notify (G_OBJECT (stream), "possible-formats");
  }
  v = g_dbus_proxy_get_cached_property (priv->source_output, "Properties");
  if (v) {
    if (priv->properties)
      pinos_properties_free (priv->properties);
    priv->properties = pinos_properties_from_variant (v);
    g_variant_unref (v);

    g_object_notify (G_OBJECT (stream), "properties");
  }

  stream_set_state (stream, PINOS_STREAM_STATE_READY);
  g_object_unref (stream);

  return;

source_output_failed:
  {
    priv->error = error;
    stream_set_state (stream, PINOS_STREAM_STATE_ERROR);
    g_warning ("failed to get source output proxy: %s", error->message);
    g_object_unref (stream);
    return;
  }
}

static void
on_source_output_created (GObject      *source_object,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  PinosStream *stream = user_data;
  PinosStreamPrivate *priv = stream->priv;
  PinosContext *context = priv->context;
  GVariant *ret;
  GError *error = NULL;
  const gchar *source_output_path;

  g_assert (context->priv->client == G_DBUS_PROXY (source_object));

  ret = g_dbus_proxy_call_finish (context->priv->client, res, &error);
  if (ret == NULL)
    goto create_failed;

  g_variant_get (ret, "(o)", &source_output_path);

  pinos_subscribe_get_proxy (context->priv->subscribe,
                          PINOS_DBUS_SERVICE,
                          source_output_path,
                          "org.pinos.SourceOutput1",
                          NULL,
                          on_source_output_proxy,
                          stream);
  g_variant_unref (ret);

  return;

  /* ERRORS */
create_failed:
  {
    priv->error = error;
    stream_set_state (stream, PINOS_STREAM_STATE_ERROR);
    g_warning ("failed to get connect capture: %s", error->message);
    g_object_unref (stream);
    return;
  }
}

static gboolean
do_connect_capture (PinosStream *stream)
{
  PinosStreamPrivate *priv = stream->priv;
  PinosContext *context = priv->context;

  g_dbus_proxy_call (context->priv->client,
                     "CreateSourceOutput",
                     g_variant_new ("(ss@a{sv})",
                       (priv->source_path ? priv->source_path : ""),
                       g_bytes_get_data (priv->accepted_formats, NULL),
                       pinos_properties_to_variant (priv->properties)),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     NULL, /* GCancellable *cancellable */
                     on_source_output_created,
                     stream);

  return FALSE;
}

/**
 * pinos_stream_connect_capture:
 * @stream: a #PinosStream
 * @source_path: the source path to connect to
 * @flags: a #PinosStreamFlags
 * @accepted_formats: (transfer full): a #GBytes with accepted formats
 *
 * Connect @stream for capturing from @source_path.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_stream_connect_capture (PinosStream      *stream,
                              const gchar      *source_path,
                              PinosStreamFlags  flags,
                              GBytes           *accepted_formats)
{
  PinosStreamPrivate *priv;
  PinosContext *context;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);
  g_return_val_if_fail (accepted_formats != NULL, FALSE);

  priv = stream->priv;
  context = priv->context;
  g_return_val_if_fail (pinos_context_get_state (context) == PINOS_CONTEXT_STATE_READY, FALSE);
  g_return_val_if_fail (pinos_stream_get_state (stream) == PINOS_STREAM_STATE_UNCONNECTED, FALSE);

  g_free (priv->source_path);
  priv->source_path = g_strdup (source_path);
  if (priv->accepted_formats)
    g_bytes_unref (priv->accepted_formats);
  priv->accepted_formats = accepted_formats;
  priv->provide = FALSE;

  stream_set_state (stream, PINOS_STREAM_STATE_CONNECTING);

  g_main_context_invoke (context->priv->context,
                         (GSourceFunc) do_connect_capture,
                         g_object_ref (stream));

  return TRUE;
}

static gboolean
do_connect_provide (PinosStream *stream)
{
  PinosStreamPrivate *priv = stream->priv;
  PinosContext *context = priv->context;

  g_dbus_proxy_call (context->priv->client,
                     "CreateSourceInput",
                     g_variant_new ("(s@a{sv})",
                       g_bytes_get_data (priv->possible_formats, NULL),
                       pinos_properties_to_variant (priv->properties)),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     NULL, /* GCancellable *cancellable */
                     on_source_output_created,
                     stream);

  return FALSE;
}

/**
 * pinos_stream_connect_provide:
 * @stream: a #PinosStream
 * @flags: a #PinosStreamFlags
 * @possible_formats: (transfer full): a #GBytes
 *
 * Connect @stream for providing data for a new source.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_stream_connect_provide (PinosStream      *stream,
                              PinosStreamFlags  flags,
                              GBytes           *possible_formats)
{
  PinosStreamPrivate *priv;
  PinosContext *context;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);
  g_return_val_if_fail (possible_formats != NULL, FALSE);

  priv = stream->priv;
  context = priv->context;
  g_return_val_if_fail (pinos_context_get_state (context) == PINOS_CONTEXT_STATE_READY, FALSE);

  if (priv->possible_formats)
    g_bytes_unref (priv->possible_formats);
  priv->possible_formats = possible_formats;
  priv->provide = TRUE;

  stream_set_state (stream, PINOS_STREAM_STATE_CONNECTING);

  g_main_context_invoke (context->priv->context,
                         (GSourceFunc) do_connect_provide,
                         g_object_ref (stream));

  return TRUE;
}

static void
on_source_output_removed (GObject      *source_object,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  PinosStream *stream = user_data;
  PinosStreamPrivate *priv = stream->priv;
  GVariant *ret;
  GError *error = NULL;

  g_assert (priv->source_output == G_DBUS_PROXY (source_object));

  priv->disconnecting = FALSE;
  g_clear_object (&priv->source_output);

  ret = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &error);
  if (ret == NULL)
    goto proxy_failed;

  stream_set_state (stream, PINOS_STREAM_STATE_UNCONNECTED);
  g_object_unref (stream);
  return;

  /* ERRORS */
proxy_failed:
  {
    priv->error = error;
    stream_set_state (stream, PINOS_STREAM_STATE_ERROR);
    g_warning ("failed to disconnect: %s", error->message);
    g_object_unref (stream);
    return;
  }
}

static gboolean
do_disconnect (PinosStream *stream)
{
  PinosStreamPrivate *priv = stream->priv;

  g_dbus_proxy_call (priv->source_output,
                     "Remove",
                     g_variant_new ("()"),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     NULL, /* GCancellable *cancellable */
                     on_source_output_removed,
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
  g_return_val_if_fail (priv->source_output != NULL, FALSE);
  context = priv->context;
  g_return_val_if_fail (pinos_context_get_state (context) >= PINOS_CONTEXT_STATE_READY, FALSE);
  g_return_val_if_fail (!priv->disconnecting, FALSE);

  priv->disconnecting = TRUE;

  g_main_context_invoke (context->priv->context,
                         (GSourceFunc) do_disconnect,
                         g_object_ref (stream));

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
      gssize len;
      GInputVector ivec;
      PinosStackHeader *hdr;
      GSocketControlMessage **messages = NULL;
      gint num_messages = 0;
      gint flags = 0;
      gsize need;
      GError *error = NULL;
      gint i;

      need = sizeof (PinosStackHeader);

      if (priv->buffer.allocated_size < need) {
        priv->buffer.allocated_size = need;
        priv->buffer.data = g_realloc (priv->buffer.data, need);
      }

      hdr = priv->buffer.data;

      /* read header first */
      ivec.buffer = hdr;
      ivec.size = sizeof (PinosStackHeader);

      len = g_socket_receive_message (socket,
                                      NULL,
                                      &ivec,
                                      1,
                                      &messages,
                                      &num_messages,
                                      &flags,
                                      NULL,
                                      &error);

      g_assert (len == sizeof (PinosStackHeader));

      if (num_messages == 0)
        break;

      /* now we know the total length */
      need += hdr->length;

      if (priv->buffer.allocated_size < need) {
        priv->buffer.allocated_size = need;
        hdr = priv->buffer.data = g_realloc (priv->buffer.data, need);
      }
      priv->buffer.size = need;

      /* read data */
      len = g_socket_receive (socket,
                              (gchar *)priv->buffer.data + sizeof (PinosStackHeader),
                              hdr->length,
                              NULL,
                              &error);
      g_assert (len == hdr->length);

      /* handle control messages */
      for (i = 0; i < num_messages; i++) {
        if (i == 0) {
          if (priv->buffer.message)
            g_object_unref (priv->buffer.message);
          priv->buffer.message = messages[0];
        }
        else {
          g_warning ("discarding control message %d", i);
          g_object_unref (messages[i]);
        }
      }
      g_free (messages);

      priv->buffer.magic = PSB_MAGIC;

      g_signal_emit (stream, signals[SIGNAL_NEW_BUFFER], 0, NULL);
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

  switch (priv->mode) {
    case PINOS_STREAM_MODE_SOCKET:
      g_object_notify (G_OBJECT (stream), "socket");
      break;

    case PINOS_STREAM_MODE_BUFFER:
    {
      if (!priv->provide) {
        priv->socket_source = g_socket_create_source (priv->socket, G_IO_IN, NULL);
        g_source_set_callback (priv->socket_source, (GSourceFunc) on_socket_condition, stream, NULL);
        g_source_attach (priv->socket_source, priv->context->priv->context);
      }
      break;
    }

    default:
      break;
  }
  return;

  /* ERRORS */
socket_failed:
  {
    priv->error = error;
    stream_set_state (stream, PINOS_STREAM_STATE_ERROR);
    g_warning ("failed to create socket: %s", error->message);
    return;
  }
}

static void
unhandle_socket (PinosStream *stream)
{
  PinosStreamPrivate *priv = stream->priv;

  switch (priv->mode) {
    case PINOS_STREAM_MODE_SOCKET:
      g_clear_object (&priv->socket);
      g_object_notify (G_OBJECT (stream), "socket");
      break;

    case PINOS_STREAM_MODE_BUFFER:
      if (priv->socket_source) {
        g_source_destroy (priv->socket_source);
        g_clear_pointer (&priv->socket_source, g_source_unref);
      }
      break;

    default:
      break;
  }
}

static void
on_stream_started (GObject      *source_object,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  PinosStream *stream = user_data;
  PinosStreamPrivate *priv = stream->priv;
  GUnixFDList *out_fd_list;
  gint fd_idx, fd;
  gchar *format;
  GError *error = NULL;
  GVariant *result, *properties;

  result = g_dbus_proxy_call_with_unix_fd_list_finish (priv->source_output,
                                                       &out_fd_list,
                                                       res,
                                                       &error);
  if (result == NULL)
    goto start_failed;

  g_variant_get (result,
                 "(hs@a{sv})",
                 &fd_idx,
                 &format,
                 &properties);

  g_variant_unref (result);

  if (priv->format)
    g_bytes_unref (priv->format);
  priv->format = g_bytes_new_take (format, strlen (format) + 1);
  g_object_notify (G_OBJECT (stream), "format");

  if (priv->properties)
    pinos_properties_free (priv->properties);
  priv->properties = pinos_properties_from_variant (properties);
  g_variant_unref (properties);

  g_object_notify (G_OBJECT (stream), "properties");

  if ((fd = g_unix_fd_list_get (out_fd_list, fd_idx, &error)) < 0)
    goto fd_failed;

  handle_socket (stream, fd);

  stream_set_state (stream, PINOS_STREAM_STATE_STREAMING);

  return;

  /* ERRORS */
start_failed:
  {
    g_warning ("failed to start: %s", error->message);
    goto exit_error;
  }
fd_failed:
  {
    g_warning ("failed to get FD: %s", error->message);
    goto exit_error;
  }
exit_error:
  {
    priv->error = error;
    stream_set_state (stream, PINOS_STREAM_STATE_ERROR);
    return;
  }
}

static gboolean
do_start (PinosStream *stream)
{
  PinosStreamPrivate *priv = stream->priv;

  g_dbus_proxy_call (priv->source_output,
                     "Start",
                     g_variant_new ("(s)", g_bytes_get_data (priv->format, NULL)),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     NULL,        /* GCancellable *cancellable */
                     on_stream_started,
                     stream);

  return FALSE;
}

/**
 * pinos_stream_start:
 * @stream: a #PinosStream
 * @format: (transfer full): a #GBytes with format
 * @mode: a #PinosStreamMode
 *
 * Start capturing from @stream in @format.
 *
 * When @mode is #PINOS_STREAM_MODE_SOCKET, you should connect to the notify::socket
 * signal to obtain a readable socket with metadata and data.
 *
 * When @mode is #PINOS_STREAM_MODE_BUFFER, you should connect to the new-buffer
 * signal and use pinos_stream_capture_buffer() to get the latest metadata and
 * data.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_stream_start (PinosStream     *stream,
                    GBytes          *format,
                    PinosStreamMode  mode)
{
  PinosStreamPrivate *priv;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);

  priv = stream->priv;
  g_return_val_if_fail (priv->state == PINOS_STREAM_STATE_READY, FALSE);

  priv->mode = mode;
  priv->format = format;

  stream_set_state (stream, PINOS_STREAM_STATE_STARTING);

  g_main_context_invoke (priv->context->priv->context, (GSourceFunc) do_start, stream);

  return TRUE;
}

static void
on_stream_stopped (GObject      *source_object,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  PinosStream *stream = user_data;
  PinosStreamPrivate *priv = stream->priv;
  GVariant *ret;
  GError *error = NULL;

  ret = g_dbus_proxy_call_finish (priv->source_output, res, &error);
  if (ret == NULL)
    goto call_failed;

  g_variant_unref (ret);

  unhandle_socket (stream);
  g_clear_pointer (&priv->format, g_free);
  g_object_notify (G_OBJECT (stream), "format");

  stream_set_state (stream, PINOS_STREAM_STATE_READY);

  return;

  /* ERRORS */
call_failed:
  {
    priv->error = error;
    stream_set_state (stream, PINOS_STREAM_STATE_ERROR);
    g_warning ("failed to release: %s", error->message);
    return;
  }
}

static gboolean
do_stop (PinosStream *stream)
{
  PinosStreamPrivate *priv = stream->priv;

  g_dbus_proxy_call (priv->source_output,
                     "Stop",
                     g_variant_new ("()"),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     NULL,        /* GCancellable *cancellable */
                     on_stream_stopped,
                     stream);


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

  g_main_context_invoke (priv->context->priv->context, (GSourceFunc) do_stop, stream);

  return TRUE;
}

/**
 * pinos_stream_capture_buffer:
 * @stream: a #PinosStream
 * @buffer: a #PinosBuffer
 *
 * Capture the next buffer from @stream. This function should be called every
 * time after the new-buffer callback has been emitted.
 *
 * Returns: %TRUE when @buffer contains valid information
 */
gboolean
pinos_stream_capture_buffer (PinosStream  *stream,
                             PinosBuffer  *buffer)
{
  PinosStreamPrivate *priv;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);
  g_return_val_if_fail (buffer != NULL, FALSE);

  priv = stream->priv;
  g_return_val_if_fail (priv->state == PINOS_STREAM_STATE_STREAMING, FALSE);
  g_return_val_if_fail (is_valid_buffer (&priv->buffer), FALSE);

  memcpy (buffer, &priv->buffer, sizeof (PinosStackBuffer));

  priv->buffer.data = NULL;
  priv->buffer.allocated_size = 0;
  priv->buffer.size = 0;
  priv->buffer.message = NULL;
  priv->buffer.magic = 0;

  return TRUE;
}

/**
 * pinos_stream_release_buffer:
 * @stream: a #PinosStream
 * @buffer: a #PinosBuffer
 *
 * Release @buffer back to @stream. This function should be called whenever the
 * buffer is processed. @buffer should not be used anymore after calling this
 * function.
 */
void
pinos_stream_release_buffer (PinosStream  *stream,
                             PinosBuffer  *buffer)
{
  PinosStackBuffer *sb = (PinosStackBuffer *) buffer;
  PinosStreamPrivate *priv;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);
  g_return_val_if_fail (is_valid_buffer (buffer), FALSE);

  priv = stream->priv;

  if (priv->buffer.data == NULL) {
    priv->buffer.data = sb->data;
    priv->buffer.allocated_size = sb->allocated_size;
    priv->buffer.size = 0;
  }
  else
    g_free (sb->data);

  if (sb->message)
    g_object_unref (sb->message);

  sb->magic = 0;
}

/**
 * pinos_stream_provide_buffer:
 * @stream: a #PinosStream
 * @buffer: a #PinosBuffer
 *
 * Provide the next buffer from @stream. This function should be called every
 * time a new frame becomes available.
 *
 * Returns: %TRUE when @buffer was handled
 */
gboolean
pinos_stream_provide_buffer (PinosStream *stream,
                             PinosBuffer *buffer)
{
  PinosStreamPrivate *priv;
  gssize len;
  PinosStackBuffer *sb = (PinosStackBuffer *) buffer;
  GOutputVector ovec[1];
  gint flags = 0;
  GError *error = NULL;

  g_return_val_if_fail (PINOS_IS_STREAM (stream), FALSE);
  g_return_val_if_fail (buffer != NULL, FALSE);

  priv = stream->priv;
  g_return_val_if_fail (priv->state == PINOS_STREAM_STATE_STREAMING, FALSE);

  ovec[0].buffer = sb->data;
  ovec[0].size = sb->size;

  len = g_socket_send_message (priv->socket,
                               NULL,
                               ovec,
                               1,
                               &sb->message,
                               1,
                               flags,
                               NULL,
                               &error);
  if (sb->message) {
    g_object_unref (sb->message);
    sb->message = NULL;
  }

  if (len == -1)
    goto send_error;

  g_assert (len == (gssize) sb->size);

  return TRUE;

send_error:
  {
    priv->error = error;
    stream_set_state (stream, PINOS_STREAM_STATE_ERROR);
    g_warning ("failed to send_message: %s", error->message);
    return FALSE;
  }
}

