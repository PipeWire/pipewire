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

static void
notify_subscription (PvContext      *context,
                     GDBusObject    *object,
                     GDBusInterface *interface,
                     PvSubscriptionEvent event)
{
  PvContextPrivate *priv = context->priv;

  if (priv->subscription_mask & PV_SUBSCRIPTION_FLAGS_CLIENT) {
    if ((interface == NULL && pv_object_peek_client1 (PV_OBJECT (object))) ||
        PV_IS_CLIENT1_PROXY (interface))
      g_signal_emit (context, signals[SIGNAL_SUBSCRIPTION_EVENT], 0, event,
          PV_SUBSCRIPTION_FLAGS_CLIENT, g_dbus_object_get_object_path (object));
  }
  if (priv->subscription_mask & PV_SUBSCRIPTION_FLAGS_DEVICE) {
    if ((interface == NULL && pv_object_peek_device1 (PV_OBJECT (object))) ||
        PV_IS_DEVICE1_PROXY (interface))
      g_signal_emit (context, signals[SIGNAL_SUBSCRIPTION_EVENT], 0, event,
          PV_SUBSCRIPTION_FLAGS_DEVICE, g_dbus_object_get_object_path (object));
  }
  if (priv->subscription_mask & PV_SUBSCRIPTION_FLAGS_SOURCE) {
    if ((interface == NULL && pv_object_peek_source1 (PV_OBJECT (object))) ||
        PV_IS_SOURCE1_PROXY (interface))
      g_signal_emit (context, signals[SIGNAL_SUBSCRIPTION_EVENT], 0, event,
          PV_SUBSCRIPTION_FLAGS_SOURCE, g_dbus_object_get_object_path (object));
  }
  if (priv->subscription_mask & PV_SUBSCRIPTION_FLAGS_SOURCE_OUTPUT) {
    if ((interface == NULL && pv_object_peek_source_output1 (PV_OBJECT (object))) ||
        PV_IS_SOURCE_OUTPUT1_PROXY (interface))
      g_signal_emit (context, signals[SIGNAL_SUBSCRIPTION_EVENT], 0, event,
          PV_SUBSCRIPTION_FLAGS_SOURCE_OUTPUT, g_dbus_object_get_object_path (object));
  }
}

static void
on_client_manager_interface_added (GDBusObjectManager *manager,
                                   GDBusObject        *object,
                                   GDBusInterface     *interface,
                                   gpointer            user_data)
{
  PvContext *context = user_data;
  notify_subscription (context, object, interface, PV_SUBSCRIPTION_EVENT_NEW);
}

static void
on_client_manager_interface_removed (GDBusObjectManager *manager,
                                     GDBusObject        *object,
                                     GDBusInterface     *interface,
                                     gpointer            user_data)
{
  PvContext *context = user_data;
  notify_subscription (context, object, interface, PV_SUBSCRIPTION_EVENT_REMOVE);
}

static void
on_client_manager_object_added (GDBusObjectManager *manager,
                                GDBusObject        *object,
                                gpointer            user_data)
{
  PvContext *context = user_data;
  notify_subscription (context, object, NULL, PV_SUBSCRIPTION_EVENT_NEW);
}

static void
on_client_manager_object_removed (GDBusObjectManager *manager,
                                  GDBusObject        *object,
                                  gpointer            user_data)
{
  PvContext *context = user_data;
  notify_subscription (context, object, NULL, PV_SUBSCRIPTION_EVENT_REMOVE);
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
connect_client_signals (PvContext *context)
{
  PvContextPrivate *priv = context->priv;

  g_signal_connect (priv->client_manager, "interface-added",
      (GCallback) on_client_manager_interface_added, context);
  g_signal_connect (priv->client_manager, "interface-removed",
      (GCallback) on_client_manager_interface_removed, context);
  g_signal_connect (priv->client_manager, "object-added",
      (GCallback) on_client_manager_object_added, context);
  g_signal_connect (priv->client_manager, "object-removed",
      (GCallback) on_client_manager_object_removed, context);
  g_signal_connect (priv->client_manager, "interface-proxy-signal",
      (GCallback) on_client_manager_signal, context);
  g_signal_connect (priv->client_manager, "interface-proxy-properties-changed",
      (GCallback) on_client_manager_properties_changed, context);
}

static void
on_client_manager_ready (GObject *source_object,
                         GAsyncResult *res,
                         gpointer user_data)
{
  PvContext *context = user_data;
  PvContextPrivate *priv = context->priv;
  GError *error = NULL;

  priv->client_manager = pv_object_manager_client_new_finish (res, &error);
  if (priv->client_manager == NULL)
    goto manager_error;

  connect_client_signals (context);

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
install_subscription (PvContext *context)
{
  PvContextPrivate *priv = context->priv;

  if (priv->client_manager)
    return;

  pv_object_manager_client_new (pv_context_get_connection (context),
                                G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                PV_DBUS_SERVICE,
                                PV_DBUS_OBJECT_PREFIX,
                                NULL,
                                on_client_manager_ready,
                                context);
}

static void
uninstall_subscription (PvContext *context)
{
  PvContextPrivate *priv = context->priv;

  g_clear_object (&priv->client_manager);
}
