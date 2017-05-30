/* PipeWire
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

#ifndef __PIPEWIRE_H__
#define __PIPEWIRE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <pipewire/client/context.h>
#include <pipewire/client/introspect.h>
#include <pipewire/client/log.h>
#include <pipewire/client/loop.h>
#include <pipewire/client/mem.h>
#include <pipewire/client/thread-mainloop.h>
#include <pipewire/client/properties.h>
#include <pipewire/client/stream.h>
#include <pipewire/client/subscribe.h>
#include <pipewire/client/utils.h>

#include <spa/type-map.h>

/** \class pw_pipewire
 *
 * \brief PipeWire initalization and infrasctructure functions
 */
void
pw_init(int *argc, char **argv[]);

bool
pw_debug_is_category_enabled(const char *name);

const char *
pw_get_application_name(void);

const char *
pw_get_prgname(void);

const char *
pw_get_user_name(void);

const char *
pw_get_host_name(void);

char *
pw_get_client_name(void);

void
pw_fill_context_properties(struct pw_properties *properties);

void
pw_fill_stream_properties(struct pw_properties *properties);

enum pw_direction
pw_direction_reverse(enum pw_direction direction);

struct spa_type_map *
pw_type_map_get_default(void);

#ifdef __cplusplus
}
#endif
#endif /* __PIPEWIRE_H__ */
