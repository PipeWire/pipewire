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

#include "client/pulsevideo.h"

#include "client/pv-context.h"
#include "client/pv-enumtypes.h"
#include "client/pv-subscribe.h"

#include "dbus/org-pulsevideo.h"

struct _PvContextPrivate
{
  gchar *name;
  GVariant *properties;

  guint id;
  GDBusConnection *connection;

  PvContextFlags flags;
  PvContextState state;

  PvDaemon1 *daemon;

  PvClient1 *client;

  PvSubscriptionFlags subscription_mask;
  PvSubscribe *subscribe;

  GList *sources;

  GDBusObjectManagerServer *server_manager;
};


#define PV_CONTEXT_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PV_TYPE_CONTEXT, PvContextPrivate))

G_DEFINE_TYPE (PvContext, pv_context, G_TYPE_OBJECT);

static void subscription_state (GObject *object, GParamSpec *pspec, gpointer user_data);
static void
subscription_cb (PvSubscribe         *subscribe,
                 PvSubscriptionEvent  event,
                 PvSubscriptionFlags  flags,
                 GDBusProxy          *object,
                 gpointer             user_data);

enum
{
  PROP_0,
  PROP_NAME,
  PROP_PROPERTIES,
  PROP_STATE,
  PROP_CONNECTION,
  PROP_SUBSCRIPTION_MASK,
};

enum
{
  SIGNAL_SUBSCRIPTION_EVENT,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
pv_context_get_property (GObject    *_object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  PvContext *context = PV_CONTEXT (_object);
  PvContextPrivate *priv = context->priv;

  switch (prop_id) {
    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    case PROP_PROPERTIES:
      g_value_set_variant (value, priv->properties);
      break;

    case PROP_STATE:
      g_value_set_enum (value, priv->state);
      break;

    case PROP_CONNECTION:
      g_value_set_object (value, priv->connection);
      break;

    case PROP_SUBSCRIPTION_MASK:
      g_value_set_flags (value, priv->subscription_mask);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (context, prop_id, pspec);
      break;
  }
}

static void
pv_context_set_property (GObject      *_object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  PvContext *context = PV_CONTEXT (_object);
  PvContextPrivate *priv = context->priv;

  switch (prop_id) {
    case PROP_NAME:
      g_free (priv->name);
      priv->name = g_value_dup_string (value);
      break;

    case PROP_PROPERTIES:
      if (priv->properties)
        g_variant_unref (priv->properties);
      priv->properties = g_value_dup_variant (value);
      break;

    case PROP_SUBSCRIPTION_MASK:
      priv->subscription_mask = g_value_get_flags (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (context, prop_id, pspec);
      break;
  }
}

static void
pv_context_finalize (GObject * object)
{
  PvContext *context = PV_CONTEXT (object);
  PvContextPrivate *priv = context->priv;

  g_object_unref (priv->server_manager);

  G_OBJECT_CLASS (pv_context_parent_class)->finalize (object);
}


static void
pv_context_class_init (PvContextClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PvContextPrivate));

  gobject_class->finalize = pv_context_finalize;
  gobject_class->set_property = pv_context_set_property;
  gobject_class->get_property = pv_context_get_property;

