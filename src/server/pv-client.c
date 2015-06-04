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
#include "client/pulsevideo.h"

#include "client/pv-enumtypes.h"

#include "server/pv-client.h"
#include "server/pv-client-source.h"

#include "dbus/org-pulsevideo.h"

struct _PvClientPrivate
{
  PvDaemon *daemon;
  gchar *sender;
  gchar *object_path;
  GVariant *properties;

  PvClient1 *client1;

  GList *outputs;
};

#define PV_CLIENT_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PV_TYPE_CLIENT, PvClientPrivate))

G_DEFINE_TYPE (PvClient, pv_client, G_TYPE_OBJECT);

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
pv_client_get_property (GObject    *_object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  PvClient *client = PV_CLIENT (_object);
  PvClientPrivate *priv = client->priv;

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
      g_value_set_variant (value, priv->properties);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (client, prop_id, pspec);
      break;
  }
}

static void
pv_client_set_property (GObject      *_object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  PvClient *client = PV_CLIENT (_object);
  PvClientPrivate *priv = client->priv;

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
        g_variant_unref (priv->properties);
      priv->properties = g_value_dup_variant (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (client, prop_id, pspec);
      break;
  }
}

static void
handle_remove_source_output (PvSourceOutput *output,
                             gpointer        user_data)
{
  PvClient *client = user_data;
  PvClientPrivate *priv = client->priv;

  priv->outputs = g_list_remove (priv->outputs, output);
  g_object_unref (output);
}

static gboolean
handle_create_source_output (PvClient1              *interface,
                             GDBusMethodInvocation  *invocation,
                             const gchar            *arg_source,
                             const gchar            *arg_accepted_formats,
                             gpointer                user_data)
{
  PvClient *client = user_data;
  PvClientPrivate *priv = client->priv;
  PvSource *source;
  PvSourceOutput *output;
  const gchar *object_path, *sender;
  GBytes *formats;
  GError *error = NULL;

  sender = g_dbus_method_invocation_get_sender (invocation);
  if (g_strcmp0 (pv_client_get_sender (client), sender) != 0)
    goto not_allowed;

  formats = g_bytes_new (arg_accepted_formats, strlen (arg_accepted_formats) + 1);

  source = pv_daemon_find_source (priv->daemon,
                                  arg_source,
                                  priv->properties,
                                  formats,
                                  &error);
  if (source == NULL)
    goto no_source;

  output = pv_source_create_source_output (source,
                                           priv->object_path,
                                           formats,
                                           priv->object_path,
                                           &error);
  if (output == NULL)
    goto no_output;

  object_path = pv_source_output_get_object_path (output);

  priv->outputs = g_list_prepend (priv->outputs, output);

  g_signal_connect (output,
                    "remove",
                    (GCallback) handle_remove_source_output,
                    client);

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(o)", object_path));

  return TRUE;

  /* ERRORS */
not_allowed:
  {
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pulsevideo.Error", "not client owner");
    return TRUE;
  }
no_source:
  {
    g_dbus_method_invocation_return_gerror (invocation, error);
    g_clear_error (&error);
    g_bytes_unref (formats);
    return TRUE;
  }
no_output:
  {
    g_dbus_method_invocation_return_gerror (invocation, error);
    g_clear_error (&error);
    g_bytes_unref (formats);
    return TRUE;
  }
}

static gboolean
handle_create_source_input (PvClient1              *interface,
                            GDBusMethodInvocation  *invocation,
                            const gchar            *arg_possible_formats,
                            gpointer                user_data)
{
  PvClient *client = user_data;
  PvClientPrivate *priv = client->priv;
  PvSource *source;
  PvSourceOutput *input;
  const gchar *source_input_path, *sender;
  GBytes *formats;
  GError *error = NULL;

  sender = g_dbus_method_invocation_get_sender (invocation);
  if (g_strcmp0 (pv_client_get_sender (client), sender) != 0)
    goto not_allowed;

  source = pv_client_source_new (priv->daemon);
  if (source == NULL)
    goto no_source;

  g_object_set_data_full (G_OBJECT (client),
                          pv_source_get_object_path (PV_SOURCE (source)),
                          source,
                          g_object_unref);

  sender = g_dbus_method_invocation_get_sender (invocation);

  formats = g_bytes_new (arg_possible_formats, strlen (arg_possible_formats) + 1);

  input = pv_client_source_get_source_input (PV_CLIENT_SOURCE (source),
                                             priv->object_path,
                                             formats,
                                             priv->object_path,
                                             &error);
  if (input == NULL)
    goto no_input;

  source_input_path = pv_source_output_get_object_path (input);

  priv->outputs = g_list_prepend (priv->outputs, input);

  g_signal_connect (input,
                    "remove",
                    (GCallback) handle_remove_source_output,
                    client);

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(o)",
                                         source_input_path));

  return TRUE;

  /* ERRORS */
not_allowed:
  {
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pulsevideo.Error", "not client owner");
    return TRUE;
  }
no_source:
  {
    g_dbus_method_invocation_return_dbus_error (invocation,
        "org.pulsevideo.Error", "Can't create source");
    return TRUE;
  }
