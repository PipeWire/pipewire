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

#include <gio/gio.h>

#include "client/pulsevideo.h"
#include "client/pv-enumtypes.h"

#include "dbus/org-pulsevideo.h"

struct _PvSubscribePrivate
{
  PvSubscriptionState state;
  GDBusConnection *connection;
  gchar *service;
  GCancellable *cancellable;

  PvSubscriptionFlags subscription_mask;

  GDBusObjectManager *client_manager;
  guint pending_subscribes;

  GHashTable *senders;

  GError *error;
};


#define PV_SUBSCRIBE_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PV_TYPE_SUBSCRIBE, PvSubscribePrivate))

G_DEFINE_TYPE (PvSubscribe, pv_subscribe, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_CONNECTION,
  PROP_SERVICE,
  PROP_SUBSCRIPTION_MASK,
  PROP_STATE,
};

enum
{
  SIGNAL_SUBSCRIPTION_EVENT,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };


typedef struct {
  PvSubscribe *subscribe;
  gchar *sender;
  guint id;
  PvSubscribe *sender_subscribe;
  GList *clients;
  gulong signal_event;
  gulong signal_state;
} SenderData;

static void
notify_subscription (PvSubscribe         *subscribe,
                     GDBusObject         *object,
                     GDBusInterface      *interface,
                     PvSubscriptionEvent  event);

static void
on_sender_subscription_event (PvSubscribe         *sender_subscribe,
                              PvSubscriptionEvent  event,
                              PvSubscriptionFlags  flags,
                              GDBusProxy          *object,
                              gpointer             user_data)
{
  SenderData *data = user_data;
  PvSubscribe *subscribe = data->subscribe;

  g_signal_emit (subscribe,
                 signals[SIGNAL_SUBSCRIPTION_EVENT],
                 0,
                 event,
                 flags,
                 object);
}

static void
subscription_set_state (PvSubscribe *subscribe, PvSubscriptionState state)
{
  PvSubscribePrivate *priv = subscribe->priv;

  if (state != priv->state) {
    priv->state = state;
    g_object_notify (G_OBJECT (subscribe), "state");
  }
}

static void
on_sender_subscription_state (GObject    *object,
                              GParamSpec *pspec,
                              gpointer    user_data)
{
  SenderData *data = user_data;
  PvSubscribe *subscribe = data->subscribe;
  PvSubscribePrivate *priv = subscribe->priv;
  PvSubscriptionState state;

  g_object_get (object, "state", &state, NULL);

  switch (state) {
    case PV_SUBSCRIPTION_STATE_READY:
      if (--priv->pending_subscribes == 0)
        subscription_set_state (subscribe, state);
      break;

    case PV_SUBSCRIPTION_STATE_ERROR:
      subscription_set_state (subscribe, state);
      break;

    default:
      break;
  }
}

static void
client_name_appeared_handler (GDBusConnection *connection,
                              const gchar *name,
                              const gchar *name_owner,
                              gpointer user_data)
{
  SenderData *data = user_data;

  g_print ("appeared client %s %p\n", name, data);
  /* subscribe to Source events. We want to be notified when this new
   * sender add/change/remove sources and outputs */
  data->sender_subscribe = pv_subscribe_new ();
  g_object_set (data->sender_subscribe, "service", data->sender,
                                        "subscription-mask", PV_SUBSCRIPTION_FLAGS_ALL,
                                        "connection", connection,
                                        NULL);

  data->signal_event = g_signal_connect (data->sender_subscribe,
                    "subscription-event",
                    (GCallback) on_sender_subscription_event,
                    data);
  data->signal_state = g_signal_connect (data->sender_subscribe,
                    "notify::state",
                    (GCallback) on_sender_subscription_state,
                    data);
}

static void
remove_client (PvClient1 *client, SenderData *data)
{
  g_signal_emit (data->subscribe,
                 signals[SIGNAL_SUBSCRIPTION_EVENT],
                 0,
                 PV_SUBSCRIPTION_EVENT_REMOVE,
                 PV_SUBSCRIPTION_FLAGS_CLIENT,
                 client);
}