  /**
   * PvContext:name
   *
   * The application name of the context.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Name",
                                                        "The application name",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));
  /**
   * PvContext:properties
   *
   * Properties of the context.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_PROPERTIES,
                                   g_param_spec_variant ("properties",
                                                         "Properties",
                                                         "Extra properties",
                                                          G_VARIANT_TYPE_VARIANT,
                                                          NULL,
                                                          G_PARAM_READWRITE |
                                                          G_PARAM_STATIC_STRINGS));
  /**
   * PvContext:state
   *
   * The state of the context.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_STATE,
                                   g_param_spec_enum ("state",
                                                      "State",
                                                      "The context state",
                                                      PV_TYPE_CONTEXT_STATE,
                                                      PV_CONTEXT_STATE_UNCONNECTED,
                                                      G_PARAM_READABLE |
                                                      G_PARAM_STATIC_STRINGS));
  /**
   * PvContext:connection
   *
   * The connection of the context.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_CONNECTION,
                                   g_param_spec_object ("connection",
                                                        "Connection",
                                                        "The DBus connection",
                                                        G_TYPE_DBUS_CONNECTION,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));
  /**
   * PvContext:subscription-mask
   *
   * The subscription mask
   */
  g_object_class_install_property (gobject_class,
                                   PROP_SUBSCRIPTION_MASK,
                                   g_param_spec_flags ("subscription-mask",
                                                       "Subscription Mask",
                                                       "The object to receive subscription events of",
                                                       PV_TYPE_SUBSCRIPTION_FLAGS,
                                                       0,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_STRINGS));
  /**
   * PvContext:subscription-event
   * @subscribe: The #PvContext emitting the signal.
   * @event: A #PvSubscriptionEvent
   * @flags: #PvSubscriptionFlags indicating the object
   * @object: the GDBusProxy object
   *
   * Notify about a new object that was added/removed/modified.
   */
  signals[SIGNAL_SUBSCRIPTION_EVENT] = g_signal_new ("subscription-event",
                                                     G_TYPE_FROM_CLASS (klass),
                                                     G_SIGNAL_RUN_LAST,
                                                     0,
                                                     NULL,
                                                     NULL,
                                                     g_cclosure_marshal_generic,
                                                     G_TYPE_NONE,
                                                     3,
                                                     PV_TYPE_SUBSCRIPTION_EVENT,
                                                     PV_TYPE_SUBSCRIPTION_FLAGS,
                                                     G_TYPE_DBUS_PROXY);

}

static void
pv_context_init (PvContext * context)
{
  PvContextPrivate *priv = context->priv = PV_CONTEXT_GET_PRIVATE (context);

  priv->state = PV_CONTEXT_STATE_UNCONNECTED;
  priv->server_manager = g_dbus_object_manager_server_new (PV_DBUS_OBJECT_PREFIX);
  priv->subscribe = pv_subscribe_new ();
  g_object_set (priv->subscribe, "subscription-mask", PV_SUBSCRIPTION_FLAGS_ALL, NULL);
  g_signal_connect (priv->subscribe, "subscription-event", (GCallback) subscription_cb, context);
  g_signal_connect (priv->subscribe, "notify::state", (GCallback) subscription_state, context);
}

/**
 * pv_context_new:
 * @name: an application name
 * @properties: optional properties
 *
 * Make a new unconnected #PvContext
 *
 * Returns: a new unconnected #PvContext
 */
PvContext *
pv_context_new (const gchar *name, GVariant *properties)
{
  return g_object_new (PV_TYPE_CONTEXT, "name", name, "properties", properties, NULL);
}

static void
context_set_state (PvContext *context, PvContextState state)
{
  if (context->priv->state != state) {
    context->priv->state = state;
    g_object_notify (G_OBJECT (context), "state");
  }
}

static void
on_client_proxy (GObject *source_object,
                 GAsyncResult *res,
                 gpointer user_data)
{
  PvContext *context = user_data;
  PvContextPrivate *priv = context->priv;
  GError *error = NULL;

  priv->client = pv_client1_proxy_new_finish (res, &error);
  if (priv->client == NULL) {
    context_set_state (context, PV_CONTEXT_STATE_ERROR);
    g_error ("failed to get client proxy: %s", error->message);
    g_clear_error (&error);
    return;
  }
  context_set_state (context, PV_CONTEXT_STATE_READY);
}

static void
on_client_connected (GObject *source_object,
                     GAsyncResult *res,
                     gpointer user_data)
{
  PvContext *context = user_data;
  PvContextPrivate *priv = context->priv;
  GError *error = NULL;
  gchar *client_path;

  if (!pv_daemon1_call_connect_client_finish (priv->daemon, &client_path, res, &error)) {
    context_set_state (context, PV_CONTEXT_STATE_ERROR);
    g_error ("failed to connect client: %s", error->message);
    g_clear_error (&error);
    return;
  }

  pv_client1_proxy_new (priv->connection,
                        G_DBUS_PROXY_FLAGS_NONE,
                        PV_DBUS_SERVICE,
                        client_path,
                        NULL,
                        on_client_proxy,
                        context);
  g_free (client_path);
}

