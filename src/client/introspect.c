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

#include "client/pinos.h"

#include "client/context.h"
#include "client/enumtypes.h"
#include "client/subscribe.h"

#include "client/private.h"

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

  for (walk = priv->sources; walk; walk = g_list_next (walk)) {
    GDBusProxy *proxy = walk->data;
    PinosSourceInfo info;

    info.id = proxy;
    info.name = "gst";

    cb (context, &info, user_data);
  }
  cb (context, NULL, user_data);
}

/**
 * pinos_context_get_source_info:
 * @context: a connected #PinosContext
 * @id: a source id
 * @flags: extra #PinosSourceInfoFlags
 * @cb: a #PinosSourceInfoCallback
 * @cancelable: a #GCancellable
 * @user_data: user data passed to @cb
 *
 * Call @cb for each source. @cb will be called with NULL when there
 * are no more sources to list.
 */
void
pinos_context_get_source_info_by_id (PinosContext *context,
                                     gpointer id,
                                     PinosSourceInfoFlags flags,
                                     PinosSourceInfoCallback cb,
                                     GCancellable *cancellable,
                                     gpointer user_data)
{
}
