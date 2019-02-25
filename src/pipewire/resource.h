/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef PIPEWIRE_RESOURCE_H
#define PIPEWIRE_RESOURCE_H

#ifdef __cplusplus
extern "C" {
#endif

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

	/** a reply to a sync event completed */
        void (*done) (void *data, uint32_t seq);

	/** an error occured on the resource */
        void (*error) (void *data, int res, const char *message);
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

/** Generate an sync method for a resource. This will generate a done event
 * with the same \a sequence number in the return value. */
int pw_resource_sync(struct pw_resource *resource, uint32_t seq);

/** Generate an error for a resource */
int pw_resource_error(struct pw_resource *resource, int result, const char *error, ...);

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

#endif /* PIPEWIRE_RESOURCE_H */
