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

#include "pinos/client/context.h"
#include "pinos/client/enumtypes.h"
#include "pinos/client/subscribe.h"

#include "pinos/client/private.h"

/**
 * pinos_context_info_finish:
 * @object: a #GObject
 * @res: a #GAsyncResult
 * @error: location to place an error
 *
 * Call this function in the introspection GAsyncReadyCallback function
 * to get the final result of the operation.
 *
 * Returns: %TRUE if the lookup was successful. If %FALSE is returned, @error
 * will contain more details.
 */
gboolean
pinos_context_info_finish (GObject      *object,
                           GAsyncResult *res,
                           GError      **error)
{
  g_return_val_if_fail (g_task_is_valid (res, object), FALSE);

  return g_task_propagate_boolean (G_TASK (res), error);
}

#define SET_STRING(name, field, idx)                                                    \
G_STMT_START {                                                                          \
  GVariant *variant;                                                                    \
  if (!changed || g_hash_table_contains (changed, name))                                \
    info->change_mask |= 1 << idx;                                                      \
  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), name))) {      \
    info->field = g_variant_get_string (variant, NULL);                                 \
    g_variant_unref (variant);                                                          \
  } else {                                                                              \
    info->field = "Unknown";                                                            \
  }                                                                                     \
} G_STMT_END

#define SET_UINT32(name, field, idx, def)                                               \
G_STMT_START {                                                                          \
  GVariant *variant;                                                                    \
  if (!changed || g_hash_table_contains (changed, name))                                \
    info->change_mask |= 1 << idx;                                                      \
  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), name))) {      \
    info->field = g_variant_get_uint32 (variant);                                       \
    g_variant_unref (variant);                                                          \
  } else {                                                                              \
    info->field = def;                                                                  \
  }                                                                                     \
} G_STMT_END

#define SET_PROPERTIES(name, field, idx)                                                \
G_STMT_START {                                                                          \
  GVariant *variant;                                                                    \
  if (!changed || g_hash_table_contains (changed, name))                                \
    info->change_mask |= 1 << idx;                                                      \
  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), name))) {      \
    info->field = pinos_properties_from_variant (variant);                              \
    g_variant_unref (variant);                                                          \
  } else {                                                                              \
    info->field = NULL;                                                                 \
  }                                                                                     \
} G_STMT_END

#define SET_BYTES(name, field, idx)                                                     \
G_STMT_START {                                                                          \
  GVariant *variant;                                                                    \
  if (!changed || g_hash_table_contains (changed, name))                                \
    info->change_mask |= 1 << idx;                                                      \
  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), name))) {      \
    gsize len;                                                                          \
    gchar *bytes = g_variant_dup_string (variant, &len);                                \
    info->field = g_bytes_new_take (bytes, len +1);                                     \
    g_variant_unref (variant);                                                          \
  } else {                                                                              \
    info->field = NULL;                                                                 \
  }                                                                                     \
} G_STMT_END

static void
daemon_fill_info (PinosDaemonInfo *info, GDBusProxy *proxy)
{
  GHashTable *changed = g_object_get_data (G_OBJECT (proxy), "pinos-changed-properties");

  info->id = proxy;
  info->daemon_path = g_dbus_proxy_get_object_path (proxy);

  info->change_mask = 0;
  SET_STRING ("UserName", user_name, 0);
  SET_STRING ("HostName", host_name, 1);
  SET_STRING ("Version", version, 2);
  SET_STRING ("Name", name, 3);
  SET_UINT32 ("Cookie", cookie, 4, 0);
  SET_PROPERTIES ("Properties", properties, 5);

  if (changed)
    g_hash_table_remove_all (changed);
}

static void
daemon_clear_info (PinosDaemonInfo *info)
{
  if (info->properties)
    pinos_properties_free (info->properties);
}

/**
 * pinos_context_get_daemon_info:
 * @context: a #PinosContext
 * @flags: extra flags
 * @cb: a callback
 * @cancelable: a #GCancellable
 * @callback: a #GAsyncReadyCallback to call when the operation is finished
 * @user_data: user data passed to @cb
 *
 * Get the information of the daemon @context is connected to.
 */
