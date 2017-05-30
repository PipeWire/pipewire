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
#include <pipewire/client/utils.h>

/** \class pw_proxy
 *
 * \brief Represents an object on the client side.
 *
 * A pw_proxy acts as a client side proxy to an object existing in the
 * pipewire server. The proxy is responsible for converting interface functions
 * invoked by the client to PipeWire messages. Events will call the handlers
 * set in implementation.
 */
struct pw_proxy {
	/** the owner context of this proxy */
	struct pw_context *context;
	/** link in the context */
	struct spa_list link;

	uint32_t id;	/**< client side id */
	uint32_t type;	/**< object type id */

	const struct pw_interface *iface;	/**< methods/events marshal/demarshal functions */
	const void *implementation;		/**< event handler implementation */

	void *user_data;		/**< optional client user data */
        pw_destroy_t destroy;		/**< optional destroy function to clean up user_data */

	/** destroy_signal is emited when the proxy is destroyed */
	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_proxy *proxy));
};

struct pw_proxy *
pw_proxy_new(struct pw_context *context, uint32_t id, uint32_t type);

void pw_proxy_destroy(struct pw_proxy *proxy);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_PROXY_H__ */
