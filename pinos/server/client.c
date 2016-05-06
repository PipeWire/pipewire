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
#include "pinos/client/pinos.h"

#include "pinos/server/client.h"
#include "pinos/server/upload-node.h"

#include "pinos/dbus/org-pinos.h"

struct _PinosClientPrivate
{
  PinosDaemon *daemon;
  gchar *sender;
  gchar *object_path;
  PinosProperties *properties;

  PinosClient1 *client1;

  PinosFdManager *fdmanager;
  GList *channels;
};

#define PINOS_CLIENT_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_CLIENT, PinosClientPrivate))

G_DEFINE_TYPE (PinosClient, pinos_client, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_SENDER,
  PROP_OBJECT_PATH,
  PROP_PROPERTIES,
};

enum
{
  SIGNAL_DISCONNECT,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
pinos_client_get_property (GObject    *_object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  PinosClient *client = PINOS_CLIENT (_object);
  PinosClientPrivate *priv = client->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      g_value_set_object (value, priv->daemon);
      break;

    case PROP_SENDER:
      g_value_set_string (value, priv->sender);
      break;

    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;

    case PROP_PROPERTIES:
      g_value_set_boxed (value, priv->properties);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (client, prop_id, pspec);
      break;
  }
}

static void
pinos_client_set_property (GObject      *_object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  PinosClient *client = PINOS_CLIENT (_object);
  PinosClientPrivate *priv = client->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    case PROP_SENDER:
      priv->sender = g_value_dup_string (value);
      break;

    case PROP_OBJECT_PATH:
      priv->object_path = g_value_dup_string (value);
      break;

    case PROP_PROPERTIES:
      if (priv->properties)
        pinos_properties_free (priv->properties);
      priv->properties = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (client, prop_id, pspec);
      break;
  }
}

static void
handle_remove_channel (PinosChannel *channel,
                       gpointer      user_data)
{
  PinosClient *client = user_data;
  PinosClientPrivate *priv = client->priv;

  g_debug ("client %p: remove channel %p", client, channel);
  priv->channels = g_list_remove (priv->channels, channel);
  g_object_unref (channel);
}

static gboolean
handle_create_channel (PinosClient1           *interface,
                       GDBusMethodInvocation  *invocation,
                       PinosDirection          direction,
                       const gchar            *arg_port,
                       const gchar            *arg_possible_formats,
                       GVariant               *arg_properties,
                       gpointer                user_data)
{
  PinosClient *client = user_data;
  PinosClientPrivate *priv = client->priv;
  PinosPort *port;
  PinosChannel *channel;
  const gchar *object_path, *sender;
  GBytes *formats;
  PinosProperties *props;
  GError *error = NULL;

  sender = g_dbus_method_invocation_get_sender (invocation);
  if (g_strcmp0 (pinos_client_get_sender (client), sender) != 0)
    goto not_allowed;

  formats = g_bytes_new (arg_possible_formats, strlen (arg_possible_formats) + 1);
  props = pinos_properties_from_variant (arg_properties);

  port = pinos_daemon_find_port (priv->daemon,
                                 direction,
                                 arg_port,
                                 props,
                                 formats,
                                 &error);
  if (port == NULL)
    goto no_port;

  g_debug ("client %p: matched port %s", client, pinos_port_get_object_path (port));

  channel = pinos_port_create_channel (port,
                                       priv->object_path,
                                       formats,
                                       props,
                                       &error);
  pinos_properties_free (props);
  g_bytes_unref (formats);

  if (channel == NULL)
    goto no_channel;

  priv->channels = g_list_prepend (priv->channels, channel);

  g_signal_connect (channel,
                    "remove",
                    (GCallback) handle_remove_channel,
                    client);

  object_path = pinos_channel_get_object_path (channel);
  g_debug ("client %p: add source channel %p, %s", client, channel, object_path);
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(o)", object_path));

  return TRUE;

  /* ERRORS */
not_allowed:
  {
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "not client owner");
    return TRUE;
  }
no_port:
  {
    g_debug ("client %p: could not find port %s, %s", client, arg_port, error->message);
    g_dbus_method_invocation_return_gerror (invocation, error);
    pinos_properties_free (props);
    g_bytes_unref (formats);
    g_clear_error (&error);
    return TRUE;
  }
no_channel:
  {
    g_debug ("client %p: could not create channel %s", client, error->message);
    g_dbus_method_invocation_return_gerror (invocation, error);
    g_clear_error (&error);
    return TRUE;
  }
}