static void
client_name_vanished_handler (GDBusConnection *connection,
                              const gchar *name,
                              gpointer user_data)
{
  SenderData *data = user_data;

  g_print ("vanished client %s %p\n", name, data);

  g_bus_unwatch_name (data->id);
}

static void
data_free (SenderData *data)
{
  g_print ("free client %s %p\n", data->sender, data);
  g_list_foreach (data->clients, (GFunc) remove_client, data);

  g_hash_table_remove (data->subscribe->priv->senders, data->sender);

  if (data->sender_subscribe) {
    g_signal_handler_disconnect (data->sender_subscribe, data->signal_event);
    g_signal_handler_disconnect (data->sender_subscribe, data->signal_state);
    g_object_unref (data->sender_subscribe);
  }

  g_free (data->sender);
  g_free (data);
}

static SenderData *
sender_data_new (PvSubscribe *subscribe, const gchar *sender)
{
  PvSubscribePrivate *priv = subscribe->priv;
  SenderData *data;

  data = g_new0 (SenderData, 1);
  data->subscribe = subscribe;
  data->sender = g_strdup (sender);

  g_print ("watch name %s %p\n", sender, data);

  data->id = g_bus_watch_name_on_connection (priv->connection,
                                    sender,
                                    G_BUS_NAME_WATCHER_FLAGS_NONE,
                                    client_name_appeared_handler,
                                    client_name_vanished_handler,
                                    data,
                                    (GDestroyNotify) data_free);

  g_hash_table_insert (priv->senders, data->sender, data);
  priv->pending_subscribes++;

  return data;
}

static void
notify_subscription (PvSubscribe         *subscribe,
                     GDBusObject         *object,
                     GDBusInterface      *interface,
                     PvSubscriptionEvent  event)
{
  PvSubscribePrivate *priv = subscribe->priv;

  if (priv->subscription_mask & PV_SUBSCRIPTION_FLAGS_DAEMON) {
    PvDaemon1 *daemon;

    if (interface == NULL)
      daemon = pv_object_peek_daemon1 (PV_OBJECT (object));
    else if (PV_IS_DAEMON1_PROXY (interface))
      daemon = PV_DAEMON1 (interface);
    else
      daemon = NULL;

    if (daemon) {
      g_signal_emit (subscribe, signals[SIGNAL_SUBSCRIPTION_EVENT], 0, event,
          PV_SUBSCRIPTION_FLAGS_DAEMON, daemon);
    }
  }
  if (priv->subscription_mask & PV_SUBSCRIPTION_FLAGS_CLIENT) {
    PvClient1 *client;

    if (interface == NULL)
      client = pv_object_peek_client1 (PV_OBJECT (object));
    else if (PV_IS_CLIENT1_PROXY (interface))
      client = PV_CLIENT1 (interface);
    else
      client = NULL;

    if (client) {
      const gchar *sender;
      SenderData *data;

      sender = pv_client1_get_name (client);

      data = g_hash_table_lookup (priv->senders, sender);
      if (data == NULL && event != PV_SUBSCRIPTION_EVENT_REMOVE) {
        data = sender_data_new (subscribe, sender);
      }
      if (data) {
        if (event == PV_SUBSCRIPTION_EVENT_NEW)
          data->clients = g_list_prepend (data->clients, client);
        else if (event == PV_SUBSCRIPTION_EVENT_REMOVE)
          data->clients = g_list_remove (data->clients, client);

        g_signal_emit (subscribe, signals[SIGNAL_SUBSCRIPTION_EVENT], 0, event,
            PV_SUBSCRIPTION_FLAGS_CLIENT, client);
      }
    }
  }
  if (priv->subscription_mask & PV_SUBSCRIPTION_FLAGS_SOURCE) {
    PvSource1 *source;

    if (interface == NULL)
      source = pv_object_peek_source1 (PV_OBJECT (object));
    else if (PV_IS_SOURCE1_PROXY (interface))
      source = PV_SOURCE1 (interface);
    else
      source = NULL;

    if (source) {
      g_signal_emit (subscribe, signals[SIGNAL_SUBSCRIPTION_EVENT], 0, event,
          PV_SUBSCRIPTION_FLAGS_SOURCE, source);
    }
  }
  if (priv->subscription_mask & PV_SUBSCRIPTION_FLAGS_SOURCE_OUTPUT) {
    PvSourceOutput1 *output;

    if (interface == NULL)
      output = pv_object_peek_source_output1 (PV_OBJECT (object));
    else if PV_IS_SOURCE_OUTPUT1_PROXY (interface)
      output = PV_SOURCE_OUTPUT1 (interface);
    else
      output = NULL;

    if (output) {
      g_signal_emit (subscribe, signals[SIGNAL_SUBSCRIPTION_EVENT], 0, event,
          PV_SUBSCRIPTION_FLAGS_SOURCE_OUTPUT, output);
    }
  }
}

