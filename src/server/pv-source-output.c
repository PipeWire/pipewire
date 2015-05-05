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

#include <sys/socket.h>

#include <gio/gunixfdlist.h>

#include "client/pv-enumtypes.h"

#include "server/pv-daemon.h"
#include "server/pv-source-output.h"

#include "dbus/org-pulsevideo.h"

struct _PvSourceOutputPrivate
{
  PvDaemon *daemon;

  gchar *object_path;
  gchar *source;

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
  PROP_SOURCE,
  PROP_SOCKET,
};

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

    case PROP_SOURCE:
      g_value_set_string (value, priv->source);
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

    case PROP_SOURCE:
      priv->source = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (output, prop_id, pspec);
      break;
  }
}

static gboolean
handle_start (PvSourceOutput1        *interface,
              GDBusMethodInvocation  *invocation,
              GVariant               *arg_properties,
              gpointer                user_data)
{
  PvSourceOutput *output = user_data;
  PvSourceOutputPrivate *priv = output->priv;
  GUnixFDList *fdlist;
  GVariantBuilder props;
  gint fd[2];

  socketpair (AF_UNIX, SOCK_STREAM, 0, fd);
  priv->socket = g_socket_new_from_fd (fd[0], NULL);
  g_object_notify (G_OBJECT (output), "socket");

  g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&props, "{sv}", "name", g_variant_new_string ("hello"));

  fdlist = g_unix_fd_list_new ();
  g_unix_fd_list_append (fdlist, fd[1], NULL);

  g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
           g_variant_new ("(h@a{sv})",
                          0,
                          g_variant_builder_end (&props)),
           fdlist);

  return TRUE;
}

static void
stop_transfer (PvSourceOutput *output)
{
  PvSourceOutputPrivate *priv = output->priv;

  if (priv->socket) {
    g_clear_object (&priv->socket);
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

  {
    PvSourceOutput1 *iface;

    iface = pv_source_output1_skeleton_new ();
    g_object_set (iface, "source", priv->source, NULL);
    g_signal_connect (iface, "handle-start", (GCallback) handle_start, output);
    g_signal_connect (iface, "handle-stop", (GCallback) handle_stop, output);
    g_signal_connect (iface, "handle-remove", (GCallback) handle_remove, output);
    pv_object_skeleton_set_source_output1 (skel, iface);
    g_object_unref (iface);
  }

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
pv_source_output_finalize (GObject * object)
{
  PvSourceOutput *output = PV_SOURCE_OUTPUT (object);
  PvSourceOutputPrivate *priv = output->priv;

  output_unregister_object (output);

  g_object_unref (priv->daemon);
  g_free (priv->object_path);
  g_free (priv->source);

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
                                   PROP_SOURCE,
                                   g_param_spec_string ("source",
                                                        "Source",
                                                        "The source object path",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_SOCKET,
                                   g_param_spec_object ("socket",
                                                        "Socket",
                                                        "The socket with data",
                                                        G_TYPE_SOCKET,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));
}

static void
pv_source_output_init (PvSourceOutput * output)
{
  output->priv = PV_SOURCE_OUTPUT_GET_PRIVATE (output);
}

const gchar *
pv_source_output_get_object_path (PvSourceOutput *output)
{
  PvSourceOutputPrivate *priv;

  g_return_val_if_fail (PV_IS_SOURCE_OUTPUT (output), NULL);
  priv = output->priv;

  return priv->object_path;
}

