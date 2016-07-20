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
#include <sys/socket.h>
#include <errno.h>

#include <gio/gunixfdlist.h>

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"
#include "pinos/client/private.h"

#include "pinos/server/daemon.h"
#include "pinos/server/channel.h"
#include "pinos/server/utils.h"

#include "pinos/dbus/org-pinos.h"


#define MAX_BUFFER_SIZE 1024
#define MAX_FDS         16

struct _PinosChannelPrivate
{
  PinosDaemon *daemon;
  PinosChannel1 *iface;

  gchar *object_path;
  gchar *client_path;
  PinosPort *port;
  PinosDirection direction;

  GBytes *possible_formats;
  PinosProperties *properties;
  PinosChannelState state;
  GBytes *format;

  gulong send_id;
  int fd;
  GSource *socket_source;
  GSocket *sockets[2];

  PinosBuffer recv_buffer;

  guint8 recv_data[MAX_BUFFER_SIZE];
  int recv_fds[MAX_FDS];

  guint8 send_data[MAX_BUFFER_SIZE];
  int send_fds[MAX_FDS];

  GSocket *socket;
};

#define PINOS_CHANNEL_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_CHANNEL, PinosChannelPrivate))

G_DEFINE_TYPE (PinosChannel, pinos_channel, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_PORT,
  PROP_OBJECT_PATH,
  PROP_CLIENT_PATH,
  PROP_DIRECTION,
  PROP_POSSIBLE_FORMATS,
  PROP_PROPERTIES,
  PROP_FORMAT,
  PROP_SOCKET,
  PROP_STATE,
};

enum
{
  SIGNAL_REMOVE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
pinos_channel_get_property (GObject    *_object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  PinosChannel *channel = PINOS_CHANNEL (_object);
  PinosChannelPrivate *priv = channel->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      g_value_set_object (value, priv->daemon);
      break;

    case PROP_PORT:
      g_value_set_object (value, priv->port);
      break;

    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;

    case PROP_CLIENT_PATH:
      g_value_set_string (value, priv->client_path);
      break;

    case PROP_DIRECTION:
      g_value_set_enum (value, priv->direction);
      break;

    case PROP_POSSIBLE_FORMATS:
      g_value_set_boxed (value, priv->possible_formats);
      break;

    case PROP_PROPERTIES:
      g_value_set_boxed (value, priv->properties);
      break;

    case PROP_FORMAT:
      g_value_set_boxed (value, priv->format);
      break;

    case PROP_SOCKET:
      g_value_set_object (value, priv->socket);
      break;

    case PROP_STATE:
      g_value_set_uint (value, priv->state);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (channel, prop_id, pspec);
      break;
  }
}

static void
pinos_channel_set_property (GObject      *_object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  PinosChannel *channel = PINOS_CHANNEL (_object);
  PinosChannelPrivate *priv = channel->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    case PROP_PORT:
      priv->port = g_value_dup_object (value);
      break;

    case PROP_OBJECT_PATH:
      priv->object_path = g_value_dup_string (value);
      break;

    case PROP_CLIENT_PATH:
      priv->client_path = g_value_dup_string (value);
      g_object_set (priv->iface, "client", priv->client_path, NULL);
      break;

    case PROP_DIRECTION:
      priv->direction = g_value_get_enum (value);
      g_object_set (priv->iface, "direction", priv->direction, NULL);
      break;

    case PROP_POSSIBLE_FORMATS:
      if (priv->possible_formats)
        g_bytes_unref (priv->possible_formats);
      priv->possible_formats = g_value_dup_boxed (value);
      g_object_set (priv->iface, "possible-formats",
          g_bytes_get_data (priv->possible_formats, NULL), NULL);
      break;

    case PROP_PROPERTIES:
      if (priv->properties)
        pinos_properties_free (priv->properties);
      priv->properties = g_value_dup_boxed (value);
      g_object_set (priv->iface, "properties", priv->properties ?
          pinos_properties_to_variant (priv->properties) : NULL, NULL);
      break;

    case PROP_FORMAT:
      if (priv->format)
        g_bytes_unref (priv->format);
      priv->format = g_value_dup_boxed (value);
      g_object_set (priv->iface, "format", priv->format ?
          g_bytes_get_data (priv->format, NULL) : NULL, NULL);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (channel, prop_id, pspec);
      break;
  }
}

static void
clear_formats (PinosChannel *channel)
{
  PinosChannelPrivate *priv = channel->priv;

  g_debug ("channel %p: clear format", channel);

  g_clear_pointer (&priv->format, g_bytes_unref);
}