void
pinos_context_get_daemon_info (PinosContext *context,
                               PinosDaemonInfoFlags flags,
                               PinosDaemonInfoCallback cb,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
  PinosDaemonInfo info;
  GTask *task;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (cb != NULL);

  task = g_task_new (context, cancellable, callback, user_data);

  daemon_fill_info (&info, context->priv->daemon);
  cb (context, &info, user_data);
  daemon_clear_info (&info);

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

static void
client_fill_info (PinosClientInfo *info, GDBusProxy *proxy)
{
  GHashTable *changed = g_object_get_data (G_OBJECT (proxy), "pinos-changed-properties");

  info->id = proxy;
  info->client_path = g_dbus_proxy_get_object_path (proxy);
  SET_STRING ("Sender", sender, 0);

  info->change_mask = 0;
  SET_PROPERTIES ("Properties", properties, 0);

  if (changed)
    g_hash_table_remove_all (changed);
}

static void
client_clear_info (PinosClientInfo *info)
{
  if (info->properties)
    pinos_properties_free (info->properties);
}


/**
 * pinos_context_list_client_info:
 * @context: a connected #PinosContext
 * @flags: extra #PinosClientInfoFlags
 * @cb: a #PinosClientInfoCallback
 * @cancelable: a #GCancellable
 * @callback: a #GAsyncReadyCallback to call when the operation is finished
 * @user_data: user data passed to @cb
 *
 * Call @cb for each client.
 */
void
pinos_context_list_client_info (PinosContext *context,
                                PinosClientInfoFlags flags,
                                PinosClientInfoCallback cb,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
  PinosContextPrivate *priv;
  GList *walk;
  GTask *task;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (cb != NULL);

  task = g_task_new (context, cancellable, callback, user_data);

  priv = context->priv;

  for (walk = priv->clients; walk; walk = g_list_next (walk)) {
    GDBusProxy *proxy = walk->data;
    PinosClientInfo info;

    client_fill_info (&info, proxy);
    cb (context, &info, user_data);
    client_clear_info (&info);
  }

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

/**
 * pinos_context_get_client_info_by_id:
 * @context: a connected #PinosContext
 * @id: a client id
 * @flags: extra #PinosClientInfoFlags
 * @cb: a #PinosClientInfoCallback
 * @cancelable: a #GCancellable
 * @callback: a #GAsyncReadyCallback to call when the operation is finished
 * @user_data: user data passed to @cb
 *
 * Call @cb for the client with @id.
 */
void
pinos_context_get_client_info_by_id (PinosContext *context,
                                     gpointer id,
                                     PinosClientInfoFlags flags,
                                     PinosClientInfoCallback cb,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
  PinosClientInfo info;
  GDBusProxy *proxy;
  GTask *task;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (id != NULL);
  g_return_if_fail (cb != NULL);

  task = g_task_new (context, cancellable, callback, user_data);

  proxy = G_DBUS_PROXY (id);

  client_fill_info (&info, proxy);
  cb (context, &info, user_data);
  client_clear_info (&info);

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

/**
 * pinos_node_state_as_string:
 * @state: a #PinosNodeeState
 *
 * Return the string representation of @state.
 *
 * Returns: the string representation of @state.
 */
const gchar *
pinos_node_state_as_string (PinosNodeState state)
{
  GEnumValue *val;

  val = g_enum_get_value (G_ENUM_CLASS (g_type_class_ref (PINOS_TYPE_NODE_STATE)),
                          state);

  return val == NULL ? "invalid-state" : val->value_nick;
}

static void
node_fill_info (PinosNodeInfo *info, GDBusProxy *proxy)
{
  GHashTable *changed = g_object_get_data (G_OBJECT (proxy), "pinos-changed-properties");

  info->id = proxy;
  info->node_path = g_dbus_proxy_get_object_path (proxy);

  info->change_mask = 0;
  SET_STRING ("Name", name, 0);
  SET_PROPERTIES ("Properties", properties, 1);
  SET_UINT32 ("State", state, 2, PINOS_NODE_STATE_ERROR);

  if (changed)
    g_hash_table_remove_all (changed);
}

static void
node_clear_info (PinosNodeInfo *info)
{
  if (info->properties)
    pinos_properties_free (info->properties);
}

/**
 * pinos_context_list_node_info:
 * @context: a connected #PinosContext
 * @flags: extra #PinosNodeInfoFlags
 * @cb: a #PinosNodeInfoCallback
 * @cancelable: a #GCancellable
 * @callback: a #GAsyncReadyCallback to call when the operation is finished
 * @user_data: user data passed to @cb
 *
 * Call @cb for each node.
 */
void
pinos_context_list_node_info (PinosContext          *context,
                              PinosNodeInfoFlags     flags,
                              PinosNodeInfoCallback  cb,
                              GCancellable          *cancellable,
                              GAsyncReadyCallback    callback,
                              gpointer               user_data)
{
  PinosContextPrivate *priv;
  GList *walk;
  GTask *task;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (cb != NULL);

  task = g_task_new (context, cancellable, callback, user_data);

  priv = context->priv;

  for (walk = priv->nodes; walk; walk = g_list_next (walk)) {
    GDBusProxy *proxy = walk->data;
    PinosNodeInfo info;

    node_fill_info (&info, proxy);
    cb (context, &info, user_data);
    node_clear_info (&info);
  }

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

/**
 * pinos_context_get_node_info_by_id:
 * @context: a connected #PinosContext
 * @id: a node id
 * @flags: extra #PinosNodeInfoFlags
 * @cb: a #PinosNodeInfoCallback
 * @cancelable: a #GCancellable
 * @callback: a #GAsyncReadyCallback to call when the operation is finished
 * @user_data: user data passed to @cb
 *
 * Call @cb for the node with @id.
 */
void
pinos_context_get_node_info_by_id (PinosContext *context,
                                   gpointer id,
                                   PinosNodeInfoFlags flags,
                                   PinosNodeInfoCallback cb,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  PinosNodeInfo info;
  GDBusProxy *proxy;
  GTask *task;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (id != NULL);
  g_return_if_fail (cb != NULL);

  task = g_task_new (context, cancellable, callback, user_data);

  proxy = G_DBUS_PROXY (id);

  node_fill_info (&info, proxy);
  cb (context, &info, user_data);
  node_clear_info (&info);

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

/**
 * pinos_direction_as_string:
 * @direction: a #PinosDirection
 *
 * Return the string representation of @direction.
 *
 * Returns: the string representation of @direction.
 */
const gchar *
pinos_direction_as_string (PinosDirection direction)
{
  GEnumValue *val;

  val = g_enum_get_value (G_ENUM_CLASS (g_type_class_ref (PINOS_TYPE_DIRECTION)),
                          direction);

  return val == NULL ? "invalid-direction" : val->value_nick;
}

static void
port_fill_info (PinosPortInfo *info, GDBusProxy *proxy)
{
  GHashTable *changed = g_object_get_data (G_OBJECT (proxy), "pinos-changed-properties");

  info->id = proxy;
  info->port_path = g_dbus_proxy_get_object_path (proxy);
  SET_UINT32 ("Direction", direction, 0, PINOS_DIRECTION_INVALID);
  SET_STRING ("Node", node_path, 0);

  info->change_mask = 0;
  SET_STRING ("Name", name, 0);
  SET_PROPERTIES ("Properties", properties, 1);
  SET_BYTES ("PossibleFormats", possible_formats, 2);

  if (changed)
    g_hash_table_remove_all (changed);
}

static void
port_clear_info (PinosPortInfo *info)
{
  if (info->properties)
    pinos_properties_free (info->properties);
  if (info->possible_formats)
    g_bytes_unref (info->possible_formats);
}

/**
 * pinos_context_list_port_info:
 * @context: a connected #PinosContext
 * @flags: extra #PinosPortInfoFlags
 * @cb: a #PinosPortInfoCallback
 * @cancelable: a #GCancellable
 * @callback: a #GAsyncReadyCallback to call when the operation is finished
 * @user_data: user data passed to @cb
 *
 * Call @cb for each port.
 */
void
pinos_context_list_port_info (PinosContext            *context,
                              PinosPortInfoFlags       flags,
                              PinosPortInfoCallback    cb,
                              GCancellable            *cancellable,
                              GAsyncReadyCallback      callback,
                              gpointer                 user_data)
{
  PinosContextPrivate *priv;
  GList *walk;
  GTask *task;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (cb != NULL);

  task = g_task_new (context, cancellable, callback, user_data);

  priv = context->priv;

  for (walk = priv->ports; walk; walk = g_list_next (walk)) {
    GDBusProxy *proxy = walk->data;
    PinosPortInfo info;

    port_fill_info (&info, proxy);
    cb (context, &info, user_data);
    port_clear_info (&info);
  }

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

/**
 * pinos_context_get_port_info_by_id:
 * @context: a connected #PinosContext
 * @id: a port id
 * @flags: extra #PinosPortInfoFlags
 * @cb: a #PinosPortInfoCallback
 * @cancelable: a #GCancellable
 * @callback: a #GAsyncReadyCallback to call when the operation is finished
 * @user_data: user data passed to @cb
 *
 * Call @cb for the port with @id.
 */
void
pinos_context_get_port_info_by_id (PinosContext *context,
                                   gpointer id,
                                   PinosPortInfoFlags flags,
                                   PinosPortInfoCallback cb,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
  PinosPortInfo info;
  GDBusProxy *proxy;
  GTask *task;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (id != NULL);
  g_return_if_fail (cb != NULL);

  task = g_task_new (context, cancellable, callback, user_data);

  proxy = G_DBUS_PROXY (id);

  port_fill_info (&info, proxy);
  cb (context, &info, user_data);
  port_clear_info (&info);

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

/**
 * pinos_channel_state_as_string:
 * @state: a #PinosChannelState
 *
 * Return the string representation of @state.
 *
 * Returns: the string representation of @state.
 */
const gchar *
pinos_channel_state_as_string (PinosChannelState state)
{
  GEnumValue *val;

  val = g_enum_get_value (G_ENUM_CLASS (g_type_class_ref (PINOS_TYPE_CHANNEL_STATE)),
                          state);

  return val == NULL ? "invalid-state" : val->value_nick;
}

static void
channel_fill_info (PinosChannelInfo *info, GDBusProxy *proxy)
{
  GHashTable *changed = g_object_get_data (G_OBJECT (proxy), "pinos-changed-properties");

  info->id = proxy;
  info->channel_path = g_dbus_proxy_get_object_path (proxy);
  SET_UINT32 ("Direction", direction, 2, PINOS_DIRECTION_INVALID);
  SET_STRING ("Client", client_path, 0);

  info->change_mask = 0;
  SET_STRING ("Port", port_path, 0);
  SET_PROPERTIES ("Properties", properties, 1);
  SET_UINT32 ("State", state, 2, PINOS_CHANNEL_STATE_ERROR);
  SET_BYTES ("PossibleFormats", possible_formats, 3);
  SET_BYTES ("Format", format, 4);

  if (changed)
    g_hash_table_remove_all (changed);
}

static void
channel_clear_info (PinosChannelInfo *info)
{
  if (info->possible_formats)
    g_bytes_unref (info->possible_formats);
  if (info->format)
    g_bytes_unref (info->format);
  if (info->properties)
    pinos_properties_free (info->properties);
}

/**
 * pinos_context_list_channel_info:
 * @context: a connected #PinosContext
 * @flags: extra #PinosChannelInfoFlags
 * @cb: a #PinosChannelInfoCallback
 * @cancelable: a #GCancellable
 * @callback: a #GAsyncReadyCallback to call when the operation is finished
 * @user_data: user data passed to @cb
 *
 * Call @cb for each channel.
 */
void
pinos_context_list_channel_info (PinosContext *context,
                                 PinosChannelInfoFlags flags,
                                 PinosChannelInfoCallback cb,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
  PinosContextPrivate *priv;
  GList *walk;
  GTask *task;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (cb != NULL);

  task = g_task_new (context, cancellable, callback, user_data);

  priv = context->priv;

  for (walk = priv->channels; walk; walk = g_list_next (walk)) {
    GDBusProxy *proxy = walk->data;
    PinosChannelInfo info;

    channel_fill_info (&info, proxy);
    cb (context, &info, user_data);
    channel_clear_info (&info);
  }

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

/**
 * pinos_context_get_channel_info_by_id:
 * @context: a connected #PinosContext
 * @id: a channel id
 * @flags: extra #PinosChannelInfoFlags
 * @cb: a #PinosChannelInfoCallback
 * @cancelable: a #GCancellable
 * @callback: a #GAsyncReadyCallback to call when the operation is finished
 * @user_data: user data passed to @cb
 *
 * Call @cb for the channel with @id.
 */
void
pinos_context_get_channel_info_by_id (PinosContext *context,
                                      gpointer id,
                                      PinosChannelInfoFlags flags,
                                      PinosChannelInfoCallback cb,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data)
{
  PinosChannelInfo info;
  GDBusProxy *proxy;
  GTask *task;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (id != NULL);
  g_return_if_fail (cb != NULL);

  task = g_task_new (context, cancellable, callback, user_data);

  proxy = G_DBUS_PROXY (id);

  channel_fill_info (&info, proxy);
  cb (context, &info, user_data);
  channel_clear_info (&info);

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}
