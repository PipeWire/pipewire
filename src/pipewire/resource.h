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

#include <spa/utils/hook.h>

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
struct pw_resource;

#include <pipewire/client.h>

/** Resource events */
struct pw_resource_events {
#define PW_VERSION_RESOURCE_EVENTS	0
	uint32_t version;

	/** The resource is destroyed */
	void (*destroy) (void *data);
};

/** Make a new resource for client */
struct pw_resource *
pw_resource_new(struct pw_client *client,	/**< the client owning the resource */
		uint32_t id,			/**< the remote per client id */
		uint32_t permissions,		/**< permissions on this resource */
		uint32_t type,			/**< interface of the resource */
		uint32_t version,		/**< requested interface version */
		size_t user_data_size		/**< extra user data size */);

/** Destroy a resource */
void pw_resource_destroy(struct pw_resource *resource);

/** Get the client owning this resource */
struct pw_client *pw_resource_get_client(struct pw_resource *resource);

/** Get the unique id of this resource */
uint32_t pw_resource_get_id(struct pw_resource *resource);

/** Get the permissions of this resource */
uint32_t pw_resource_get_permissions(struct pw_resource *resource);

/** Get the type of this resource */
uint32_t pw_resource_get_type(struct pw_resource *resource);

/** Get the protocol used for this resource */
struct pw_protocol *pw_resource_get_protocol(struct pw_resource *resource);

/** Get the user data for the resource, the size was given in \ref pw_resource_new */
void *pw_resource_get_user_data(struct pw_resource *resource);

/** Add an event listener */
void pw_resource_add_listener(struct pw_resource *resource,
			      struct spa_hook *listener,
			      const struct pw_resource_events *events,
			      void *data);

/** Set the resource implementation. */
void pw_resource_set_implementation(struct pw_resource *resource,
				    const void *implementation,
				    void *data);

/** Override the implementation of a resource. */
void pw_resource_add_override(struct pw_resource *resource,
			      struct spa_hook *listener,
			      const void *implementation,
			      void *data);

/** Generate an error for a resource */
void pw_resource_error(struct pw_resource *resource, int result, const char *error);

/** Get the implementation list of a resource */
struct spa_hook_list *pw_resource_get_implementation(struct pw_resource *resource);

/** Get the marshal functions for the resource */
const struct pw_protocol_marshal *pw_resource_get_marshal(struct pw_resource *resource);

#define pw_resource_do(r,type,method,v,...)		\
	spa_hook_list_call_once(pw_resource_get_implementation(r),type,method,v,## __VA_ARGS__)

#define pw_resource_do_parent(r,l,type,method,...)	\
	spa_hook_list_call_once_start(pw_resource_get_implementation(r),l,type,method,v,## __VA_ARGS__)

#define pw_resource_notify(r,type,event,...)		\
	((type*) pw_resource_get_marshal(r)->event_marshal)->event(r, ## __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_RESOURCE_H__ */
