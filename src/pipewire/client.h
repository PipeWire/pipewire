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

#ifndef __PIPEWIRE_CLIENT_H__
#define __PIPEWIRE_CLIENT_H__

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <sys/socket.h>

#include <spa/utils/hook.h>

/** \class pw_client
 *
 * \brief PipeWire client object class.
 *
 * The client object represents a client connection with the PipeWire
 * server.
 *
 * Each client has its own list of resources it is bound to along with
 * a mapping between the client types and server types.
 */
struct pw_client;

#include <pipewire/core.h>
#include <pipewire/global.h>
#include <pipewire/introspect.h>
#include <pipewire/properties.h>
#include <pipewire/resource.h>

#define PW_TYPE__Client           PW_TYPE_OBJECT_BASE "Client"
#define PW_TYPE_CLIENT_BASE       PW_TYPE__Client ":"

/** \page page_client Client
 *
 * \section sec_page_client_overview Overview
 *
 * The \ref pw_client object is created by a protocol implementation when
 * a new client connects.
 *
 * The client is used to keep track of all resources belonging to one
 * connection with the PipeWire server.
 *
 * \section sec_page_client_credentials Credentials
 *
 * The client object will have its credentials filled in by the protocol.
 * This information is used to check if a resource or action is available
 * for this client. See also \ref page_access
 *
 * \section sec_page_client_types Types
 *
 * The client and server maintain a mapping between the client and server
 * types. All type ids that are in messages exchanged between the client
 * and server will automatically be remapped. See also \ref page_types.
 *
 * \section sec_page_client_resources Resources
 *
 * When a client binds to core global object, a resource is made for this
 * binding and a unique id is assigned to the resources. The client and
 * server will use this id as the destination when exchanging messages.
 * See also \ref page_resource
 */

/** The events that a client can emit */
struct pw_client_events {
#define PW_VERSION_CLIENT_EVENTS	0
        uint32_t version;

	/** emited when the client is destroyed */
	void (*destroy) (void *data);

	/** emited right before the client is freed */
	void (*free) (void *data);

	/** emited when the client info changed */
	void (*info_changed) (void *data, struct pw_client_info *info);

	/** emited when a new resource is added for client */
	void (*resource_added) (void *data, struct pw_resource *resource);

	/** emited when an implementation is set on a resource. This can
	 * be used to override the implementation */
	void (*resource_impl) (void *data, struct pw_resource *resource);

	/** emited when a resource is removed */
	void (*resource_removed) (void *data, struct pw_resource *resource);

	/** emited when the client becomes busy processing an asynchronous
	 * message. In the busy state no messages should be processed.
	 * Processing should resume when the client becomes not busy */
	void (*busy_changed) (void *data, bool busy);
};

/** The name of the protocol used by the client, set by the protocol */
#define PW_CLIENT_PROP_PROTOCOL		"pipewire.protocol"

#define PW_CLIENT_PROP_UCRED_PID	"pipewire.ucred.pid"	/**< Client pid, set by protocol */
#define PW_CLIENT_PROP_UCRED_UID	"pipewire.ucred.uid"	/**< Client uid, set by protocol*/
#define PW_CLIENT_PROP_UCRED_GID	"pipewire.ucred.gid"	/**< client gid, set by protocol*/

/** Create a new client. This is mainly used by protocols. */
struct pw_client *
pw_client_new(struct pw_core *core,		/**< the core object */
	      struct ucred *ucred,		/**< optional ucred */
	      struct pw_properties *properties,	/**< client properties */
	      size_t user_data_size		/**< extra user data size */);

/** Destroy a previously created client */
void pw_client_destroy(struct pw_client *client);

/** Finish configuration and register a client */
int pw_client_register(struct pw_client *client,	/**< the client to register */
		       struct pw_client *owner,	/**< optional owner */
		       struct pw_global *parent,	/**< the client parent */
		       struct pw_properties *properties/**< extra properties */);

/** Get the client user data */
void *pw_client_get_user_data(struct pw_client *client);

/** Get the client information */
const struct pw_client_info *pw_client_get_info(struct pw_client *client);

/** Update the client properties */
int pw_client_update_properties(struct pw_client *client, const struct spa_dict *dict);

/** Update the client permissions */
int pw_client_update_permissions(struct pw_client *client, const struct spa_dict *dict);

/** Get the client properties */
const struct pw_properties *pw_client_get_properties(struct pw_client *client);

/** Get the core used to create this client */
struct pw_core *pw_client_get_core(struct pw_client *client);

/** Get the client core resource */
struct pw_resource *pw_client_get_core_resource(struct pw_client *client);

/** Get a resource with the given id */
struct pw_resource *pw_client_find_resource(struct pw_client *client, uint32_t id);

/** Get the global associated with this client */
struct pw_global *pw_client_get_global(struct pw_client *client);

/** Get the ucred from a client or NULL when not specified/valid */
const struct ucred *pw_client_get_ucred(struct pw_client *client);

/** listen to events from this client */
void pw_client_add_listener(struct pw_client *client,
			    struct spa_hook *listener,
			    const struct pw_client_events *events,
			    void *data);


/** Mark the client busy. This can be used when an asynchronous operation is
  * started and no further processing is allowed to happen for the client */
void pw_client_set_busy(struct pw_client *client, bool busy);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_CLIENT_H__ */
