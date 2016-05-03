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

#include <gio/gunixfdlist.h>

#include "pinos/client/enumtypes.h"

#include "pinos/server/daemon.h"
#include "pinos/server/channel.h"

#include "pinos/dbus/org-pinos.h"

struct _PinosChannelPrivate
{
  PinosDaemon *daemon;
  PinosChannel1 *iface;

  gchar *object_path;
  gchar *client_path;
  gchar *owner_path;

  GBytes *possible_formats;
  PinosProperties *properties;
  GBytes *requested_format;
  PinosChannelState state;
  GBytes *format;

  GSocket *socket;
};

#define PINOS_CHANNEL_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_CHANNEL, PinosChannelPrivate))

G_DEFINE_TYPE (PinosChannel, pinos_channel, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_OBJECT_PATH,
  PROP_CLIENT_PATH,
  PROP_OWNER_PATH,
  PROP_POSSIBLE_FORMATS,
  PROP_PROPERTIES,
  PROP_REQUESTED_FORMAT,
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

    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;

    case PROP_CLIENT_PATH:
      g_value_set_string (value, priv->client_path);
      break;

    case PROP_OWNER_PATH:
      g_value_set_string (value, priv->owner_path);
      break;

    case PROP_POSSIBLE_FORMATS:
      g_value_set_boxed (value, priv->possible_formats);
      break;

    case PROP_PROPERTIES:
      g_value_set_boxed (value, priv->properties);
      break;

    case PROP_REQUESTED_FORMAT:
      g_value_set_boxed (value, priv->requested_format);
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

    case PROP_OBJECT_PATH:
      priv->object_path = g_value_dup_string (value);
      break;

    case PROP_CLIENT_PATH:
      priv->client_path = g_value_dup_string (value);
      g_object_set (priv->iface, "client", priv->client_path, NULL);
      break;

    case PROP_OWNER_PATH:
      priv->owner_path = g_value_dup_string (value);
      g_object_set (priv->iface, "owner", priv->owner_path, NULL);
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
      g_object_set (priv->iface, "properties",
          pinos_properties_to_variant (priv->properties), NULL);
      break;

    case PROP_FORMAT:
      if (priv->format)
        g_bytes_unref (priv->format);
      priv->format = g_value_dup_boxed (value);
      g_object_set (priv->iface, "format",
          g_bytes_get_data (priv->format, NULL), NULL);
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

  g_clear_pointer (&priv->requested_format, g_bytes_unref);
  g_clear_pointer (&priv->format, g_bytes_unref);
}

static void
stop_transfer (PinosChannel *channel)
{
  PinosChannelPrivate *priv = channel->priv;

  g_debug ("channel %p: stop transfer", channel);

  if (priv->socket) {
    g_clear_object (&priv->socket);
    g_object_notify (G_OBJECT (channel), "socket");
  }
  clear_formats (channel);
  priv->state = PINOS_CHANNEL_STATE_IDLE;
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
  GUnixFDList *fdlist;
  gint fd[2];
  const gchar *format;

  priv->state = PINOS_CHANNEL_STATE_STARTING;

  priv->requested_format = g_bytes_new (arg_requested_format,
                                        strlen (arg_requested_format) + 1);

  socketpair (AF_UNIX, SOCK_STREAM, 0, fd);

  g_debug ("channel %p: handle start, fd[%d,%d]", channel, fd[0], fd[1]);

  g_clear_object (&priv->socket);
  priv->socket = g_socket_new_from_fd (fd[0], NULL);
  g_object_set_data (G_OBJECT (priv->socket), "pinos-client-path", priv->client_path);

  g_debug ("channel %p: notify socket %p, path %s", channel, priv->socket, priv->client_path);
  g_object_notify (G_OBJECT (channel), "socket");

  /* the notify of the socket above should configure the format */
  if (priv->format == NULL)
    goto no_format;

  format = g_bytes_get_data (priv->format, NULL);

  priv->state = PINOS_CHANNEL_STATE_STREAMING;
  g_debug ("channel %p: we are now streaming in format \"%s\"", channel, format);

  fdlist = g_unix_fd_list_new ();
  g_unix_fd_list_append (fdlist, fd[1], NULL);

  g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
           g_variant_new ("(hs@a{sv})",
                          0,
                          format,
                          pinos_properties_to_variant (priv->properties)),
           fdlist);
  g_object_unref (fdlist);
  close (fd[1]);

  g_object_set (priv->iface,
               "format", format,
               "state", priv->state,
                NULL);

  return TRUE;

  /* error */
no_format:
  {
    g_debug ("channel %p: no format configured", channel);
    g_dbus_method_invocation_return_dbus_error (invocation,
        "org.pinos.Error", "No format");
    close (fd[0]);
    close (fd[1]);
    g_clear_pointer (&priv->requested_format, g_bytes_unref);
    g_clear_object (&priv->socket);
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

static void
channel_register_object (PinosChannel *channel,
                        const gchar       *prefix)
{
  PinosChannelPrivate *priv = channel->priv;
  PinosObjectSkeleton *skel;
  gchar *name;

  name = g_strdup_printf ("%s/channel", prefix);
  skel = pinos_object_skeleton_new (name);
  g_free (name);

  pinos_object_skeleton_set_channel1 (skel, priv->iface);

  g_free (priv->object_path);
  priv->object_path = pinos_daemon_export_uniquely (priv->daemon, G_DBUS_OBJECT_SKELETON (skel));
  g_debug ("channel %p: register object %s", channel, priv->object_path);
}

static void
channel_unregister_object (PinosChannel *channel)
{
  PinosChannelPrivate *priv = channel->priv;

  g_debug ("channel %p: unregister object", channel);
  pinos_daemon_unexport (priv->daemon, priv->object_path);
}

static void
pinos_channel_dispose (GObject * object)
{
  PinosChannel *channel = PINOS_CHANNEL (object);
  PinosChannelPrivate *priv = channel->priv;

  g_debug ("channel %p: dispose", channel);
  clear_formats (channel);
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
  g_clear_object (&priv->daemon);
  g_clear_object (&priv->iface);
  g_free (priv->client_path);
  g_free (priv->object_path);
  g_free (priv->owner_path);

  G_OBJECT_CLASS (pinos_channel_parent_class)->finalize (object);
}

static void
pinos_channel_constructed (GObject * object)
{
  PinosChannel *channel = PINOS_CHANNEL (object);
  PinosChannelPrivate *priv = channel->priv;

  g_debug ("channel %p: constructed", channel);
  channel_register_object (channel, priv->object_path);

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
                                   PROP_OWNER_PATH,
                                   g_param_spec_string ("owner-path",
                                                        "Owner Path",
                                                        "The owner object path",
                                                        NULL,
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
                                   PROP_REQUESTED_FORMAT,
                                   g_param_spec_boxed ("requested-format",
                                                       "Requested Format",
                                                       "The requested format of the stream",
                                                       G_TYPE_BYTES,
                                                       G_PARAM_READABLE |
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

  priv->state = PINOS_CHANNEL_STATE_IDLE;
  g_object_set (priv->iface, "state", priv->state, NULL);

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
