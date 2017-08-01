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

#define PW_TYPE__Resource                            "PipeWire:Object:Resource"
#define PW_TYPE_RESOURCE_BASE                        PW_TYPE__Resource ":"

#include <spa/list.h>

#include <pipewire/sig.h>
#include <pipewire/utils.h>
#include <pipewire/core.h>

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
	uint32_t permissions;		/**< resource permissions */
	uint32_t type;			/**< type of the client interface */
	uint32_t version;		/**< version of the client interface */

	void *object;
	const void *implementation;

	pw_destroy_t destroy;		/**< function to clean up the object */

        const struct pw_protocol_marshal *marshal;

	/** Emited when the resource is destroyed */
	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_resource *resource));

	void *access_private;		/**< private data for access control */
	void *user_data;		/**< extra user data */
};

/** Make a new resource for client */
struct pw_resource *
pw_resource_new(struct pw_client *client,	/**< the client owning the resource */
		uint32_t id,			/**< the remote per client id */
		uint32_t permissions,		/**< permissions on this resource */
		uint32_t type,			/**< interface of the resource */
		uint32_t version,		/**< requested interface version */
		size_t user_data_size,		/**< extra user data size */
		pw_destroy_t destroy		/**< destroy function for user data */);


int
pw_resource_set_implementation(struct pw_resource *resource,
			       void *object, const void *implementation);

void
pw_resource_destroy(struct pw_resource *resource);

#define pw_resource_do(r,type,method,...)	((type*) r->implementation)->method(r, __VA_ARGS__)
#define pw_resource_do_na(r,type,method)	((type*) r->implementation)->method(r)
#define pw_resource_notify(r,type,event,...)	((type*) r->marshal->event_marshal)->event(r, __VA_ARGS__)
#define pw_resource_notify_na(r,type,event)	((type*) r->marshal->event_marshal)->event(r)

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_RESOURCE_H__ */
