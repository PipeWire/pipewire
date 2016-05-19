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

#include "pinos/client/pinos.h"

#include "pinos/client/context.h"
#include "pinos/client/enumtypes.h"
#include "pinos/client/subscribe.h"
#include "pinos/client/client-node.h"

#include "pinos/client/private.h"

#define PINOS_CONTEXT_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_CONTEXT, PinosContextPrivate))

G_DEFINE_TYPE (PinosContext, pinos_context, G_TYPE_OBJECT);

static void subscription_state (GObject *object, GParamSpec *pspec, gpointer user_data);
static void subscription_cb (PinosSubscribe         *subscribe,
                             PinosSubscriptionEvent  event,
                             PinosSubscriptionFlags  flags,
                             GDBusProxy             *object,
                             gpointer                user_data);

enum
{
  PROP_0,
  PROP_MAIN_CONTEXT,
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
pinos_context_get_property (GObject    *_object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  PinosContext *context = PINOS_CONTEXT (_object);
  PinosContextPrivate *priv = context->priv;

  switch (prop_id) {
    case PROP_MAIN_CONTEXT:
      g_value_set_boxed (value, priv->context);
      break;

    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    case PROP_PROPERTIES:
      g_value_set_boxed (value, priv->properties);
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
pinos_context_set_property (GObject      *_object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  PinosContext *context = PINOS_CONTEXT (_object);
  PinosContextPrivate *priv = context->priv;

  switch (prop_id) {
    case PROP_MAIN_CONTEXT:
      priv->context = g_value_dup_boxed (value);
      break;

    case PROP_NAME:
      g_free (priv->name);
      priv->name = g_value_dup_string (value);
      break;

    case PROP_PROPERTIES:
      if (priv->properties)
        pinos_properties_free (priv->properties);
      priv->properties = g_value_dup_boxed (value);
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
pinos_context_finalize (GObject * object)
{
  PinosContext *context = PINOS_CONTEXT (object);
  PinosContextPrivate *priv = context->priv;

  g_debug ("free context %p", context);

  if (priv->id)
    g_bus_unwatch_name(priv->id);

  g_clear_pointer (&priv->context, g_main_context_unref);
  g_free (priv->name);
  if (priv->properties)
    pinos_properties_free (priv->properties);

  g_list_free (priv->nodes);
  g_list_free (priv->ports);
  g_clear_object (&priv->subscribe);
  g_clear_error (&priv->error);

  G_OBJECT_CLASS (pinos_context_parent_class)->finalize (object);
}

static void
pinos_context_class_init (PinosContextClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosContextPrivate));

  gobject_class->finalize = pinos_context_finalize;
  gobject_class->set_property = pinos_context_set_property;
  gobject_class->get_property = pinos_context_get_property;

  /**
   * PinosContext:main-context
   *
   * The main context to use
   */
  g_object_class_install_property (gobject_class,
                                   PROP_MAIN_CONTEXT,
                                   g_param_spec_boxed ("main-context",
                                                       "Main Context",
                                                       "The main context to use",
                                                       G_TYPE_MAIN_CONTEXT,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT_ONLY |
                                                       G_PARAM_STATIC_STRINGS));
  /**
   * PinosContext:name
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
   * PinosContext:properties
   *
   * Properties of the context.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_PROPERTIES,
                                   g_param_spec_boxed ("properties",
                                                       "Properties",
                                                       "Extra properties",
                                                        PINOS_TYPE_PROPERTIES,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));
  /**
   * PinosContext:state
   *
   * The state of the context.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_STATE,
                                   g_param_spec_enum ("state",
                                                      "State",
                                                      "The context state",
                                                      PINOS_TYPE_CONTEXT_STATE,
                                                      PINOS_CONTEXT_STATE_UNCONNECTED,
                                                      G_PARAM_READABLE |
                                                      G_PARAM_STATIC_STRINGS));
  /**
   * PinosContext:connection
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
   * PinosContext:subscription-mask
   *
   * The subscription mask
   */
  g_object_class_install_property (gobject_class,
                                   PROP_SUBSCRIPTION_MASK,
                                   g_param_spec_flags ("subscription-mask",
                                                       "Subscription Mask",
                                                       "The object to receive subscription events of",
                                                       PINOS_TYPE_SUBSCRIPTION_FLAGS,
                                                       0,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_STRINGS));
  /**
   * PinosContext:subscription-event
   * @subscribe: The #PinosContext emitting the signal.
   * @event: A #PinosSubscriptionEvent
   * @flags: #PinosSubscriptionFlags indicating the object
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
                                                     PINOS_TYPE_SUBSCRIPTION_EVENT,
                                                     PINOS_TYPE_SUBSCRIPTION_FLAGS,
                                                     G_TYPE_DBUS_PROXY);

}

static void
pinos_context_init (PinosContext * context)
{
  PinosContextPrivate *priv = context->priv = PINOS_CONTEXT_GET_PRIVATE (context);

  g_debug ("new context %p", context);

  priv->state = PINOS_CONTEXT_STATE_UNCONNECTED;

  priv->subscribe = pinos_subscribe_new ();
  g_object_set (priv->subscribe,
                "subscription-mask", PINOS_SUBSCRIPTION_FLAGS_ALL,
                NULL);
  g_signal_connect (priv->subscribe,
                    "subscription-event",
                    (GCallback) subscription_cb,
                    context);
  g_signal_connect (priv->subscribe,
                    "notify::state",
                    (GCallback) subscription_state,
                    context);
}

/**
 * pinos_context_state_as_string:
 * @state: a #PinosContextState
 *
 * Return the string representation of @state.
 *
 * Returns: the string representation of @state.
 */
const gchar *
pinos_context_state_as_string (PinosContextState state)
{
  GEnumValue *val;

  val = g_enum_get_value (G_ENUM_CLASS (g_type_class_ref (PINOS_TYPE_CONTEXT_STATE)),
                          state);

  return val == NULL ? "invalid-state" : val->value_nick;
}

/**
 * pinos_context_new:
 * @context: a #GMainContext to run in
 * @name: an application name
 * @properties: (transfer full): optional properties
 *
 * Make a new unconnected #PinosContext
 *
 * Returns: a new unconnected #PinosContext
 */
PinosContext *
pinos_context_new (GMainContext    *context,
                   const gchar     *name,
                   PinosProperties *properties)
{
  PinosContext *ctx;

  g_return_val_if_fail (name != NULL, NULL);

  if (properties == NULL)
    properties = pinos_properties_new ("application.name", name, NULL);

  pinos_fill_context_properties (properties);

  ctx = g_object_new (PINOS_TYPE_CONTEXT,
                      "main-context", context,
                      "name", name,
                      "properties", properties,
                      NULL);

  pinos_properties_free (properties);

  return ctx;
}

static gboolean
do_notify_state (PinosContext *context)
{
  g_object_notify (G_OBJECT (context), "state");
  g_object_unref (context);
  return FALSE;
}

static void
context_set_state (PinosContext      *context,
                   PinosContextState  state,
                   GError            *error)
{
  if (context->priv->state != state) {
    if (error) {
      g_clear_error (&context->priv->error);
      context->priv->error = error;
    }
    context->priv->state = state;
    g_main_context_invoke (context->priv->context,
                           (GSourceFunc) do_notify_state,
                           g_object_ref (context));
  } else {
    if (error)
      g_error_free (error);
  }
}

static void
on_daemon_connected (GObject      *source_object,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  PinosContext *context = user_data;

  context_set_state (context, PINOS_CONTEXT_STATE_CONNECTED, NULL);
}

static void
subscription_cb (PinosSubscribe         *subscribe,
                 PinosSubscriptionEvent  event,
                 PinosSubscriptionFlags  flags,
                 GDBusProxy             *object,
                 gpointer                user_data)
{
  PinosContext *context = user_data;
  PinosContextPrivate *priv = context->priv;

  switch (flags) {
    case PINOS_SUBSCRIPTION_FLAG_DAEMON:
      priv->daemon = g_object_ref (object);
      break;

    case PINOS_SUBSCRIPTION_FLAG_NODE:
      if (event == PINOS_SUBSCRIPTION_EVENT_NEW)
        priv->nodes = g_list_prepend (priv->nodes, object);
      else if (event == PINOS_SUBSCRIPTION_EVENT_REMOVE)
        priv->nodes = g_list_remove (priv->nodes, object);
      break;

    case PINOS_SUBSCRIPTION_FLAG_PORT:
      if (event == PINOS_SUBSCRIPTION_EVENT_NEW)
        priv->ports = g_list_prepend (priv->ports, object);
      else if (event == PINOS_SUBSCRIPTION_EVENT_REMOVE)
        priv->ports = g_list_remove (priv->ports, object);
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
  PinosContext *context = user_data;
  PinosContextPrivate *priv = context->priv;
  PinosSubscriptionState state;

  g_assert (object == G_OBJECT (priv->subscribe));

  state = pinos_subscribe_get_state (priv->subscribe);

  switch (state) {
    case PINOS_SUBSCRIPTION_STATE_READY:
      on_daemon_connected (NULL, NULL, context);
      break;

    default:
      break;
  }

}

static void
on_name_appeared (GDBusConnection *connection,
                  const gchar     *name,
                  const gchar     *name_owner,
                  gpointer         user_data)
{
  PinosContext *context = user_data;
  PinosContextPrivate *priv = context->priv;

  priv->connection = connection;

  g_object_set (priv->subscribe, "connection", priv->connection,
                                 "service", name, NULL);
}

static void
on_name_vanished (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  PinosContext *context = user_data;
  PinosContextPrivate *priv = context->priv;

  priv->connection = connection;

  g_object_set (priv->subscribe, "connection", connection, NULL);

  if (priv->flags & PINOS_CONTEXT_FLAGS_NOFAIL) {
    context_set_state (context, PINOS_CONTEXT_STATE_CONNECTING, NULL);
  } else {
    context_set_state (context,
                       PINOS_CONTEXT_STATE_ERROR,
                       g_error_new_literal (G_IO_ERROR,
                                            G_IO_ERROR_CLOSED,
                                            "Connection closed"));
  }
}

static gboolean
do_connect (PinosContext *context)
{
  PinosContextPrivate *priv = context->priv;
  GBusNameWatcherFlags nw_flags;

  nw_flags = G_BUS_NAME_WATCHER_FLAGS_NONE;
  if (!(priv->flags & PINOS_CONTEXT_FLAGS_NOAUTOSPAWN))
    nw_flags = G_BUS_NAME_WATCHER_FLAGS_AUTO_START;

  priv->id = g_bus_watch_name (G_BUS_TYPE_SESSION,
                               PINOS_DBUS_SERVICE,
                               nw_flags,
                               on_name_appeared,
                               on_name_vanished,
                               context,
                               NULL);
  g_object_unref (context);

  return FALSE;
}

/**
 * pinos_context_connect:
 * @context: a #PinosContext
 * @flags: #PinosContextFlags
 *
 * Connect to the daemon with @flags
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_context_connect (PinosContext      *context,
                       PinosContextFlags  flags)
{
  PinosContextPrivate *priv;

  g_return_val_if_fail (PINOS_IS_CONTEXT (context), FALSE);

  priv = context->priv;
  g_return_val_if_fail (priv->connection == NULL, FALSE);

  priv->flags = flags;

  context_set_state (context, PINOS_CONTEXT_STATE_CONNECTING, NULL);
  g_main_context_invoke (priv->context,
                         (GSourceFunc) do_connect,
                         g_object_ref (context));

  return TRUE;
}

static void
do_disconnect (PinosContext *context)
{
  PinosContextPrivate *priv = context->priv;

  g_clear_object (&priv->daemon);
  if (priv->id) {
    g_bus_unwatch_name(priv->id);
    priv->id = 0;
  }
  context_set_state (context, PINOS_CONTEXT_STATE_UNCONNECTED, NULL);
}

/**
 * pinos_context_disconnect:
 * @context: a #PinosContext
 *
 * Disonnect from the daemon.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_context_disconnect (PinosContext *context)
{
  PinosContextPrivate *priv;

  g_return_val_if_fail (PINOS_IS_CONTEXT (context), FALSE);

  priv = context->priv;
  g_return_val_if_fail (!priv->disconnecting, FALSE);

  priv->disconnecting = TRUE;

  g_main_context_invoke (priv->context,
                         (GSourceFunc) do_disconnect,
                         g_object_ref (context));

  return TRUE;
}

/**
 * pinos_context_get_state:
 * @context: a #PinosContext
 *
 * Get the state of @context.
 *
 * Returns: the state of @context
 */
PinosContextState
pinos_context_get_state (PinosContext *context)
{
  PinosContextPrivate *priv;

  g_return_val_if_fail (PINOS_IS_CONTEXT (context), PINOS_CONTEXT_STATE_ERROR);
  priv = context->priv;

  return priv->state;
}

/**
 * pinos_context_get_error:
 * @context: a #PinosContext
 *
 * Get the current error of @context or %NULL when the context state
 * is not #PINOS_CONTEXT_STATE_ERROR
 *
 * Returns: the last error or %NULL
 */
const GError *
pinos_context_get_error (PinosContext *context)
{
  PinosContextPrivate *priv;

  g_return_val_if_fail (PINOS_IS_CONTEXT (context), NULL);
  priv = context->priv;

  return priv->error;
}

typedef struct {
  gchar *factory_name;
  gchar *name;
  PinosProperties *properties;
} CreateNodeData;

static void
create_node_data_free (CreateNodeData *data)
{
  g_free (data->factory_name);
  g_free (data->name);
  if (data->properties)
    pinos_properties_free (data->properties);
  g_slice_free (CreateNodeData, data);
}

static void
on_node_proxy (GObject      *source_object,
               GAsyncResult *res,
               gpointer      user_data)
{
  GTask *task = user_data;
  PinosContext *context = g_task_get_source_object (task);
  GError *error = NULL;
  GDBusProxy *proxy;
  PinosClientNode *node;

  proxy = pinos_subscribe_get_proxy_finish (context->priv->subscribe,
                                            res,
                                            &error);
  if (proxy == NULL)
    goto node_failed;

  node = pinos_client_node_new (context, proxy);

  g_task_return_pointer (task, node, (GDestroyNotify) g_object_unref);
  g_object_unref (task);

  return;

node_failed:
  {
    g_warning ("failed to get node proxy: %s", error->message);
    g_task_return_error (task, error);
    g_object_unref (task);
    return;
  }
}

static void
on_node_created (GObject      *source_object,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  GTask *task = user_data;
  PinosContext *context = g_task_get_source_object (task);
  GVariant *ret;
  GError *error = NULL;
  const gchar *node_path;

  g_assert (context->priv->daemon == G_DBUS_PROXY (source_object));

  ret = g_dbus_proxy_call_finish (context->priv->daemon, res, &error);
  if (ret == NULL)
    goto create_failed;

  g_variant_get (ret, "(&o)", &node_path);

  pinos_subscribe_get_proxy (context->priv->subscribe,
                             PINOS_DBUS_SERVICE,
                             node_path,
                             "org.pinos.Node1",
                             NULL,
                             on_node_proxy,
                             task);
  g_variant_unref (ret);

  return;

  /* ERRORS */
create_failed:
  {
    g_warning ("failed to create node: %s", error->message);
    g_task_return_error (task, error);
    g_object_unref (task);
    return;
  }
}


static gboolean
do_create_node (GTask *task)
{
  PinosContext *context = g_task_get_source_object (task);
  CreateNodeData *data = g_task_get_task_data (task);

  g_dbus_proxy_call (context->priv->daemon,
                     "CreateNode",
                     g_variant_new ("(ss@a{sv})",
                       data->factory_name,
                       data->name,
                       pinos_properties_to_variant (data->properties)),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     NULL, /* GCancellable *cancellable */
                     on_node_created,
                     task);
  return FALSE;
}

/**
 * pinos_context_create_node:
 * @context: a #PinosContext
 * @name: the name of the Node
 * @properties: properties of the node
 * @cancelable: a #GCancellable
 * @callback: a #GAsyncReadyCallback
 * @user_data: user data.
 *
 * Asynchronously create a new node in @context.
 */
void
pinos_context_create_node (PinosContext        *context,
                           const gchar         *factory_name,
                           const gchar         *name,
                           PinosProperties     *properties,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
  PinosContextPrivate *priv;
  GTask *task;
  CreateNodeData *data;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  priv = context->priv;

  task = g_task_new (context,
                     cancellable,
                     callback,
                     user_data);

  data = g_slice_new (CreateNodeData);
  data->factory_name = g_strdup (factory_name);
  data->name = g_strdup (name);
  data->properties = pinos_properties_merge (priv->properties, properties);

  g_task_set_task_data (task, data, (GDestroyNotify) create_node_data_free);

  g_main_context_invoke (context->priv->context,
                        (GSourceFunc) do_create_node,
                        task);
}

/**
 * pinos_context_create_node_finish:
 * @context: a #PinosContext
 * @res: a #GAsyncResult
 * @error: a #GError or %NULL
 *
 * Get the newly created #PinosNode. This function should be called in the callback
 * of pinos_context_create_node() to get the result or error.
 *
 * Returns: a new #PinosNode. If %NULL is returned, @error will
 *          be set.
 */
PinosNode *
pinos_context_create_node_finish (PinosContext   *context,
                                  GAsyncResult   *res,
                                  GError         **error)
{
  return g_task_propagate_pointer (G_TASK (res), error);
}
