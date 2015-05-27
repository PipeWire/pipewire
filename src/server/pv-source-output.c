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

#include <string.h>
#include <sys/socket.h>

#include <gio/gunixfdlist.h>

#include "client/pv-enumtypes.h"

#include "server/pv-daemon.h"
#include "server/pv-source-output.h"

#include "dbus/org-pulsevideo.h"

struct _PvSourceOutputPrivate
{
  PvDaemon *daemon;
  PvSourceOutput1 *iface;

  gchar *object_path;
  gchar *client_path;
  gchar *source_path;

  GBytes *possible_formats;
  GBytes *requested_format;
  GBytes *format;

  GSocket *socket;
};

#define PV_SOURCE_OUTPUT_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PV_TYPE_SOURCE_OUTPUT, PvSourceOutputPrivate))

G_DEFINE_TYPE (PvSourceOutput, pv_source_output, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_OBJECT_PATH,
  PROP_CLIENT_PATH,
  PROP_SOURCE_PATH,
  PROP_POSSIBLE_FORMATS,
  PROP_REQUESTED_FORMAT,
  PROP_FORMAT,
  PROP_SOCKET,
};

enum
{
  SIGNAL_REMOVE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
pv_source_output_get_property (GObject    *_object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  PvSourceOutput *output = PV_SOURCE_OUTPUT (_object);
  PvSourceOutputPrivate *priv = output->priv;

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

    case PROP_SOURCE_PATH:
      g_value_set_string (value, priv->source_path);
      break;

    case PROP_POSSIBLE_FORMATS:
      g_value_set_boxed (value, priv->possible_formats);
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

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (output, prop_id, pspec);
      break;
  }
}

static void
pv_source_output_set_property (GObject      *_object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  PvSourceOutput *output = PV_SOURCE_OUTPUT (_object);
  PvSourceOutputPrivate *priv = output->priv;

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

    case PROP_SOURCE_PATH:
      priv->source_path = g_value_dup_string (value);
      g_object_set (priv->iface, "source", priv->source_path, NULL);
      break;

    case PROP_POSSIBLE_FORMATS:
      if (priv->possible_formats)
        g_bytes_unref (priv->possible_formats);
      priv->possible_formats = g_value_dup_boxed (value);
      g_object_set (priv->iface, "possible-formats",
          g_bytes_get_data (priv->possible_formats, NULL), NULL);
      break;

    case PROP_FORMAT:
      if (priv->format)
        g_bytes_unref (priv->format);
      priv->format = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (output, prop_id, pspec);
      break;
  }
}

static gboolean
handle_start (PvSourceOutput1        *interface,
              GDBusMethodInvocation  *invocation,
              const gchar            *arg_requested_format,
              gpointer                user_data)
{
  PvSourceOutput *output = user_data;
  PvSourceOutputPrivate *priv = output->priv;
  GUnixFDList *fdlist;
  gint fd[2];

  priv->requested_format = g_bytes_new (arg_requested_format, strlen (arg_requested_format) + 1);

  socketpair (AF_UNIX, SOCK_STREAM, 0, fd);
  priv->socket = g_socket_new_from_fd (fd[0], NULL);
  g_object_notify (G_OBJECT (output), "socket");

  if (priv->format == NULL)
    goto no_format;

  fdlist = g_unix_fd_list_new ();
  g_unix_fd_list_append (fdlist, fd[1], NULL);

  g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
           g_variant_new ("(hs)",
                          0,
                          g_bytes_get_data (priv->format, NULL)),
           fdlist);

  return TRUE;

  /* error */
no_format:
  {
    g_dbus_method_invocation_return_dbus_error (invocation,
        "org.pulsevideo.Error", "No format");
    close (fd[0]);
    close (fd[1]);
    g_clear_pointer (&priv->requested_format, g_bytes_unref);
    g_clear_object (&priv->socket);
    return TRUE;
  }
}

static void
stop_transfer (PvSourceOutput *output)
{
  PvSourceOutputPrivate *priv = output->priv;

  if (priv->socket) {
    g_clear_object (&priv->socket);
    g_clear_pointer (&priv->requested_format, g_bytes_unref);
    g_clear_pointer (&priv->format, g_bytes_unref);
    g_object_notify (G_OBJECT (output), "socket");
  }
}