static void
on_client_manager_interface_added (GDBusObjectManager *manager,
                                   GDBusObject        *object,
                                   GDBusInterface     *interface,
                                   gpointer            user_data)
{
  PvSubscribe *subscribe = user_data;
  notify_subscription (subscribe, object, interface, PV_SUBSCRIPTION_EVENT_NEW);
}

static void
on_client_manager_interface_removed (GDBusObjectManager *manager,
                                     GDBusObject        *object,
                                     GDBusInterface     *interface,
                                     gpointer            user_data)
{
  PvSubscribe *subscribe = user_data;
  notify_subscription (subscribe, object, interface, PV_SUBSCRIPTION_EVENT_REMOVE);
}

static void
on_client_manager_object_added (GDBusObjectManager *manager,
                                GDBusObject        *object,
                                gpointer            user_data)
{
  PvSubscribe *subscribe = user_data;
  notify_subscription (subscribe, object, NULL, PV_SUBSCRIPTION_EVENT_NEW);
}

static void
on_client_manager_object_removed (GDBusObjectManager *manager,
                                  GDBusObject        *object,
                                  gpointer            user_data)
{
  PvSubscribe *subscribe = user_data;
  notify_subscription (subscribe, object, NULL, PV_SUBSCRIPTION_EVENT_REMOVE);
}

static void
on_client_manager_properties_changed (GDBusObjectManagerClient *manager,
                                      GDBusObjectProxy         *object_proxy,
                                      GDBusProxy               *interface_proxy,
                                      GVariant                 *changed_properties,
                                      GStrv                     invalidated_properties,
                                      gpointer                  user_data)
{
  g_print ("properties changed\n");
}

static void
on_client_manager_signal (GDBusObjectManagerClient *manager,
                          GDBusObjectProxy         *object_proxy,
                          GDBusProxy               *interface_proxy,
                          gchar                    *sender_name,
                          gchar                    *signal_name,
                          GVariant                 *parameters,
                          gpointer                  user_data)
{
  g_print ("proxy signal %s\n", signal_name);
}

static void
client_manager_appeared (PvSubscribe *subscribe)
{
  PvSubscribePrivate *priv = subscribe->priv;
  GList *objects, *walk;

  objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (priv->client_manager));
  for (walk = objects; walk ; walk = g_list_next (walk)) {
    on_client_manager_object_added (G_DBUS_OBJECT_MANAGER (priv->client_manager),
                                    walk->data,
                                    subscribe);
  }
  if (--priv->pending_subscribes == 0)
    subscription_set_state (subscribe, PV_SUBSCRIPTION_STATE_READY);
}

static void
client_manager_disappeared (PvSubscribe *subscribe)
{
}

static void
on_client_manager_name_owner (GObject    *object,
                              GParamSpec *pspec,
                              gpointer    user_data)
{
  PvSubscribe *subscribe = user_data;
  PvSubscribePrivate *priv = subscribe->priv;
  gchar *name_owner;

  g_object_get (priv->client_manager, "name-owner", &name_owner, NULL);
  g_print ("client manager %s %s\n",
      g_dbus_object_manager_client_get_name (G_DBUS_OBJECT_MANAGER_CLIENT (priv->client_manager)),
      name_owner);

  if (name_owner) {
    client_manager_appeared (subscribe);
    g_free (name_owner);
  } else {
    client_manager_disappeared (subscribe);
  }
}


