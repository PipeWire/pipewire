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

#include <gio/gio.h>

#include "client/pinos.h"
#include "client/pv-enumtypes.h"

struct _PvSubscribePrivate
{
  gchar *service;
  PvSubscriptionFlags subscription_mask;

  GDBusConnection *connection;
  GCancellable *cancellable;

  GDBusProxy *manager_proxy;

  guint pending_proxies;
  GList *objects;

  PvSubscriptionState state;
  GError *error;
};

typedef struct
{
  PvSubscribe *subscribe;
  gchar *sender_name;
  gchar *object_path;
  gchar *interface_name;
  gboolean pending;
  GDBusProxy *proxy;
  GList *tasks;
  gboolean removed;
} PvObjectData;


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
notify_event (PvSubscribe         *subscribe,
              PvObjectData        *data,
              PvSubscriptionEvent  event)
{
  const gchar *interface_name;
  PvSubscriptionFlags flags = 0;

  interface_name = g_dbus_proxy_get_interface_name (data->proxy);
  if (g_strcmp0 (interface_name, "org.pinos.Daemon1") == 0) {
    flags = PV_SUBSCRIPTION_FLAGS_DAEMON;
  }
  else if (g_strcmp0 (interface_name, "org.pinos.Client1") == 0) {
    flags = PV_SUBSCRIPTION_FLAGS_CLIENT;
  }
  else if (g_strcmp0 (interface_name, "org.pinos.Source1") == 0) {
    flags = PV_SUBSCRIPTION_FLAGS_SOURCE;
  }
  else if (g_strcmp0 (interface_name, "org.pinos.SourceOutput1") == 0) {
    flags = PV_SUBSCRIPTION_FLAGS_SOURCE_OUTPUT;
  }
  g_signal_emit (subscribe, signals[SIGNAL_SUBSCRIPTION_EVENT], 0,
          event, flags, data->proxy);
}

static void
on_proxy_properties_changed (GDBusProxy *proxy,
                             GVariant   *changed_properties,
                             GStrv       invalidated_properties,
                             gpointer    user_data)
{
  PvObjectData *data = user_data;

  notify_event (data->subscribe, data, PV_SUBSCRIPTION_EVENT_CHANGE);
}

static void
object_data_free (PvObjectData *data)
{
  g_object_unref (data->proxy);
  g_free (data->sender_name);
  g_free (data->object_path);
  g_free (data->interface_name);
  g_free (data);
}

static void
remove_data (PvSubscribe *subscribe, PvObjectData *data)
{
  if (data->pending) {
    data->removed = TRUE;
  } else {
    notify_event (subscribe, data, PV_SUBSCRIPTION_EVENT_REMOVE);
    object_data_free (data);
  }
}

static void
remove_all_data (PvSubscribe *subscribe)
{
  PvSubscribePrivate *priv = subscribe->priv;
  GList *walk;

  for (walk = priv->objects; walk; walk = g_list_next (walk)) {
    PvObjectData *data = walk->data;
    remove_data (subscribe, data);
  }
  g_list_free (priv->objects);
  priv->objects = NULL;
}

static void
on_proxy_created (GObject *source_object,
                  GAsyncResult *res,
                  gpointer user_data)
{
  PvObjectData *data = user_data;
  PvSubscribe *subscribe = data->subscribe;
  PvSubscribePrivate *priv = subscribe->priv;
  GError *error = NULL;
  GList *walk;

  data->pending = FALSE;

  data->proxy = g_dbus_proxy_new_finish (res, &error);
  if (data->proxy == NULL) {
    priv->objects = g_list_remove (priv->objects, data);
    g_warning ("could not create proxy: %s", error->message);
    subscription_set_state (subscribe, PV_SUBSCRIPTION_STATE_ERROR);
    priv->error = error;
    return;
  }

  g_signal_connect (data->proxy,
                    "g-properties-changed",
                    (GCallback) on_proxy_properties_changed,
                    data);

  notify_event (subscribe, data, PV_SUBSCRIPTION_EVENT_NEW);

  for (walk = data->tasks; walk; walk = g_list_next (walk)) {
    GTask *task = walk->data;
    g_task_return_pointer (task, g_object_ref (data->proxy), g_object_unref);
    g_object_unref (task);
  }
  g_list_free (data->tasks);
  data->tasks = NULL;

  if (--priv->pending_proxies == 0)
    subscription_set_state (subscribe, PV_SUBSCRIPTION_STATE_READY);

  if (data->removed) {
    priv->objects = g_list_remove (priv->objects, data);
    remove_data (subscribe, data);
  }
}