no_input:
  {
    g_dbus_method_invocation_return_gerror (invocation, error);
    g_clear_error (&error);
    g_bytes_unref (formats);
    return TRUE;
  }
}
static gboolean
handle_disconnect (PvClient1              *interface,
                   GDBusMethodInvocation  *invocation,
                   gpointer                user_data)
{
  PvClient *client = user_data;

  g_signal_emit (client, signals[SIGNAL_DISCONNECT], 0, NULL);

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("()"));
  return TRUE;
}

static void
client_register_object (PvClient *client, const gchar *prefix)
{
  PvClientPrivate *priv = client->priv;
  PvDaemon *daemon = priv->daemon;
  PvObjectSkeleton *skel;
  gchar *name;

  name = g_strdup_printf ("%s/client", prefix);
  skel = pv_object_skeleton_new (name);
  g_free (name);

  priv->client1 = pv_client1_skeleton_new ();
  pv_client1_set_name (priv->client1, priv->sender);
  g_signal_connect (priv->client1, "handle-create-source-output",
                                   (GCallback) handle_create_source_output,
                                   client);
  g_signal_connect (priv->client1, "handle-create-source-input",
                                   (GCallback) handle_create_source_input,
                                   client);
  g_signal_connect (priv->client1, "handle-disconnect",
                                   (GCallback) handle_disconnect,
                                   client);
  pv_object_skeleton_set_client1 (skel, priv->client1);

  g_free (priv->object_path);
  priv->object_path = pv_daemon_export_uniquely (daemon, G_DBUS_OBJECT_SKELETON (skel));
}

static void
client_unregister_object (PvClient *client)
{
  PvClientPrivate *priv = client->priv;
  PvDaemon *daemon = priv->daemon;

  g_clear_object (&priv->client1);

  pv_daemon_unexport (daemon, priv->object_path);
  g_free (priv->object_path);
}

static void
do_remove_output (PvSourceOutput *output, PvClient *client)
{
  pv_source_output_remove (output);
}

static void
pv_client_dispose (GObject * object)
{
  PvClient *client = PV_CLIENT (object);
  PvClientPrivate *priv = client->priv;

  g_list_foreach (priv->outputs, (GFunc) do_remove_output, client);
  client_unregister_object (client);

  G_OBJECT_CLASS (pv_client_parent_class)->dispose (object);
}
static void
pv_client_finalize (GObject * object)
{
  PvClient *client = PV_CLIENT (object);
  PvClientPrivate *priv = client->priv;

  if (priv->properties)
    g_variant_unref (priv->properties);

  G_OBJECT_CLASS (pv_client_parent_class)->finalize (object);
}

static void
pv_client_constructed (GObject * object)
{
  PvClient *client = PV_CLIENT (object);
  PvClientPrivate *priv = client->priv;

  client_register_object (client, priv->object_path);

  G_OBJECT_CLASS (pv_client_parent_class)->constructed (object);
}

static void
pv_client_class_init (PvClientClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PvClientPrivate));

  gobject_class->constructed = pv_client_constructed;
  gobject_class->dispose = pv_client_dispose;
  gobject_class->finalize = pv_client_finalize;
  gobject_class->set_property = pv_client_set_property;
  gobject_class->get_property = pv_client_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "The daemon",
                                                        PV_TYPE_DAEMON,
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
                                   g_param_spec_variant ("properties",
                                                         "Properties",
                                                         "Client properties",
                                                         G_VARIANT_TYPE_DICTIONARY,
                                                         NULL,
                                                         G_PARAM_READWRITE |
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
pv_client_init (PvClient * client)
{
  client->priv = PV_CLIENT_GET_PRIVATE (client);
}


/**
 * pv_client_new:
 * @daemon: a #PvDaemon
 * @prefix: a prefix
 *
 * Make a new #PvClient object and register it to @daemon under the @prefix.
 *
 * Returns: a new #PvClient
 */
PvClient *
pv_client_new (PvDaemon    *daemon,
               const gchar *sender,
               const gchar *prefix,
               GVariant    *properties)
{
  g_return_val_if_fail (PV_IS_DAEMON (daemon), NULL);
  g_return_val_if_fail (g_variant_is_object_path (prefix), NULL);

  return g_object_new (PV_TYPE_CLIENT, "daemon", daemon,
                                       "sender", sender,
                                       "object-path", prefix,
                                       "properties", properties,
                                       NULL);
}

/**
 * pv_client_get_sender:
 * @client: a #PvClient
 *
 * Get the sender of @client.
 *
 * Returns: the sender of @client
 */
const gchar *
pv_client_get_sender (PvClient *client)
{
  PvClientPrivate *priv;

  g_return_val_if_fail (PV_IS_CLIENT (client), NULL);
  priv = client->priv;

  return priv->sender;
}

/**
 * pv_client_get_object_path:
 * @client: a #PvClient
 *
 * Get the object path of @client.
 *
 * Returns: the object path of @client
 */
const gchar *
pv_client_get_object_path (PvClient *client)
{
  PvClientPrivate *priv;

  g_return_val_if_fail (PV_IS_CLIENT (client), NULL);
  priv = client->priv;

  return priv->object_path;
}

