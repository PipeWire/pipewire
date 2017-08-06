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
#include <pipewire/introspect.h>
#include <pipewire/properties.h>
#include <pipewire/listener.h>
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

struct pw_client_events {
#define PW_VERSION_CLIENT_EVENTS	0
        uint32_t version;

	void (*destroy) (void *data);

	void (*free) (void *data);

	void (*info_changed) (void *data, struct pw_client_info *info);

	void (*resource_added) (void *data, struct pw_resource *resource);

	void (*resource_impl) (void *data, struct pw_resource *resource);

	void (*resource_removed) (void *data, struct pw_resource *resource);

	void (*busy_changed) (void *data, bool busy);
};

struct pw_client *
pw_client_new(struct pw_core *core,
	      struct pw_global *parent,
	      struct ucred *ucred,
	      struct pw_properties *properties,
	      size_t user_data_size);

void pw_client_destroy(struct pw_client *client);

struct pw_core *pw_client_get_core(struct pw_client *client);

struct pw_global *pw_client_get_global(struct pw_client *client);

const struct pw_properties *pw_client_get_properties(struct pw_client *client);

const struct ucred *pw_client_get_ucred(struct pw_client *client);

void *pw_client_get_user_data(struct pw_client *client);

void pw_client_add_listener(struct pw_client *client,
			    struct pw_listener *listener,
			    const struct pw_client_events *events,
			    void *data);


const struct pw_client_info *pw_client_get_info(struct pw_client *client);

void pw_client_update_properties(struct pw_client *client, const struct spa_dict *dict);

/** Mark the client busy. This can be used when an asynchronous operation is
  * started and no further processing is allowed to happen for the client */
void pw_client_set_busy(struct pw_client *client, bool busy);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_CLIENT_H__ */
