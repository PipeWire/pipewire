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

static void
daemon_fill_info (PinosDaemonInfo *info, GDBusProxy *proxy)
{
  GVariant *variant;

  info->id = proxy;

  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "UserName"))) {
    info->user_name = g_variant_get_string (variant, NULL);
    g_variant_unref (variant);
  } else {
    info->user_name = "Unknown";
  }
  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "HostName"))) {
    info->host_name = g_variant_get_string (variant, NULL);
    g_variant_unref (variant);
  } else {
    info->host_name = "Unknown";
  }
  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Version"))) {
    info->version = g_variant_get_string (variant, NULL);
    g_variant_unref (variant);
  } else {
    info->version = "Unknown";
  }
  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Name"))) {
    info->name = g_variant_get_string (variant, NULL);
    g_variant_unref (variant);
  } else {
    info->name = "Unknown";
  }
  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Cookie"))) {
    info->cookie = g_variant_get_uint32 (variant);
    g_variant_unref (variant);
  } else {
    info->cookie = 0;
  }
  info->properties = pinos_properties_from_variant (
      g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Properties"));
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
 * @user_data: user data passed to @cb
 *
 * Get the information of the daemon @context is connected to.
 */
void
pinos_context_get_daemon_info (PinosContext *context,
                               PinosDaemonInfoFlags flags,
                               PinosDaemonInfoCallback cb,
                               GCancellable *cancellable,
                               gpointer user_data)
{
  PinosDaemonInfo info;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (cb != NULL);

  daemon_fill_info (&info, context->priv->daemon);
  cb (context, &info, user_data);
  daemon_clear_info (&info);
  cb (context, NULL, user_data);
}

static void
client_fill_info (PinosClientInfo *info, GDBusProxy *proxy)
{
  GVariant *variant;

  info->id = proxy;

  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Name"))) {
    info->name = g_variant_get_string (variant, NULL);
    g_variant_unref (variant);
  } else {
    info->name = "Unknown";
  }

  info->properties = pinos_properties_from_variant (
      g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Properties"));
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
 * @user_data: user data passed to @cb
 *
 * Call @cb for each client. @cb will be called with NULL when there
 * are no more clients to list.
 */
void
pinos_context_list_client_info (PinosContext *context,
                                PinosClientInfoFlags flags,
                                PinosClientInfoCallback cb,
                                GCancellable *cancellable,
                                gpointer user_data)
{
  GList *walk;
  PinosContextPrivate *priv = context->priv;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (cb != NULL);

  for (walk = priv->clients; walk; walk = g_list_next (walk)) {
    GDBusProxy *proxy = walk->data;
    PinosClientInfo info;

    client_fill_info (&info, proxy);
    cb (context, &info, user_data);
    client_clear_info (&info);
  }
  cb (context, NULL, user_data);
}

/**
 * pinos_context_get_client_info_by_id:
 * @context: a connected #PinosContext
 * @id: a client id
 * @flags: extra #PinosClientInfoFlags
 * @cb: a #PinosClientInfoCallback
 * @cancelable: a #GCancellable
 * @user_data: user data passed to @cb
 *
 * Call @cb for the client with @id. Then @cb will be called with NULL.
 */
void
pinos_context_get_client_info_by_id (PinosContext *context,
                                     gpointer id,
                                     PinosClientInfoFlags flags,
                                     PinosClientInfoCallback cb,
                                     GCancellable *cancellable,
                                     gpointer user_data)
{
  PinosClientInfo info;
  GDBusProxy *proxy;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (id != NULL);
  g_return_if_fail (cb != NULL);

  proxy = G_DBUS_PROXY (id);

  client_fill_info (&info, proxy);
  cb (context, &info, user_data);
  client_clear_info (&info);
  cb (context, NULL, user_data);
}

static void
source_fill_info (PinosSourceInfo *info, GDBusProxy *proxy)
{
  GVariant *variant;

  info->id = proxy;

  info->source_path = g_dbus_proxy_get_object_path (proxy);

  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Name"))) {
    info->name = g_variant_get_string (variant, NULL);
    g_variant_unref (variant);
  } else {
    info->name = "Unknown";
  }

  info->properties = pinos_properties_from_variant (
      g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Properties"));

  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "State"))) {
    info->state = g_variant_get_uint32 (variant);
    g_variant_unref (variant);
  } else {
    info->state = PINOS_SOURCE_STATE_ERROR;
  }

  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "PossibleFormats"))) {
    gsize len;
    gchar *formats = g_variant_dup_string (variant, &len);
    info->possible_formats = g_bytes_new_take (formats, len + 1);
    g_variant_unref (variant);
  } else {
    info->possible_formats = NULL;
  }
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
 * @user_data: user data passed to @cb
 *
 * Call @cb for each source. @cb will be called with NULL when there
 * are no more sources to list.
 */
