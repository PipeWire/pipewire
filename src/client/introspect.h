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

#ifndef __PINOS_INTROSPECT_H__
#define __PINOS_INTROSPECT_H__

#include <gio/gio.h>
#include <glib-object.h>

#include <client/context.h>

G_BEGIN_DECLS

/**
 * PinosSourceState:
 * @PINOS_SOURCE_STATE_ERROR: the source is in error
 * @PINOS_SOURCE_STATE_SUSPENDED: the source is suspended, the device might
 *                             be closed
 * @PINOS_SOURCE_STATE_INIT: the source is initializing, it opens the device
 *                        and gets the device capabilities
 * @PINOS_SOURCE_STATE_IDLE: the source is running but there is no active
 *                        source-output
 * @PINOS_SOURCE_STATE_RUNNING: the source is running.
 *
 * The different source states
 */
typedef enum {
  PINOS_SOURCE_STATE_ERROR = 0,
  PINOS_SOURCE_STATE_SUSPENDED = 1,
  PINOS_SOURCE_STATE_INIT = 2,
  PINOS_SOURCE_STATE_IDLE = 3,
  PINOS_SOURCE_STATE_RUNNING = 4,
} PinosSourceState;

/**
 * PinosSourceInfo:
 * @name: the name of the source
 * @properties: the properties of the source
 * @state: the current state of the source
 * @formats: the supported formats
 *
 * The source information
 */
typedef struct {
  const char *name;
  GVariant *properties;
  PinosSourceState state;
  GBytes *formats;
} PinosSourceInfo;

/**
 * PinosSourceInfoFlags:
 * @PINOS_SOURCE_INFO_FLAGS_NONE: no flags
 * @PINOS_SOURCE_INFO_FLAGS_FORMATS: include formats
 *
 * Extra flags to pass to pinos_context_get_source_info_list.
 */
typedef enum {
  PINOS_SOURCE_INFO_FLAGS_NONE            = 0,
  PINOS_SOURCE_INFO_FLAGS_FORMATS         = (1 << 0)
} PinosSourceInfoFlags;

typedef gboolean (*PinosSourceInfoCallback)  (PinosContext *c, const PinosSourceInfo *info, gpointer userdata);

void            pinos_context_list_source_info     (PinosContext *context,
                                                    PinosSourceInfoFlags flags,
                                                    PinosSourceInfoCallback cb,
                                                    GCancellable *cancellable,
                                                    gpointer user_data);

G_END_DECLS

#endif /* __PINOS_INTROSPECT_H__ */

