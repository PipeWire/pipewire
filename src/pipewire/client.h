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

#include <pipewire/core.h>
#include <pipewire/introspect.h>
#include <pipewire/properties.h>
#include <pipewire/sig.h>
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
struct pw_client {
	struct pw_core *core;		/**< core object */
	struct spa_list link;		/**< link in core object client list */
	struct pw_global *global;	/**< global object created for this client */

	struct pw_properties *properties;	/**< Client properties */
	/** Emited when the properties changed */
	PW_SIGNAL(properties_changed, (struct pw_listener *listener, struct pw_client *client));

	struct pw_client_info info;	/**< client info */
	bool ucred_valid;		/**< if the ucred member is valid */
	struct ucred ucred;		/**< ucred information */

	struct pw_resource *core_resource;	/**< core resource object */

	struct pw_map objects;		/**< list of resource objects */
	uint32_t n_types;		/**< number of client types */
	struct pw_map types;		/**< map of client types */

	struct spa_list resource_list;	/**< The list of resources of this client */
	/** Emited when a resource is added */
	PW_SIGNAL(resource_added, (struct pw_listener *listener,
				   struct pw_client *client, struct pw_resource *resource));
	/** Emited when a resource implementation is set */
	PW_SIGNAL(resource_impl, (struct pw_listener *listener,
				  struct pw_client *client, struct pw_resource *resource));
	/** Emited when a resource is removed */
	PW_SIGNAL(resource_removed, (struct pw_listener *listener,
				     struct pw_client *client, struct pw_resource *resource));

	bool busy;
	/** Emited when the client starts/stops an async operation that should
	 * block/resume all methods for this client */
	PW_SIGNAL(busy_changed, (struct pw_listener *listener, struct pw_client *client));

	/** Emited when the client is destroyed */
	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_client *client));

	struct pw_protocol *protocol;	/**< protocol in use */
	struct spa_list protocol_link;	/**< link in the protocol client_list */
	void *protocol_private;		/**< private data for the protocol */

	void *user_data;		/**< extra user data */
	pw_destroy_t destroy;		/**< function to clean up the object */
};

struct pw_client *
pw_client_new(struct pw_core *core,
	      struct pw_global *parent,
	      struct ucred *ucred,
	      struct pw_properties *properties,
	      size_t user_data_size);

void
pw_client_destroy(struct pw_client *client);

void
pw_client_update_properties(struct pw_client *client, const struct spa_dict *dict);

void pw_client_set_busy(struct pw_client *client, bool busy);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_CLIENT_H__ */