void
pinos_context_list_source_info (PinosContext            *context,
                                PinosSourceInfoFlags     flags,
                                PinosSourceInfoCallback  cb,
                                GCancellable            *cancellable,
                                gpointer                 user_data)
{
  GList *walk;
  PinosContextPrivate *priv = context->priv;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (cb != NULL);

  for (walk = priv->sources; walk; walk = g_list_next (walk)) {
    GDBusProxy *proxy = walk->data;
    PinosSourceInfo info;

    source_fill_info (&info, proxy);
    cb (context, &info, user_data);
    source_clear_info (&info);
  }
  cb (context, NULL, user_data);
}

/**
 * pinos_context_get_source_info_by_id:
 * @context: a connected #PinosContext
 * @id: a source id
 * @flags: extra #PinosSourceInfoFlags
 * @cb: a #PinosSourceInfoCallback
 * @cancelable: a #GCancellable
 * @user_data: user data passed to @cb
 *
 * Call @cb for the source with @id. Then @cb will be called with NULL.
 */
void
pinos_context_get_source_info_by_id (PinosContext *context,
                                     gpointer id,
                                     PinosSourceInfoFlags flags,
                                     PinosSourceInfoCallback cb,
                                     GCancellable *cancellable,
                                     gpointer user_data)
{
  PinosSourceInfo info;
  GDBusProxy *proxy;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (id != NULL);
  g_return_if_fail (cb != NULL);

  proxy = G_DBUS_PROXY (id);

  source_fill_info (&info, proxy);
  cb (context, &info, user_data);
  source_clear_info (&info);
  cb (context, NULL, user_data);
}

static void
source_output_fill_info (PinosSourceOutputInfo *info, GDBusProxy *proxy)
{
  GVariant *variant;

  info->id = proxy;

  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Client"))) {
    info->client_path = g_variant_get_string (variant, NULL);
    g_variant_unref (variant);
  } else {
    info->client_path = "Unknown";
  }
  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Source"))) {
    info->source_path = g_variant_get_string (variant, NULL);
    g_variant_unref (variant);
  } else {
    info->source_path = "Unknown";
  }
  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "PossibleFormats"))) {
    gsize len;
    gchar *formats = g_variant_dup_string (variant, &len);
    info->possible_formats = g_bytes_new_take (formats, len + 1);
    g_variant_unref (variant);
  } else {
    info->possible_formats = NULL;
  }
  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "State"))) {
    info->state = g_variant_get_uint32 (variant);
    g_variant_unref (variant);
  } else {
    info->state = PINOS_SOURCE_OUTPUT_STATE_ERROR;
  }
  if ((variant = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Format"))) {
    gsize len;
    gchar *format = g_variant_dup_string (variant, &len);
    info->format = g_bytes_new_take (format, len + 1);
    g_variant_unref (variant);
  } else {
    info->format = NULL;
  }
  info->properties = pinos_properties_from_variant (
      g_dbus_proxy_get_cached_property (G_DBUS_PROXY (proxy), "Properties"));

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
 * @user_data: user data passed to @cb
 *
 * Call @cb for each source-output. @cb will be called with NULL when there
 * are no more outputs to list.
 */
void
pinos_context_list_source_output_info (PinosContext *context,
                                       PinosSourceOutputInfoFlags flags,
                                       PinosSourceOutputInfoCallback cb,
                                       GCancellable *cancellable,
                                       gpointer user_data)
{
  GList *walk;
  PinosContextPrivate *priv = context->priv;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (cb != NULL);

  for (walk = priv->source_outputs; walk; walk = g_list_next (walk)) {
    GDBusProxy *proxy = walk->data;
    PinosSourceOutputInfo info;

    source_output_fill_info (&info, proxy);
    cb (context, &info, user_data);
    source_output_clear_info (&info);
  }
  cb (context, NULL, user_data);
}

/**
 * pinos_context_get_source_output_info_by_id:
 * @context: a connected #PinosContext
 * @id: a source output id
 * @flags: extra #PinosSourceOutputInfoFlags
 * @cb: a #PinosSourceOutputInfoCallback
 * @cancelable: a #GCancellable
 * @user_data: user data passed to @cb
 *
 * Call @cb for the source output with @id. Then @cb will be called with NULL.
 */
void
pinos_context_get_source_output_info_by_id (PinosContext *context,
                                            gpointer id,
                                            PinosSourceOutputInfoFlags flags,
                                            PinosSourceOutputInfoCallback cb,
                                            GCancellable *cancellable,
                                            gpointer user_data)
{
  PinosSourceOutputInfo info;
  GDBusProxy *proxy;

  g_return_if_fail (PINOS_IS_CONTEXT (context));
  g_return_if_fail (id != NULL);
  g_return_if_fail (cb != NULL);

  proxy = G_DBUS_PROXY (id);

  source_output_fill_info (&info, proxy);
  cb (context, &info, user_data);
  source_output_clear_info (&info);
  cb (context, NULL, user_data);
}