static void
add_interface (PvSubscribe *subscribe,
               const gchar *object_path,
               const gchar *interface_name,
               GVariant    *properties)
{
  PvSubscribePrivate *priv = subscribe->priv;
  PvObjectData *data;

  data = g_new0 (PvObjectData, 1);
  data->subscribe = subscribe;
  data->sender_name = g_strdup (priv->service);
  data->object_path = g_strdup (object_path);
  data->interface_name = g_strdup (interface_name);
  data->pending = TRUE;

  priv->objects = g_list_prepend (priv->objects, data);
  priv->pending_proxies++;

  g_dbus_proxy_new (priv->connection,
                    G_DBUS_PROXY_FLAGS_NONE,
                    NULL, /* GDBusInterfaceInfo* */
                    priv->service,
                    object_path,
                    interface_name,
                    priv->cancellable,
                    on_proxy_created,
                    data);
}

static void
remove_interface (PvSubscribe *subscribe,
                  const gchar *object_path,
                  const gchar *interface_name)
{
  PvSubscribePrivate *priv = subscribe->priv;
  GList *walk;

  for (walk = priv->objects; walk; walk = g_list_next (walk)) {
    PvObjectData *data = walk->data;

    if (g_strcmp0 (data->object_path, object_path) == 0 &&
        g_strcmp0 (data->interface_name, interface_name) == 0) {
      priv->objects = g_list_remove (priv->objects, data);
      remove_data (subscribe, data);
      break;
    }
  }
}

static void
add_ifaces_and_properties (PvSubscribe *subscribe,
                           const gchar *object_path,
                           GVariant    *ifaces_and_properties)
{
  GVariantIter iter;
  const gchar *interface_name;
  GVariant *properties;

  g_variant_iter_init (&iter, ifaces_and_properties);
  while (g_variant_iter_next (&iter,
                              "{&s@a{sv}}",
                              &interface_name,
                              &properties)) {

    add_interface (subscribe, object_path, interface_name, properties);

    g_variant_unref (properties);
  }
}

static void
remove_ifaces (PvSubscribe *subscribe,
               const gchar *object_path,
               const gchar **ifaces)
{
  while (*ifaces) {
    remove_interface (subscribe, object_path, *ifaces);

    ifaces++;
  }
}

static void
on_manager_proxy_signal (GDBusProxy   *proxy,
                         const gchar  *sender_name,
                         const gchar  *signal_name,
                         GVariant     *parameters,
                         gpointer      user_data)
{
  PvSubscribe *subscribe = user_data;
  const gchar *object_path;

  if (g_strcmp0 (signal_name, "InterfacesAdded") == 0) {
    GVariant *ifaces_and_properties;

    g_variant_get (parameters,
                   "(&o@a{sa{sv}})",
                   &object_path,
                   &ifaces_and_properties);

    add_ifaces_and_properties (subscribe, object_path, ifaces_and_properties);

    g_variant_unref (ifaces_and_properties);
  } else if (g_strcmp0 (signal_name, "InterfacesRemoved") == 0) {
    const gchar **ifaces;
    g_variant_get (parameters,
                   "(&o^a&s)",
                   &object_path,
                   &ifaces);

    remove_ifaces (subscribe, object_path, ifaces);

    g_free (ifaces);
  }
}

static void
on_managed_objects_ready (GObject *source_object,
                          GAsyncResult *res,
                          gpointer user_data)
{
  PvSubscribe *subscribe = user_data;
  PvSubscribePrivate *priv = subscribe->priv;
  GError *error = NULL;
  GVariant *objects;
  GVariant *arg0;
  const gchar *object_path;
  GVariant *ifaces_and_properties;
  GVariantIter object_iter;

  objects = g_dbus_proxy_call_finish (priv->manager_proxy, res, &error);
  if (objects == NULL) {
    g_warning ("could not get objects: %s", error->message);
    subscription_set_state (subscribe, PV_SUBSCRIPTION_STATE_ERROR);
    priv->error = error;
    return;
  }

  arg0 = g_variant_get_child_value (objects, 0);
  g_variant_iter_init (&object_iter, arg0);
  while (g_variant_iter_next (&object_iter,
                              "{&o@a{sa{sv}}}",
                              &object_path,
                              &ifaces_and_properties)) {

    add_ifaces_and_properties (subscribe, object_path, ifaces_and_properties);

    g_variant_unref (ifaces_and_properties);
  }
  g_variant_unref (arg0);
  g_variant_unref (objects);

  if (priv->pending_proxies == 0)
    subscription_set_state (subscribe, PV_SUBSCRIPTION_STATE_READY);
}