static void
on_daemon_connected (GObject *source_object,
                     GAsyncResult *res,
                     gpointer user_data)
{
  PvContext *context = user_data;
  PvContextPrivate *priv = context->priv;

  context_set_state (context, PV_CONTEXT_STATE_REGISTERING);

  {
    GVariantBuilder builder;

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&builder, "{sv}", "name", g_variant_new_string ("hello"));

    pv_daemon1_call_connect_client (priv->daemon,
                                    g_variant_builder_end (&builder), /* GVariant *arg_properties */
                                    NULL,        /* GCancellable *cancellable */
                                    on_client_connected,
                                    context);

  }
}

static void
subscription_cb (PvSubscribe         *subscribe,
                 PvSubscriptionEvent  event,
                 PvSubscriptionFlags  flags,
                 GDBusProxy          *object,
                 gpointer             user_data)
{
  PvContext *context = user_data;
  PvContextPrivate *priv = context->priv;

  g_print ("got event %d %d\n", event, flags);

  switch (flags) {
    case PV_SUBSCRIPTION_FLAGS_DAEMON:
      priv->daemon = PV_DAEMON1 (object);
      break;

    case PV_SUBSCRIPTION_FLAGS_CLIENT:
      break;

    case PV_SUBSCRIPTION_FLAGS_SOURCE:
      priv->sources = g_list_prepend (priv->sources, object);
      break;

    case PV_SUBSCRIPTION_FLAGS_SOURCE_OUTPUT:
      break;
  }

  if (flags & priv->subscription_mask)
    g_signal_emit (context,
                   signals[SIGNAL_SUBSCRIPTION_EVENT],
                   0,
                   event,
                   flags,
                   object);

}

static void
subscription_state (GObject    *object,
                    GParamSpec *pspec,
                    gpointer    user_data)
{
  PvContext *context = user_data;
  PvSubscriptionState state;

  g_object_get (object, "state", &state, NULL);
  g_print ("got subscription state %d\n", state);

  switch (state) {
    case PV_SUBSCRIPTION_STATE_READY:
      on_daemon_connected (NULL, NULL, context);
      break;

    default:
      break;
  }

}

static void
on_name_appeared (GDBusConnection *connection,
                  const gchar *name,
                  const gchar *name_owner,
                  gpointer user_data)
{
  PvContext *context = user_data;
  PvContextPrivate *priv = context->priv;

  priv->connection = connection;
  g_dbus_object_manager_server_set_connection (priv->server_manager, connection);

  g_object_set (priv->subscribe, "connection", priv->connection,
                                 "service", name, NULL);
}

static void
on_name_vanished (GDBusConnection *connection,
                  const gchar *name,
                  gpointer user_data)
{
  PvContext *context = user_data;
  PvContextPrivate *priv = context->priv;

  priv->connection = connection;
  g_dbus_object_manager_server_set_connection (priv->server_manager, connection);

  g_object_set (priv->subscribe, "connection", connection, NULL);

  if (priv->flags & PV_CONTEXT_FLAGS_NOFAIL) {
    context_set_state (context, PV_CONTEXT_STATE_CONNECTING);
  } else {
    context_set_state (context, PV_CONTEXT_STATE_ERROR);
  }
}

/**
 * pv_context_connect:
 * @context: a #PvContext
 * @flags: #PvContextFlags
 *
 * Connect to the daemon with @flags
 *
 * Returns: %TRUE on success.
 */