static void
stop_transfer (PinosChannel *channel)
{
  PinosChannelPrivate *priv = channel->priv;

  g_debug ("channel %p: stop transfer", channel);

  pinos_port_deactivate (priv->port);
  clear_formats (channel);
  priv->state = PINOS_CHANNEL_STATE_STOPPED;
  g_object_set (priv->iface,
               "state", priv->state,
                NULL);
}



static gboolean
handle_start (PinosChannel1          *interface,
              GDBusMethodInvocation  *invocation,
              const gchar            *arg_requested_format,
              gpointer                user_data)
{
  PinosChannel *channel = user_data;
  PinosChannelPrivate *priv = channel->priv;
  GBytes *req_format, *format;
  const gchar *format_str;
  GError *error = NULL;

  priv->state = PINOS_CHANNEL_STATE_STARTING;

  req_format = g_bytes_new (arg_requested_format, strlen (arg_requested_format) + 1);

  format = pinos_format_filter (priv->possible_formats, req_format, &error);
  if (format == NULL)
    goto no_format;

  format_str = g_bytes_get_data (format, NULL);

  g_debug ("channel %p: handle start, format %s", channel, format_str);

  g_object_set (priv->port, "possible-formats", format, NULL);
  pinos_port_activate (priv->port);
  g_object_get (priv->port, "format", &format, NULL);
  if (format == NULL)
    goto no_format_activate;

  format_str = g_bytes_get_data (format, NULL);

  priv->state = PINOS_CHANNEL_STATE_STREAMING;
  g_debug ("channel %p: we are now streaming in format \"%s\"", channel, format_str);

  g_dbus_method_invocation_return_value (invocation,
           g_variant_new ("(s@a{sv})",
                          format_str,
                          pinos_properties_to_variant (priv->properties)));
  g_object_set (priv->iface,
               "format", format_str,
               "state", priv->state,
                NULL);

  return TRUE;

no_format:
  {
    g_debug ("no format found");
    g_dbus_method_invocation_return_gerror (invocation, error);
    g_clear_error (&error);
    return TRUE;
  }
no_format_activate:
  {
    g_debug ("no format found when activating");
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "can't negotiate formats");
    return TRUE;
  }
}

static gboolean
handle_stop (PinosChannel1          *interface,
             GDBusMethodInvocation  *invocation,
             gpointer                user_data)
{
  PinosChannel *channel = user_data;

  g_debug ("channel %p: handle stop", channel);

  stop_transfer (channel);

  g_dbus_method_invocation_return_value (invocation, NULL);

  return TRUE;
}

static gboolean
handle_remove (PinosChannel1          *interface,
               GDBusMethodInvocation  *invocation,
               gpointer                user_data)
{
  PinosChannel *channel = user_data;

  g_debug ("channel %p: handle remove", channel);
  stop_transfer (channel);

  g_signal_emit (channel, signals[SIGNAL_REMOVE], 0, NULL);

  g_dbus_method_invocation_return_value (invocation, NULL);

  return TRUE;
}

static gboolean
on_send_buffer (PinosPort    *port,
                PinosBuffer  *buffer,
                GError      **error,
                gpointer      user_data)
{
  PinosChannel *channel = user_data;
  PinosChannelPrivate *priv = channel->priv;
  gboolean res;

  if (priv->state == PINOS_CHANNEL_STATE_STREAMING)
    res = pinos_io_write_buffer (priv->fd, buffer, error);
  else
    res = TRUE;

  return res;
}

static gboolean
on_socket_condition (GSocket      *socket,
                     GIOCondition  condition,
                     gpointer      user_data)
{
  PinosChannel *channel = user_data;
  PinosChannelPrivate *priv = channel->priv;

  switch (condition) {
    case G_IO_IN:
    {
      PinosBuffer *buffer = &priv->recv_buffer;
      GError *error = NULL;

      if (!pinos_io_read_buffer (priv->fd,
                                 buffer,
                                 priv->recv_data,
                                 MAX_BUFFER_SIZE,
                                 priv->recv_fds,
                                 MAX_FDS,
                                 &error)) {
        g_warning ("channel %p: failed to read buffer: %s", channel, error->message);
        g_clear_error (&error);
        return TRUE;
      }

      if (!pinos_port_receive_buffer (priv->port, buffer, &error)) {
        g_warning ("channel %p: port %p failed to receive buffer: %s", channel, priv->port, error->message);
        g_clear_error (&error);
      }
      g_assert (pinos_buffer_unref (buffer) == FALSE);
      break;
    }

    case G_IO_OUT:
      g_warning ("can do IO OUT\n");
      break;

    default:
      break;
  }
  return TRUE;
}

