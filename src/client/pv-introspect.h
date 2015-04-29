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

#ifndef __PV_INTROSPECT_H__
#define __PV_INTROSPECT_H__

#include <gio/gio.h>
#include <glib-object.h>

#include <client/pv-context.h>

G_BEGIN_DECLS

/**
 * PvSourceInfo:
 * @name: the name of the source
 * @properties: the properties of the source
 * @state: the current state of the source
 *
 * The source information
 */
typedef struct {
  const char *name;
  GVariant *properties;
  PvSourceState state;
  GVariant *capabilities;
} PvSourceInfo;

/**
 * PvSourceInfoFlags:
 * @PV_SOURCE_INFO_FLAGS_NONE: no flags
 * @PV_SOURCE_INFO_FLAGS_CAPABILITIES: include capabilities
 *
 * Extra flags to pass to pv_context_get_source_info_list.
 */
typedef enum {
  PV_SOURCE_INFO_FLAGS_NONE            = 0,
  PV_SOURCE_INFO_FLAGS_CAPABILITIES    = (1 << 0)
} PvSourceInfoFlags;

typedef gboolean (*PvSourceInfoCallback)  (PvContext *c, const PvSourceInfo *info, gpointer userdata);

void            pv_context_list_source_info        (PvContext *context,
                                                    PvSourceInfoFlags flags,
                                                    PvSourceInfoCallback cb,
                                                    GCancellable *cancellable,
                                                    gpointer user_data);

G_END_DECLS

#endif /* __PV_INTROSPECT_H__ */

