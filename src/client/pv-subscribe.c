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
  GDBusConnection *connection;
  gchar *service;

  PvSubscriptionFlags subscription_mask;

  GDBusObjectManager *client_manager;
};


#define PV_SUBSCRIBE_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PV_TYPE_SUBSCRIBE, PvSubscribePrivate))

G_DEFINE_TYPE (PvSubscribe, pv_subscribe, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_CONNECTION,
  PROP_SERVICE,
  PROP_SUBSCRIPTION_MASK
};

enum
{
  SIGNAL_SUBSCRIPTION_EVENT,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
notify_subscription (PvSubscribe         *subscribe,
                     GDBusObject         *object,
                     GDBusInterface      *interface,
                     PvSubscriptionEvent  event)
{
  PvSubscribePrivate *priv = subscribe->priv;

  if (priv->subscription_mask & PV_SUBSCRIPTION_FLAGS_CLIENT) {
    if ((interface == NULL && pv_object_peek_client1 (PV_OBJECT (object))) ||
        PV_IS_CLIENT1_PROXY (interface))
      g_signal_emit (subscribe, signals[SIGNAL_SUBSCRIPTION_EVENT], 0, event,
          PV_SUBSCRIPTION_FLAGS_CLIENT, g_dbus_object_get_object_path (object));
  }
  if (priv->subscription_mask & PV_SUBSCRIPTION_FLAGS_SOURCE) {
    if ((interface == NULL && pv_object_peek_source1 (PV_OBJECT (object))) ||
        PV_IS_SOURCE1_PROXY (interface))
      g_signal_emit (subscribe, signals[SIGNAL_SUBSCRIPTION_EVENT], 0, event,
          PV_SUBSCRIPTION_FLAGS_SOURCE, g_dbus_object_get_object_path (object));
  }
  if (priv->subscription_mask & PV_SUBSCRIPTION_FLAGS_SOURCE_OUTPUT) {
    if ((interface == NULL && pv_object_peek_source_output1 (PV_OBJECT (object))) ||
        PV_IS_SOURCE_OUTPUT1_PROXY (interface))
      g_signal_emit (subscribe, signals[SIGNAL_SUBSCRIPTION_EVENT], 0, event,
          PV_SUBSCRIPTION_FLAGS_SOURCE_OUTPUT, g_dbus_object_get_object_path (object));
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
connect_client_signals (PvSubscribe *subscribe)
{
  PvSubscribePrivate *priv = subscribe->priv;

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

  return;

  /* ERRORS */
manager_error:
  {
    g_warning ("could not create client manager: %s", error->message);
    g_clear_error (&error);
    return;
  }
}

static void
install_subscription (PvSubscribe *subscribe)
{
  PvSubscribePrivate *priv = subscribe->priv;

  pv_object_manager_client_new (priv->connection,
                                G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                priv->service,
                                PV_DBUS_OBJECT_PREFIX,
                                NULL,
                                on_client_manager_ready,
                                subscribe);
}

static void
uninstall_subscription (PvSubscribe *subscribe)
{
  PvSubscribePrivate *priv = subscribe->priv;

  g_clear_object (&priv->client_manager);
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

  g_free (priv->service);
  g_object_unref (priv->client_manager);

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
                                                     G_TYPE_STRING);
}

static void
pv_subscribe_init (PvSubscribe * subscribe)
{
  PvSubscribePrivate *priv = subscribe->priv = PV_SUBSCRIBE_GET_PRIVATE (subscribe);

  priv->service = g_strdup (PV_DBUS_SERVICE);
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