static void
handle_socket (PinosChannel *channel, GSocket *socket)
{
  PinosChannelPrivate *priv = channel->priv;
  GMainContext *context = g_main_context_get_thread_default();

  g_debug ("channel %p: handle socket in context %p", channel, context);
  priv->fd = g_socket_get_fd (socket);
  priv->socket_source = g_socket_create_source (socket, G_IO_IN, NULL);
  g_source_set_callback (priv->socket_source, (GSourceFunc) on_socket_condition, channel, NULL);
  g_source_attach (priv->socket_source, context);
}

static void
unhandle_socket (PinosChannel *channel)
{
  PinosChannelPrivate *priv = channel->priv;

  g_debug ("channel %p: unhandle socket", channel);
  if (priv->socket_source) {
    g_source_destroy (priv->socket_source);
    g_clear_pointer (&priv->socket_source, g_source_unref);
    priv->fd = -1;
  }
}

/**
 * pinos_channel_get_socket_pair:
 * @channel: a #PinosChannel
 * @error: a #GError
 *
 * Create or return a previously create socket pair for @channel. The
 * Socket for the other end is returned.
 *
 * Returns: a #GSocket that can be used to send/receive buffers to channel.
 */
GSocket *
pinos_channel_get_socket_pair (PinosChannel  *channel,
                               GError       **error)
{
  PinosChannelPrivate *priv;

  g_return_val_if_fail (PINOS_IS_CHANNEL (channel), FALSE);
  priv = channel->priv;

  if (priv->sockets[1] == NULL) {
    int fd[2];

    if (socketpair (AF_UNIX, SOCK_STREAM, 0, fd) != 0)
      goto no_sockets;

    priv->sockets[0] = g_socket_new_from_fd (fd[0], error);
    if (priv->sockets[0] == NULL)
      goto create_failed;

    priv->sockets[1] = g_socket_new_from_fd (fd[1], error);
    if (priv->sockets[1] == NULL)
      goto create_failed;

    handle_socket (channel, priv->sockets[0]);
  }
  return g_object_ref (priv->sockets[1]);

  /* ERRORS */
no_sockets:
  {
    g_set_error (error,
                 G_IO_ERROR,
                 g_io_error_from_errno (errno),
                 "could not create socketpair: %s", strerror (errno));
    return NULL;
  }
create_failed:
  {
    g_clear_object (&priv->sockets[0]);
    g_clear_object (&priv->sockets[1]);
    return NULL;
  }
}

static void
channel_register_object (PinosChannel *channel)
{
  PinosChannelPrivate *priv = channel->priv;
  PinosObjectSkeleton *skel;
  gchar *name;

  priv->send_id = pinos_port_add_send_buffer_cb (priv->port, on_send_buffer, channel, NULL);

  name = g_strdup_printf ("%s/channel", priv->client_path);
  skel = pinos_object_skeleton_new (name);
  g_free (name);

  pinos_object_skeleton_set_channel1 (skel, priv->iface);

  g_free (priv->object_path);
  priv->object_path = pinos_daemon_export_uniquely (priv->daemon, G_DBUS_OBJECT_SKELETON (skel));
  g_object_unref (skel);

  g_debug ("channel %p: register object %s", channel, priv->object_path);
}

static void
channel_unregister_object (PinosChannel *channel)
{
  PinosChannelPrivate *priv = channel->priv;

  pinos_port_remove_send_buffer_cb (priv->port, priv->send_id);

  g_debug ("channel %p: unregister object", channel);
  pinos_daemon_unexport (priv->daemon, priv->object_path);
}

static void
pinos_channel_dispose (GObject * object)
{
  PinosChannel *channel = PINOS_CHANNEL (object);
  PinosChannelPrivate *priv = channel->priv;

  g_debug ("channel %p: dispose", channel);
  pinos_port_deactivate (priv->port);
  clear_formats (channel);
  unhandle_socket (channel);
  g_clear_object (&priv->socket);
  channel_unregister_object (channel);

  G_OBJECT_CLASS (pinos_channel_parent_class)->dispose (object);
}

