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

#include "client/pinos.h"

#include "client/context.h"
#include "client/enumtypes.h"
#include "client/subscribe.h"

#include "client/private.h"

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

  info->change_mask = 0;
  SET_STRING ("Name", name, 0);
  SET_PROPERTIES ("Properties", properties, 1);

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
 * pinos_source_state_as_string:
 * @state: a #PinosSourceState
 *
 * Return the string representation of @state.
 *
 * Returns: the string representation of @state.
 */
const gchar *
pinos_source_state_as_string (PinosSourceState state)
{
  GEnumValue *val;

  val = g_enum_get_value (G_ENUM_CLASS (g_type_class_ref (PINOS_TYPE_SOURCE_STATE)),
                          state);

  return val == NULL ? "invalid-state" : val->value_nick;
}

static void
source_fill_info (PinosSourceInfo *info, GDBusProxy *proxy)
{
  GHashTable *changed = g_object_get_data (G_OBJECT (proxy), "pinos-changed-properties");

  info->id = proxy;
  info->source_path = g_dbus_proxy_get_object_path (proxy);

  info->change_mask = 0;
  SET_STRING ("Name", name, 0);
  SET_PROPERTIES ("Properties", properties, 1);
  SET_UINT32 ("State", state, 2, PINOS_SOURCE_STATE_ERROR);
  SET_BYTES ("PossibleFormats", possible_formats, 3);

  if (changed)
    g_hash_table_remove_all (changed);
}

static void
source_clear_info (PinosSourceInfo *info)
{
  if (info->properties)
    pinos_properties_free (info->properties);
  if (info->possible_formats)
    g_bytes_unref (info->possible_formats);
}

/**
 * pinos_context_list_source_info:
 * @context: a connected #PinosContext
 * @flags: extra #PinosSourceInfoFlags
 * @cb: a #PinosSourceInfoCallback
 * @cancelable: a #GCancellable
 * @callback: a #GAsyncReadyCallback to call when the operation is finished
 * @user_data: user data passed to @cb
 *
 * Call @cb for each source.
 */
void
pinos_context_list_source_info (PinosContext            *context,
                                PinosSourceInfoFlags     flags,
                                PinosSourceInfoCallback  cb,
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

  for (walk = priv->sources; walk; walk = g_list_next (walk)) {
    GDBusProxy *proxy = walk->data;
    PinosSourceInfo info;

    source_fill_info (&info, proxy);
    cb (context, &info, user_data);
    source_clear_info (&info);
  }

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

/**
 * pinos_context_get_source_info_by_id:
 * @context: a connected #PinosContext
 * @id: a source id
 * @flags: extra #PinosSourceInfoFlags
 * @cb: a #PinosSourceInfoCallback
 * @cancelable: a #GCancellable
 * @callback: a #GAsyncReadyCallback to call when the operation is finished
 * @user_data: user data passed to @cb
 *
 * Call @cb for the source with @id.
 */
void
pinos_context_get_source_info_by_id (PinosContext *context,
                                     gpointer id,
                                     PinosSourceInfoFlags flags,
                                     PinosSourceInfoCallback cb,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
  PinosSourceInfo info;
  GDBusProxy *proxy;
  GTask *task;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (id != NULL);
  g_return_if_fail (cb != NULL);

  task = g_task_new (context, cancellable, callback, user_data);

  proxy = G_DBUS_PROXY (id);

  source_fill_info (&info, proxy);
  cb (context, &info, user_data);
  source_clear_info (&info);

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

/**
 * pinos_source_output_state_as_string:
 * @state: a #PinosSourceOutputState
 *
 * Return the string representation of @state.
 *
 * Returns: the string representation of @state.
 */
const gchar *
pinos_source_output_state_as_string (PinosSourceOutputState state)
{
  GEnumValue *val;

  val = g_enum_get_value (G_ENUM_CLASS (g_type_class_ref (PINOS_TYPE_SOURCE_OUTPUT_STATE)),
                          state);

  return val == NULL ? "invalid-state" : val->value_nick;
}

static void
source_output_fill_info (PinosSourceOutputInfo *info, GDBusProxy *proxy)
{
  GHashTable *changed = g_object_get_data (G_OBJECT (proxy), "pinos-changed-properties");

  info->id = proxy;
  info->output_path = g_dbus_proxy_get_object_path (proxy);

  info->change_mask = 0;
  SET_STRING ("Client", client_path, 0);
  SET_STRING ("Source", source_path, 1);
  SET_BYTES ("PossibleFormats", possible_formats, 2);
  SET_UINT32 ("State", state, 3, PINOS_SOURCE_OUTPUT_STATE_ERROR);
  SET_BYTES ("Format", format, 4);
  SET_PROPERTIES ("Properties", properties, 5);

  if (changed)
    g_hash_table_remove_all (changed);
}

static void
source_output_clear_info (PinosSourceOutputInfo *info)
{
  if (info->possible_formats)
    g_bytes_unref (info->possible_formats);
  if (info->properties)
    pinos_properties_free (info->properties);
}

/**
 * pinos_context_list_source_output_info:
 * @context: a connected #PinosContext
 * @flags: extra #PinosSourceOutputInfoFlags
 * @cb: a #PinosSourceOutputInfoCallback
 * @cancelable: a #GCancellable
 * @callback: a #GAsyncReadyCallback to call when the operation is finished
 * @user_data: user data passed to @cb
 *
 * Call @cb for each source-output.
 */
void
pinos_context_list_source_output_info (PinosContext *context,
                                       PinosSourceOutputInfoFlags flags,
                                       PinosSourceOutputInfoCallback cb,
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

  for (walk = priv->source_outputs; walk; walk = g_list_next (walk)) {
    GDBusProxy *proxy = walk->data;
    PinosSourceOutputInfo info;

    source_output_fill_info (&info, proxy);
    cb (context, &info, user_data);
    source_output_clear_info (&info);
  }

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

/**
 * pinos_context_get_source_output_info_by_id:
 * @context: a connected #PinosContext
 * @id: a source output id
 * @flags: extra #PinosSourceOutputInfoFlags
 * @cb: a #PinosSourceOutputInfoCallback
 * @cancelable: a #GCancellable
 * @callback: a #GAsyncReadyCallback to call when the operation is finished
 * @user_data: user data passed to @cb
 *
 * Call @cb for the source output with @id.
 */
void
pinos_context_get_source_output_info_by_id (PinosContext *context,
                                            gpointer id,
                                            PinosSourceOutputInfoFlags flags,
                                            PinosSourceOutputInfoCallback cb,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data)
{
  PinosSourceOutputInfo info;
  GDBusProxy *proxy;
  GTask *task;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (id != NULL);
  g_return_if_fail (cb != NULL);

  task = g_task_new (context, cancellable, callback, user_data);

  proxy = G_DBUS_PROXY (id);

  source_output_fill_info (&info, proxy);
  cb (context, &info, user_data);
  source_output_clear_info (&info);

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

