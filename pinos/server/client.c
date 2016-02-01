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

#include "pinos/client/enumtypes.h"

#include "pinos/server/client.h"
#include "pinos/server/client-source.h"

#include "pinos/dbus/org-pinos.h"

struct _PinosClientPrivate
{
  PinosDaemon *daemon;
  gchar *sender;
  gchar *object_path;
  PinosProperties *properties;

  PinosClient1 *client1;

  GList *outputs;
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
handle_remove_source_output (PinosSourceOutput *output,
                             gpointer           user_data)
{
  PinosClient *client = user_data;
  PinosClientPrivate *priv = client->priv;

  priv->outputs = g_list_remove (priv->outputs, output);
  g_object_unref (output);
}

static gboolean
handle_create_source_output (PinosClient1           *interface,
                             GDBusMethodInvocation  *invocation,
                             const gchar            *arg_source,
                             const gchar            *arg_accepted_formats,
                             GVariant               *arg_properties,
                             gpointer                user_data)
{
  PinosClient *client = user_data;
  PinosClientPrivate *priv = client->priv;
  PinosSource *source;
  PinosSourceOutput *output;
  const gchar *object_path, *sender;
  GBytes *formats;
  PinosProperties *props;
  GError *error = NULL;

  sender = g_dbus_method_invocation_get_sender (invocation);
  if (g_strcmp0 (pinos_client_get_sender (client), sender) != 0)
    goto not_allowed;

  formats = g_bytes_new (arg_accepted_formats, strlen (arg_accepted_formats) + 1);
  props = pinos_properties_from_variant (arg_properties);

  source = pinos_daemon_find_source (priv->daemon,
                                     arg_source,
                                     props,
                                     formats,
                                     &error);
  if (source == NULL)
    goto no_source;

  output = pinos_source_create_source_output (source,
                                              priv->object_path,
                                              formats,
                                              props,
                                              priv->object_path,
                                              &error);
  if (output == NULL)
    goto no_output;

  priv->outputs = g_list_prepend (priv->outputs, output);

  g_signal_connect (output,
                    "remove",
                    (GCallback) handle_remove_source_output,
                    client);

  object_path = pinos_source_output_get_object_path (output);
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
handle_create_source_input (PinosClient1           *interface,
                            GDBusMethodInvocation  *invocation,
                            const gchar            *arg_possible_formats,
                            GVariant               *arg_properties,
                            gpointer                user_data)
{
  PinosClient *client = user_data;
  PinosClientPrivate *priv = client->priv;
  PinosSource *source;
  PinosSourceOutput *input;
  const gchar *source_input_path, *sender;
  GBytes *formats;
  GError *error = NULL;
  PinosProperties *props;

  sender = g_dbus_method_invocation_get_sender (invocation);
  if (g_strcmp0 (pinos_client_get_sender (client), sender) != 0)
    goto not_allowed;

  formats = g_bytes_new (arg_possible_formats, strlen (arg_possible_formats) + 1);
  props = pinos_properties_from_variant (arg_properties);

  source = pinos_client_source_new (priv->daemon, formats);
  if (source == NULL)
    goto no_source;

  g_object_set_data_full (G_OBJECT (client),
                          pinos_source_get_object_path (PINOS_SOURCE (source)),
                          source,
                          g_object_unref);

  sender = g_dbus_method_invocation_get_sender (invocation);

  input = pinos_client_source_get_source_input (PINOS_CLIENT_SOURCE (source),
                                                priv->object_path,
                                                formats,
                                                props,
                                                priv->object_path,
                                                &error);
  if (input == NULL)
    goto no_input;

  source_input_path = pinos_source_output_get_object_path (input);

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
                 "org.pinos.Error", "not client owner");
    return TRUE;
  }
no_source:
  {
    g_dbus_method_invocation_return_dbus_error (invocation,
        "org.pinos.Error", "Can't create source");
    g_bytes_unref (formats);
    pinos_properties_free (props);
    return TRUE;
  }
no_input:
  {
    g_dbus_method_invocation_return_gerror (invocation, error);
    g_clear_error (&error);
    g_bytes_unref (formats);
    pinos_properties_free (props);
    return TRUE;
  }
}
static gboolean
handle_disconnect (PinosClient1           *interface,
                   GDBusMethodInvocation  *invocation,
                   gpointer                user_data)
{
  PinosClient *client = user_data;

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
  g_signal_connect (priv->client1, "handle-create-source-output",
                                   (GCallback) handle_create_source_output,
                                   client);
  g_signal_connect (priv->client1, "handle-create-source-input",
                                   (GCallback) handle_create_source_input,
                                   client);
  g_signal_connect (priv->client1, "handle-disconnect",
                                   (GCallback) handle_disconnect,
                                   client);
  pinos_object_skeleton_set_client1 (skel, priv->client1);

  g_free (priv->object_path);
  priv->object_path = pinos_daemon_export_uniquely (daemon, G_DBUS_OBJECT_SKELETON (skel));
}

static void
client_unregister_object (PinosClient *client)
{
  PinosClientPrivate *priv = client->priv;
  PinosDaemon *daemon = priv->daemon;

  g_clear_object (&priv->client1);

  pinos_daemon_unexport (daemon, priv->object_path);
  g_free (priv->object_path);
}

static void
do_remove_output (PinosSourceOutput *output,
                  PinosClient       *client)
{
  pinos_source_output_remove (output);
}

static void
pinos_client_dispose (GObject * object)
{
  PinosClient *client = PINOS_CLIENT (object);
  PinosClientPrivate *priv = client->priv;

  g_list_foreach (priv->outputs, (GFunc) do_remove_output, client);
  client_unregister_object (client);

  G_OBJECT_CLASS (pinos_client_parent_class)->dispose (object);
}
static void
pinos_client_finalize (GObject * object)
{
  PinosClient *client = PINOS_CLIENT (object);
  PinosClientPrivate *priv = client->priv;

  if (priv->properties)
    pinos_properties_free (priv->properties);

  G_OBJECT_CLASS (pinos_client_parent_class)->finalize (object);
}

static void
pinos_client_constructed (GObject * object)
{
  PinosClient *client = PINOS_CLIENT (object);
  PinosClientPrivate *priv = client->priv;

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
  client->priv = PINOS_CLIENT_GET_PRIVATE (client);
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