static void
connect_client_signals (PvSubscribe *subscribe)
{
  PvSubscribePrivate *priv = subscribe->priv;

  g_signal_connect (priv->client_manager, "notify::name-owner",
      (GCallback) on_client_manager_name_owner, subscribe);
  g_signal_connect (priv->client_manager, "interface-added",
      (GCallback) on_client_manager_interface_added, subscribe);
  g_signal_connect (priv->client_manager, "interface-removed",
      (GCallback) on_client_manager_interface_removed, subscribe);
  g_signal_connect (priv->client_manager, "object-added",
      (GCallback) on_client_manager_object_added, subscribe);
  g_signal_connect (priv->client_manager, "object-removed",
      (GCallback) on_client_manager_object_removed, subscribe);
  g_signal_connect (priv->client_manager, "interface-proxy-signal",
      (GCallback) on_client_manager_signal, subscribe);
  g_signal_connect (priv->client_manager, "interface-proxy-properties-changed",
      (GCallback) on_client_manager_properties_changed, subscribe);
}

static void
on_client_manager_ready (GObject *source_object,
                         GAsyncResult *res,
                         gpointer user_data)
{
  PvSubscribe *subscribe = user_data;
  PvSubscribePrivate *priv = subscribe->priv;
  GError *error = NULL;

  priv->client_manager = pv_object_manager_client_new_finish (res, &error);
  if (priv->client_manager == NULL)
    goto manager_error;

  connect_client_signals (subscribe);

  on_client_manager_name_owner (G_OBJECT (priv->client_manager), NULL, subscribe);
  g_object_unref (subscribe);

  return;

  /* ERRORS */
manager_error:
  {
    g_warning ("could not create client manager: %s", error->message);
    subscription_set_state (subscribe, PV_SUBSCRIPTION_STATE_ERROR);
    priv->error = error;
    g_object_unref (subscribe);
    return;
  }
}

static void
install_subscription (PvSubscribe *subscribe)
{
  PvSubscribePrivate *priv = subscribe->priv;

  subscription_set_state (subscribe, PV_SUBSCRIPTION_STATE_CONNECTING);

  g_print ("new client manager for %s\n", priv->service);
  pv_object_manager_client_new (priv->connection,
                                G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                priv->service,
                                PV_DBUS_OBJECT_PREFIX,
                                priv->cancellable,
                                on_client_manager_ready,
                                g_object_ref (subscribe));
  priv->pending_subscribes++;
}

static void
uninstall_subscription (PvSubscribe *subscribe)
{
  PvSubscribePrivate *priv = subscribe->priv;

  g_clear_object (&priv->client_manager);
  g_clear_error (&priv->error);
  subscription_set_state (subscribe, PV_SUBSCRIPTION_STATE_UNCONNECTED);
}

static void
pv_subscribe_get_property (GObject    *_object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  PvSubscribe *subscribe = PV_SUBSCRIBE (_object);
  PvSubscribePrivate *priv = subscribe->priv;

  switch (prop_id) {
    case PROP_CONNECTION:
      g_value_set_object (value, priv->connection);
      break;

    case PROP_SERVICE:
      g_value_set_string (value, priv->service);
      break;

    case PROP_SUBSCRIPTION_MASK:
      g_value_set_flags (value, priv->subscription_mask);
      break;

    case PROP_STATE:
      g_value_set_enum (value, priv->state);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (subscribe, prop_id, pspec);
      break;
  }
}