gboolean
pv_context_connect (PvContext *context, PvContextFlags flags)
{
  PvContextPrivate *priv;
  GBusNameWatcherFlags nw_flags;

  g_return_val_if_fail (PV_IS_CONTEXT (context), FALSE);

  priv = context->priv;
  g_return_val_if_fail (priv->connection == NULL, FALSE);

  priv->flags = flags;

  context_set_state (context, PV_CONTEXT_STATE_CONNECTING);

  nw_flags = G_BUS_NAME_WATCHER_FLAGS_NONE;
  if (!(flags & PV_CONTEXT_FLAGS_NOAUTOSPAWN))
    nw_flags = G_BUS_NAME_WATCHER_FLAGS_AUTO_START;

  priv->id = g_bus_watch_name (G_BUS_TYPE_SESSION,
                               PV_DBUS_SERVICE,
                               nw_flags,
                               on_name_appeared,
                               on_name_vanished,
                               context,
                               NULL);
  return TRUE;
}

static void
on_client_disconnected (GObject *source_object,
                        GAsyncResult *res,
                        gpointer user_data)
{
  PvContext *context = user_data;
  PvContextPrivate *priv = context->priv;
  GError *error = NULL;

  if (!pv_client1_call_disconnect_finish (priv->client, res, &error)) {
    context_set_state (context, PV_CONTEXT_STATE_ERROR);
    g_error ("failed to disconnect client: %s", error->message);
    g_clear_error (&error);
    return;
  }
  context_set_state (context, PV_CONTEXT_STATE_UNCONNECTED);
}

/**
 * pv_context_disconnect:
 * @context: a #PvContext
 *
 * Disonnect from the daemon.
 *
 * Returns: %TRUE on success.
 */
gboolean
pv_context_disconnect (PvContext *context)
{
  PvContextPrivate *priv;

  g_return_val_if_fail (PV_IS_CONTEXT (context), FALSE);

  priv = context->priv;
  g_return_val_if_fail (priv->client != NULL, FALSE);

  pv_client1_call_disconnect (priv->client,
                              NULL,        /* GCancellable *cancellable */
                              on_client_disconnected,
                              context);
  return TRUE;
}

/**
 * pv_context_register_source:
 * @context: a #PvContext
 * @source: a #PvSource
 *
 * Register @source in @context. This makes @source availabe to other
 * connected clients.
 *
 * Returns: %TRUE on success.
 */
gboolean
pv_context_register_source (PvContext *context, PvSource *source)
{
  PvContextPrivate *priv;

  g_return_val_if_fail (PV_IS_CONTEXT (context), FALSE);
  g_return_val_if_fail (PV_IS_SOURCE (source), FALSE);

  priv = context->priv;

  pv_source_set_manager (source, priv->server_manager);

  return TRUE;
}

/**
 * pv_context_unregister_source:
 * @context: a #PvContext
 * @source: a #PvSource
 *
 * Unregister @source from @context. @source will no longer be
 * available for clients.
 *
 * Returns: %TRUE on success.
 */
gboolean
pv_context_unregister_source (PvContext *context, PvSource *source)
{
  g_return_val_if_fail (PV_IS_CONTEXT (context), FALSE);
  g_return_val_if_fail (PV_IS_SOURCE (source), FALSE);

  pv_source_set_manager (source, NULL);

  return TRUE;
}

/**
 * pv_context_get_state:
 * @context: a #PvContext
 *
 * Get the state of @context.
 *
 * Returns: the state of @context
 */
PvContextState
pv_context_get_state (PvContext *context)
{
  PvContextPrivate *priv;

  g_return_val_if_fail (PV_IS_CONTEXT (context), PV_CONTEXT_STATE_ERROR);
  priv = context->priv;

  return priv->state;
}

/**
 * pv_context_get_connection:
 * @context: a #PvContext
 *
 * Get the #GDBusConnection of @context.
 *
 * Returns: the #GDBusConnection of @context or %NULL when not connected.
 */
GDBusConnection *
pv_context_get_connection (PvContext *context)
{
  g_return_val_if_fail (PV_IS_CONTEXT (context), NULL);

  return context->priv->connection;
}

GDBusProxy *
pv_context_find_source (PvContext *context, const gchar *name, GVariant *props)
{
  PvContextPrivate *priv;

  g_return_val_if_fail (PV_IS_CONTEXT (context), NULL);
  priv = context->priv;

  if (priv->sources == NULL)
    return NULL;

  return priv->sources->data;
}