static gboolean
handle_create_upload_channel (PinosClient1           *interface,
                              GDBusMethodInvocation  *invocation,
                              const gchar            *arg_possible_formats,
                              GVariant               *arg_properties,
                              gpointer                user_data)
{
  PinosClient *client = user_data;
  PinosClientPrivate *priv = client->priv;
  PinosNode *node;
  PinosChannel *channel;
  const gchar *channel_path, *sender;
  GBytes *formats;
  GError *error = NULL;
  PinosProperties *props;

  sender = g_dbus_method_invocation_get_sender (invocation);
  if (g_strcmp0 (pinos_client_get_sender (client), sender) != 0)
    goto not_allowed;

  formats = g_bytes_new (arg_possible_formats, strlen (arg_possible_formats) + 1);

  node = pinos_upload_node_new (priv->daemon, formats);
  if (node == NULL)
    goto no_node;

  sender = g_dbus_method_invocation_get_sender (invocation);
  props = pinos_properties_from_variant (arg_properties);

  channel = pinos_upload_node_get_channel (PINOS_UPLOAD_NODE (node),
                                           priv->object_path,
                                           formats,
                                           props,
                                           &error);
  pinos_properties_free (props);

  if (channel == NULL)
    goto no_channel;

  g_object_set_data_full (G_OBJECT (channel),
                          "channel-owner",
                          node,
                          g_object_unref);

  channel_path = pinos_channel_get_object_path (channel);
  g_debug ("client %p: add source channel %p, %s", client, channel, channel_path);
  priv->channels = g_list_prepend (priv->channels, channel);

  g_signal_connect (channel,
                    "remove",
                    (GCallback) handle_remove_channel,
                    client);

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(o)",
                                         channel_path));

  return TRUE;

  /* ERRORS */
not_allowed:
  {
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "not client owner");
    return TRUE;
  }
no_node:
  {
    g_debug ("client %p: could not create upload node", client);
    g_dbus_method_invocation_return_dbus_error (invocation,
        "org.pinos.Error", "Can't create upload node");
    g_bytes_unref (formats);
    return TRUE;
  }
no_channel:
  {
    g_debug ("client %p: could not create upload channel %s", client, error->message);
    g_dbus_method_invocation_return_gerror (invocation, error);
    g_object_unref (node);
    g_clear_error (&error);
    g_bytes_unref (formats);
    return TRUE;
  }
}
static gboolean
handle_disconnect (PinosClient1           *interface,
                   GDBusMethodInvocation  *invocation,
                   gpointer                user_data)
{
  PinosClient *client = user_data;

  g_debug ("client %p: disconnect", client);
  g_signal_emit (client, signals[SIGNAL_DISCONNECT], 0, NULL);

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("()"));
  return TRUE;
}

static void
client_register_object (PinosClient *client,
                        const gchar *prefix)
{
  PinosClientPrivate *priv = client->priv;
  PinosDaemon *daemon = priv->daemon;
  PinosObjectSkeleton *skel;
  gchar *name;

  name = g_strdup_printf ("%s/client", prefix);
  skel = pinos_object_skeleton_new (name);
  g_free (name);

  priv->client1 = pinos_client1_skeleton_new ();
  pinos_client1_set_name (priv->client1, priv->sender);
  pinos_client1_set_properties (priv->client1, pinos_properties_to_variant (priv->properties));
  g_signal_connect (priv->client1, "handle-create-channel",
                                   (GCallback) handle_create_channel,
                                   client);
  g_signal_connect (priv->client1, "handle-create-upload-channel",
                                   (GCallback) handle_create_upload_channel,
                                   client);
  g_signal_connect (priv->client1, "handle-disconnect",
                                   (GCallback) handle_disconnect,
                                   client);
  pinos_object_skeleton_set_client1 (skel, priv->client1);

  g_free (priv->object_path);
  priv->object_path = pinos_daemon_export_uniquely (daemon, G_DBUS_OBJECT_SKELETON (skel));
  g_debug ("client %p: register %s", client, priv->object_path);
}

static void
client_unregister_object (PinosClient *client)
{
  PinosClientPrivate *priv = client->priv;
  PinosDaemon *daemon = priv->daemon;

  g_debug ("client %p: unregister", client);
  g_clear_object (&priv->client1);

  pinos_daemon_unexport (daemon, priv->object_path);
  g_clear_pointer (&priv->object_path, g_free);
}