static void
pv_subscribe_set_property (GObject      *_object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  PvSubscribe *subscribe = PV_SUBSCRIBE (_object);
  PvSubscribePrivate *priv = subscribe->priv;

  switch (prop_id) {
    case PROP_CONNECTION:
    {
      uninstall_subscription (subscribe);
      if (priv->connection)
        g_object_unref (priv->connection);
      priv->connection = g_value_dup_object (value);
      if (priv->connection)
        install_subscription (subscribe);
      break;
    }

    case PROP_SERVICE:
      g_free (priv->service);
      priv->service = g_value_dup_string (value);
      break;

    case PROP_SUBSCRIPTION_MASK:
      priv->subscription_mask = g_value_get_flags (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (subscribe, prop_id, pspec);
      break;
  }
}

static void
pv_subscribe_finalize (GObject * object)
{
  PvSubscribe *subscribe = PV_SUBSCRIBE (object);
  PvSubscribePrivate *priv = subscribe->priv;

  g_print ("cancel\n");
  g_cancellable_cancel (priv->cancellable);
  g_hash_table_unref (priv->senders);
  if (priv->client_manager)
    g_object_unref (priv->client_manager);
  g_object_unref (priv->cancellable);
  g_free (priv->service);

  G_OBJECT_CLASS (pv_subscribe_parent_class)->finalize (object);
}

static void
pv_subscribe_class_init (PvSubscribeClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PvSubscribePrivate));

  gobject_class->finalize = pv_subscribe_finalize;
  gobject_class->set_property = pv_subscribe_set_property;
  gobject_class->get_property = pv_subscribe_get_property;

  /**
   * PvSubscribe:connection
   *
   * The connection of the subscribe.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_CONNECTION,
                                   g_param_spec_object ("connection",
                                                        "Connection",
                                                        "The DBus connection",
                                                        G_TYPE_DBUS_CONNECTION,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));
  /**
   * PvSubscribe:service
   *
   * The service of the subscribe.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_SERVICE,
                                   g_param_spec_string ("service",
                                                        "Service",
                                                        "The service",
                                                        PV_DBUS_SERVICE,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));
  /**
   * PvSubscribe:subscription-mask
   *
   * A mask for what object notifications will be signaled with
   * PvSubscribe:subscription-event
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
   * PvSubscribe:state
   *
   * The state of the subscription
   */
  g_object_class_install_property (gobject_class,
                                   PROP_STATE,
                                   g_param_spec_enum ("state",
                                                      "State",
                                                      "The state",
                                                      PV_TYPE_SUBSCRIPTION_STATE,
                                                      PV_SUBSCRIPTION_STATE_UNCONNECTED,
                                                      G_PARAM_READABLE |
                                                      G_PARAM_STATIC_STRINGS));
  /**
   * PvSubscribe:subscription-event
   * @subscribe: The #PvSubscribe emitting the signal.
   * @event: A #PvSubscriptionEvent
   * @flags: #PvSubscriptionFlags indicating the object
   * @path: the object path
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
pv_subscribe_init (PvSubscribe * subscribe)
{
  PvSubscribePrivate *priv = subscribe->priv = PV_SUBSCRIBE_GET_PRIVATE (subscribe);

  priv->service = g_strdup (PV_DBUS_SERVICE);
  priv->senders = g_hash_table_new (g_str_hash, g_str_equal);
  priv->state = PV_SUBSCRIPTION_STATE_UNCONNECTED;
  priv->cancellable = g_cancellable_new ();
}

/**
 * pv_subscribe_new:
 * @name: an application name
 * @properties: optional properties
 *
 * Make a new unconnected #PvSubscribe
 *
 * Returns: a new unconnected #PvSubscribe
 */
PvSubscribe *
pv_subscribe_new (void)
{
  return g_object_new (PV_TYPE_SUBSCRIBE, NULL);
}

PvSubscriptionState
pv_subscribe_get_state (PvSubscribe *subscribe)
{
  PvSubscribePrivate *priv;

  g_return_val_if_fail (PV_IS_SUBSCRIBE (subscribe), PV_SUBSCRIPTION_STATE_ERROR);
  priv = subscribe->priv;

  return priv->state;
}

GError *
pv_subscribe_get_error (PvSubscribe *subscribe)
{
  PvSubscribePrivate *priv;

  g_return_val_if_fail (PV_IS_SUBSCRIBE (subscribe), NULL);
  priv = subscribe->priv;

  return priv->error;
}

