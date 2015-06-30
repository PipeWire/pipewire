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

#include "client/pv-context.h"
#include "client/pv-enumtypes.h"
#include "client/pv-subscribe.h"

#include "client/pv-private.h"

/**
 * pv_context_list_source_info:
 * @context: a connected #PvContext
 * @flags: extra #PvSourceInfoFlags
 * @cb: a #PvSourceInfoCallback
 * @cancelable: a #GCancellable
 * @user_data: user data passed to @cb
 *
 * Call @cb for each source.
 */
void
pv_context_list_source_info (PvContext *context,
                             PvSourceInfoFlags flags,
                             PvSourceInfoCallback cb,
                             GCancellable *cancellable,
                             gpointer user_data)
{
  GList *walk;
  PvContextPrivate *priv = context->priv;

  for (walk = priv->sources; walk; walk = g_list_next (walk)) {
    GDBusProxy *proxy = walk->data;
    PvSourceInfo info;

    info.name = "gst";

    cb (context, &info, user_data);
  }
  cb (context, NULL, user_data);
}
