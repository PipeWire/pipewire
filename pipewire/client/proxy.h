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

#ifndef __PIPEWIRE_PROXY_H__
#define __PIPEWIRE_PROXY_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <pipewire/client/connection.h>
#include <pipewire/client/context.h>
#include <pipewire/client/type.h>

struct pw_proxy {
	struct pw_context *context;
	struct spa_list link;

	uint32_t id;
	uint32_t type;

	const struct pw_interface *iface;
	const void *implementation;

	void *user_data;

	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_proxy *proxy));
};

struct pw_proxy *
pw_proxy_new(struct pw_context *context, uint32_t id, uint32_t type);

void
pw_proxy_destroy(struct pw_proxy *proxy);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_PROXY_H__ */