static void
manager_proxy_appeared (PvSubscribe *subscribe)
{
  PvSubscribePrivate *priv = subscribe->priv;

  g_dbus_proxy_call (priv->manager_proxy,
                    "GetManagedObjects",
                    NULL, /* parameters */
                    G_DBUS_CALL_FLAGS_NONE,
                    -1,
                    priv->cancellable,
                    on_managed_objects_ready,
                    subscribe);
}

static void
manager_proxy_disappeared (PvSubscribe *subscribe)
{
  remove_all_data (subscribe);
}

static void
on_manager_proxy_name_owner (GObject    *object,
                             GParamSpec *pspec,
                             gpointer    user_data)
{
  PvSubscribe *subscribe = user_data;
  PvSubscribePrivate *priv = subscribe->priv;
  gchar *name_owner;

  g_object_get (priv->manager_proxy, "g-name-owner", &name_owner, NULL);

  if (name_owner) {
    manager_proxy_appeared (subscribe);
    g_free (name_owner);
  } else {
    manager_proxy_disappeared (subscribe);
  }
}


static void
connect_client_signals (PvSubscribe *subscribe)
{
  PvSubscribePrivate *priv = subscribe->priv;

  g_signal_connect (priv->manager_proxy, "notify::g-name-owner",
      (GCallback) on_manager_proxy_name_owner, subscribe);

  g_signal_connect (priv->manager_proxy, "g-signal",
      (GCallback) on_manager_proxy_signal, subscribe);
}

static void
on_manager_proxy_ready (GObject *source_object,
                        GAsyncResult *res,
                        gpointer user_data)
{
  PvSubscribe *subscribe = user_data;
  PvSubscribePrivate *priv = subscribe->priv;
  GError *error = NULL;

  priv->manager_proxy = g_dbus_proxy_new_finish (res, &error);
  if (priv->manager_proxy == NULL)
    goto manager_error;

  connect_client_signals (subscribe);

  on_manager_proxy_name_owner (G_OBJECT (priv->manager_proxy), NULL, subscribe);
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

  g_dbus_proxy_new (priv->connection,
                    G_DBUS_PROXY_FLAGS_NONE,
                    NULL, /* GDBusInterfaceInfo* */
                    priv->service,
                    PV_DBUS_OBJECT_PREFIX,
                    "org.freedesktop.DBus.ObjectManager",
                    priv->cancellable,
                    on_manager_proxy_ready,
                    g_object_ref (subscribe));
}

static void
uninstall_subscription (PvSubscribe *subscribe)
{
  PvSubscribePrivate *priv = subscribe->priv;

  g_clear_object (&priv->manager_proxy);
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

  g_cancellable_cancel (priv->cancellable);
  if (priv->manager_proxy)
    g_object_unref (priv->manager_proxy);
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

static gint
compare_data (PvObjectData *data, const gchar *name,
                                  const gchar *object_path,
                                  const gchar *interface_name)
{
  gint res;

  if ((res = g_strcmp0 (data->sender_name, name)) != 0)
    return res;

  if ((res = g_strcmp0 (data->object_path, object_path)) != 0)
    return res;

  return g_strcmp0 (data->interface_name, interface_name);
}

void
pv_subscribe_get_proxy (PvSubscribe *subscribe,
                        const gchar *name,
                        const gchar *object_path,
                        const gchar *interface_name,
                        GCancellable *cancellable,
                        GAsyncReadyCallback callback,
                        gpointer user_data)
{
  PvSubscribePrivate *priv;
  GList *walk;

  g_return_if_fail (PV_IS_SUBSCRIBE (subscribe));
  priv = subscribe->priv;

  for (walk = priv->objects; walk; walk = g_list_next (walk)) {
    PvObjectData *data = walk->data;

    if (compare_data (data, name, object_path, interface_name) == 0) {
      GTask *task;

      task = g_task_new (subscribe,
                         cancellable,
                         callback,
                         user_data);

      if (data->pending) {
        data->tasks = g_list_prepend (data->tasks, task);
      } else if (data->proxy) {
        g_task_return_pointer (task, g_object_ref (data->proxy), g_object_unref);
      } else {
        g_task_return_error (task, NULL);
      }
      break;
    }
  }
}

GDBusProxy *
pv_subscribe_get_proxy_finish (PvSubscribe  *subscribe,
                               GAsyncResult *res,
                               GError       **error)
{
  return g_task_propagate_pointer (G_TASK (res), error);
}

