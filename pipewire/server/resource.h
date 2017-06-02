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

/** \page page_resource Resource
 *
 * \section sec_page_resource Overview
 *
 * Resources represent objects owned by a \ref pw_client. They are
 * the result of binding to a global resource or by calling API that
 * creates client owned objects.
 *
 * The client usually has a proxy object associated with the resource
 * that it can use to communicate with the resource. See \ref page_proxy.
 *
 * Resources are destroyed when the client or the bound object is
 * destroyed.
 *
 */
/** \class pw_resource
 *
 * \brief Client owned objects
 *
 * Resources are objects owned by a client and are destroyed when the
 * client disappears.
 *
 * See also \ref page_resource
 */
struct pw_resource {
	struct pw_core *core;		/**< the core object */
	struct spa_list link;		/**< link in object resource_list */

	struct pw_client *client;	/**< owner client */

	uint32_t id;			/**< per client unique id, index in client objects */
	uint32_t type;			/**< type id of the object */
	void *object;			/**< pointer to the object */
	pw_destroy_t destroy;		/**< function to clean up the object */

	const struct pw_interface *iface;	/**< protocol specific interface functions */
	const void *implementation;		/**< implementation */

	/** Emited when the resource is destroyed */
	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_resource *resource));
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