static gboolean
handle_stop (PvSourceOutput1        *interface,
             GDBusMethodInvocation  *invocation,
             gpointer                user_data)
{
  PvSourceOutput *output = user_data;

  stop_transfer (output);

  g_dbus_method_invocation_return_value (invocation, NULL);

  return TRUE;
}

static gboolean
handle_remove (PvSourceOutput1        *interface,
               GDBusMethodInvocation  *invocation,
               gpointer                user_data)
{
  PvSourceOutput *output = user_data;

  stop_transfer (output);

  g_signal_emit (output, signals[SIGNAL_REMOVE], 0, NULL);

  g_dbus_method_invocation_return_value (invocation, NULL);

  return TRUE;
}

static void
output_register_object (PvSourceOutput *output, const gchar *prefix)
{
  PvSourceOutputPrivate *priv = output->priv;
  PvObjectSkeleton *skel;
  gchar *name;

  name = g_strdup_printf ("%s/output", prefix);
  skel = pv_object_skeleton_new (name);
  g_free (name);

  pv_object_skeleton_set_source_output1 (skel, priv->iface);

  g_free (priv->object_path);
  priv->object_path = pv_daemon_export_uniquely (priv->daemon, G_DBUS_OBJECT_SKELETON (skel));
}

static void
output_unregister_object (PvSourceOutput *output)
{
  PvSourceOutputPrivate *priv = output->priv;

  stop_transfer (output);

  pv_daemon_unexport (priv->daemon, priv->object_path);
}

static void
pv_source_output_dispose (GObject * object)
{
  PvSourceOutput *output = PV_SOURCE_OUTPUT (object);

  output_unregister_object (output);

  G_OBJECT_CLASS (pv_source_output_parent_class)->dispose (object);
}

static void
pv_source_output_finalize (GObject * object)
{
  PvSourceOutput *output = PV_SOURCE_OUTPUT (object);
  PvSourceOutputPrivate *priv = output->priv;

  g_clear_object (&priv->daemon);
  g_clear_object (&priv->iface);
  g_free (priv->client_path);
  g_free (priv->object_path);
  g_free (priv->source_path);

  G_OBJECT_CLASS (pv_source_output_parent_class)->finalize (object);
}

static void
pv_source_output_constructed (GObject * object)
{
  PvSourceOutput *output = PV_SOURCE_OUTPUT (object);
  PvSourceOutputPrivate *priv = output->priv;

  output_register_object (output, priv->object_path);

  G_OBJECT_CLASS (pv_source_output_parent_class)->constructed (object);
}

static void
pv_source_output_class_init (PvSourceOutputClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PvSourceOutputPrivate));

  gobject_class->dispose = pv_source_output_dispose;
  gobject_class->finalize = pv_source_output_finalize;
  gobject_class->set_property = pv_source_output_set_property;
  gobject_class->get_property = pv_source_output_get_property;
  gobject_class->constructed = pv_source_output_constructed;

  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "The Daemon",
                                                        PV_TYPE_DAEMON,
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
                                   PROP_SOURCE_PATH,
                                   g_param_spec_string ("source-path",
                                                        "Source Path",
                                                        "The source object path",
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
pv_source_output_init (PvSourceOutput * output)
{
  PvSourceOutputPrivate *priv = output->priv = PV_SOURCE_OUTPUT_GET_PRIVATE (output);

  priv->iface = pv_source_output1_skeleton_new ();
  g_signal_connect (priv->iface, "handle-start", (GCallback) handle_start, output);
  g_signal_connect (priv->iface, "handle-stop", (GCallback) handle_stop, output);
  g_signal_connect (priv->iface, "handle-remove", (GCallback) handle_remove, output);
}

const gchar *
pv_source_output_get_object_path (PvSourceOutput *output)
{
  PvSourceOutputPrivate *priv;

  g_return_val_if_fail (PV_IS_SOURCE_OUTPUT (output), NULL);
  priv = output->priv;

  return priv->object_path;
}