static void
do_remove_channel (PinosChannel *channel,
                   PinosClient  *client)
{
  g_debug ("client %p: remove channel %p", client, channel);
  pinos_channel_remove (channel);
}

static void
pinos_client_dispose (GObject * object)
{
  PinosClient *client = PINOS_CLIENT (object);
  PinosClientPrivate *priv = client->priv;

  g_debug ("client %p: dispose", client);
  if (priv->object_path)
    pinos_fd_manager_remove_all (priv->fdmanager, priv->object_path);

  g_list_foreach (priv->channels, (GFunc) do_remove_channel, client);
  client_unregister_object (client);
  g_clear_object (&priv->daemon);

  G_OBJECT_CLASS (pinos_client_parent_class)->dispose (object);
}

static void
pinos_client_finalize (GObject * object)
{
  PinosClient *client = PINOS_CLIENT (object);
  PinosClientPrivate *priv = client->priv;

  g_debug ("client %p: finalize", client);
  g_free (priv->sender);
  if (priv->properties)
    pinos_properties_free (priv->properties);
  g_clear_object (&priv->fdmanager);

  G_OBJECT_CLASS (pinos_client_parent_class)->finalize (object);
}

static void
pinos_client_constructed (GObject * object)
{
  PinosClient *client = PINOS_CLIENT (object);
  PinosClientPrivate *priv = client->priv;

  g_debug ("client %p: constructed", client);

  client_register_object (client, priv->object_path);

  G_OBJECT_CLASS (pinos_client_parent_class)->constructed (object);
}

static void
pinos_client_class_init (PinosClientClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosClientPrivate));

  gobject_class->constructed = pinos_client_constructed;
  gobject_class->dispose = pinos_client_dispose;
  gobject_class->finalize = pinos_client_finalize;
  gobject_class->set_property = pinos_client_set_property;
  gobject_class->get_property = pinos_client_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "The daemon",
                                                        PINOS_TYPE_DAEMON,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_SENDER,
                                   g_param_spec_string ("sender",
                                                        "Sender",
                                                        "The sender",
                                                        NULL,
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
                                   PROP_PROPERTIES,
                                   g_param_spec_boxed ("properties",
                                                       "Properties",
                                                       "Client properties",
                                                       PINOS_TYPE_PROPERTIES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));

  signals[SIGNAL_DISCONNECT] = g_signal_new ("disconnect",
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
pinos_client_init (PinosClient * client)
{
  PinosClientPrivate *priv = client->priv = PINOS_CLIENT_GET_PRIVATE (client);

  g_debug ("client %p: new", client);
  priv->fdmanager = pinos_fd_manager_get (PINOS_FD_MANAGER_DEFAULT);
}


/**
 * pinos_client_new:
 * @daemon: a #PinosDaemon
 * @sender: the sender id
 * @prefix: a prefix
 * @properties: extra client properties
 *
 * Make a new #PinosClient object and register it to @daemon under the @prefix.
 *
 * Returns: a new #PinosClient
 */
PinosClient *
pinos_client_new (PinosDaemon     *daemon,
                  const gchar     *sender,
                  const gchar     *prefix,
                  PinosProperties *properties)
{
  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);
  g_return_val_if_fail (g_variant_is_object_path (prefix), NULL);

  return g_object_new (PINOS_TYPE_CLIENT, "daemon", daemon,
                                          "sender", sender,
                                          "object-path", prefix,
                                          "properties", properties,
                                          NULL);
}

/**
 * pinos_client_get_sender:
 * @client: a #PinosClient
 *
 * Get the sender of @client.
 *
 * Returns: the sender of @client
 */
const gchar *
pinos_client_get_sender (PinosClient *client)
{
  PinosClientPrivate *priv;

  g_return_val_if_fail (PINOS_IS_CLIENT (client), NULL);
  priv = client->priv;

  return priv->sender;
}

/**
 * pinos_client_get_object_path:
 * @client: a #PinosClient
 *
 * Get the object path of @client.
 *
 * Returns: the object path of @client
 */
const gchar *
pinos_client_get_object_path (PinosClient *client)
{
  PinosClientPrivate *priv;

  g_return_val_if_fail (PINOS_IS_CLIENT (client), NULL);
  priv = client->priv;

  return priv->object_path;
}
