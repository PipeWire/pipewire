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

#ifndef __PIPEWIRE_RESOURCE_H__
#define __PIPEWIRE_RESOURCE_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PIPEWIRE_TYPE__Resource                            "PipeWire:Object:Resource"
#define PIPEWIRE_TYPE_RESOURCE_BASE                        PIPEWIRE_TYPE__Resource ":"

#include <spa/list.h>

#include <pipewire/client/sig.h>
#include <pipewire/server/core.h>

typedef void (*pw_destroy_t) (void *object);

struct pw_resource {
	struct pw_core *core;
	struct spa_list link;

	struct pw_client *client;

	uint32_t id;
	uint32_t type;
	void *object;
	pw_destroy_t destroy;

	const struct pw_interface *iface;
	const void *implementation;

	PW_SIGNAL(destroy_signal, (struct pw_listener * listener, struct pw_resource * resource));
};

struct pw_resource *
pw_resource_new(struct pw_client *client,
		uint32_t id, uint32_t type, void *object, pw_destroy_t destroy);

void
pw_resource_destroy(struct pw_resource *resource);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_RESOURCE_H__ */