static void
pinos_channel_finalize (GObject * object)
{
  PinosChannel *channel = PINOS_CHANNEL (object);
  PinosChannelPrivate *priv = channel->priv;

  g_debug ("channel %p: finalize", channel);
  if (priv->possible_formats)
    g_bytes_unref (priv->possible_formats);
  if (priv->properties)
    pinos_properties_free (priv->properties);
  g_clear_object (&priv->port);
  g_clear_object (&priv->daemon);
  g_clear_object (&priv->iface);
  g_free (priv->client_path);
  g_free (priv->object_path);

  G_OBJECT_CLASS (pinos_channel_parent_class)->finalize (object);
}

static void
pinos_channel_constructed (GObject * object)
{
  PinosChannel *channel = PINOS_CHANNEL (object);

  g_debug ("channel %p: constructed", channel);
  channel_register_object (channel);

  G_OBJECT_CLASS (pinos_channel_parent_class)->constructed (object);
}
static void
pinos_channel_class_init (PinosChannelClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosChannelPrivate));

  gobject_class->constructed = pinos_channel_constructed;
  gobject_class->dispose = pinos_channel_dispose;
  gobject_class->finalize = pinos_channel_finalize;
  gobject_class->set_property = pinos_channel_set_property;
  gobject_class->get_property = pinos_channel_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "The Daemon",
                                                        PINOS_TYPE_DAEMON,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_PORT,
                                   g_param_spec_object ("port",
                                                        "Port",
                                                        "The Port",
                                                        PINOS_TYPE_PORT,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_OBJECT_PATH,
                                   g_param_spec_string ("object-path",
                                                        "Object Path",
                                                        "The object path",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_CLIENT_PATH,
                                   g_param_spec_string ("client-path",
                                                        "Client Path",
                                                        "The client object path",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

 g_object_class_install_property (gobject_class,
                                   PROP_DIRECTION,
                                   g_param_spec_enum ("direction",
                                                      "Direction",
                                                      "The direction of the port",
                                                      PINOS_TYPE_DIRECTION,
                                                      PINOS_DIRECTION_INVALID,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT_ONLY |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_POSSIBLE_FORMATS,
                                   g_param_spec_boxed ("possible-formats",
                                                       "Possible Formats",
                                                       "The possbile formats of the stream",
                                                       G_TYPE_BYTES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_PROPERTIES,
                                   g_param_spec_boxed ("properties",
                                                       "Properties",
                                                       "Extra properties of the stream",
                                                       PINOS_TYPE_PROPERTIES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_FORMAT,
                                   g_param_spec_boxed ("format",
                                                       "Format",
                                                       "The format of the stream",
                                                       G_TYPE_BYTES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_SOCKET,
                                   g_param_spec_object ("socket",
                                                        "Socket",
                                                        "The socket with data",
                                                        G_TYPE_SOCKET,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));

  signals[SIGNAL_REMOVE] = g_signal_new ("remove",
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
pinos_channel_init (PinosChannel * channel)
{
  PinosChannelPrivate *priv = channel->priv = PINOS_CHANNEL_GET_PRIVATE (channel);

  priv->iface = pinos_channel1_skeleton_new ();
  g_signal_connect (priv->iface, "handle-start", (GCallback) handle_start, channel);
  g_signal_connect (priv->iface, "handle-stop", (GCallback) handle_stop, channel);
  g_signal_connect (priv->iface, "handle-remove", (GCallback) handle_remove, channel);

  priv->state = PINOS_CHANNEL_STATE_STOPPED;
  g_object_set (priv->iface, "state", priv->state, NULL);

  priv->direction = PINOS_DIRECTION_INVALID;

  g_debug ("channel %p: new", channel);
}

/**
 * pinos_channel_remove:
 * @channel: a #PinosChannel
 *
 * Remove @channel. This will stop the transfer on the channel and
 * free the resources allocated by @channel.
 */
void
pinos_channel_remove (PinosChannel *channel)
{
  g_debug ("channel %p: remove", channel);
  stop_transfer (channel);

  g_signal_emit (channel, signals[SIGNAL_REMOVE], 0, NULL);
}

const gchar *
pinos_channel_get_client_path (PinosChannel *channel)
{
  PinosChannelPrivate *priv;

  g_return_val_if_fail (PINOS_IS_CHANNEL (channel), NULL);
  priv = channel->priv;

  return priv->client_path;
}

/**
 * pinos_channel_get_object_path:
 * @channel: a #PinosChannel
 *
 * Get the object patch of @channel
 *
 * Returns: the object path of @source.
 */
const gchar *
pinos_channel_get_object_path (PinosChannel *channel)
{
  PinosChannelPrivate *priv;

  g_return_val_if_fail (PINOS_IS_CHANNEL (channel), NULL);
  priv = channel->priv;

  return priv->object_path;
}
