/* PipeWire
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PIPEWIRE_PROTOCOL_H__
#define __PIPEWIRE_PROTOCOL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/defs.h>
#include <spa/list.h>

#include <pipewire/type.h>
#include <pipewire/utils.h>
#include <pipewire/core.h>
#include <pipewire/properties.h>

#define PW_TYPE__Protocol               "PipeWire:Protocol"
#define PW_TYPE_PROTOCOL_BASE           PW_TYPE__Protocol ":"

struct pw_protocol_connection {
	struct spa_list link;		/**< link in protocol connection_list */
	struct pw_protocol *protocol;	/**< the owner protocol */

	struct pw_remote *remote;	/**< the associated remote */

	int (*connect) (struct pw_protocol_connection *conn);
	int (*connect_fd) (struct pw_protocol_connection *conn, int fd);
	void (*disconnect) (struct pw_protocol_connection *conn);
	void (*destroy) (struct pw_protocol_connection *conn);
};

#define pw_protocol_connection_connect(c)	((c)->connect(c))
#define pw_protocol_connection_connect_fd(c,fd)	((c)->connect_fd(c,fd))
#define pw_protocol_connection_disconnect(c)	((c)->disconnect(c))
#define pw_protocol_connection_destroy(c)	((c)->destroy(c))

struct pw_protocol_listener {
	struct spa_list link;		/**< link in protocol listener_list */
	struct pw_protocol *protocol;	/**< the owner protocol */

	struct spa_list client_list;	/**< list of client of this protocol */

	void (*destroy) (struct pw_protocol_listener *listen);
};

#define pw_protocol_listener_destroy(l)	((l)->destroy(l))

struct pw_protocol_marshal {
        const char *type;               /**< interface type */
	uint32_t version;               /**< version */
	uint32_t n_methods;             /**< number of methods in the interface */
	const void *method_marshal;
	const void *method_demarshal;
        uint32_t n_events;              /**< number of events in the interface */
	const void *event_marshal;
	const void *event_demarshal;
};

struct pw_protocol_implementaton {
#define PW_VERSION_PROTOCOL_IMPLEMENTATION	0
	uint32_t version;
	struct pw_protocol_connection * (*new_connection) (struct pw_protocol *protocol,
							   struct pw_remote *remote,
							   struct pw_properties *properties);
	struct pw_protocol_listener * (*add_listener) (struct pw_protocol *protocol,
						       struct pw_core *core,
						       struct pw_properties *properties);
};

struct pw_protocol {
	struct spa_list link;			/**< link in core protocol_list */
	struct pw_core *core;			/**< core for this protocol */

	char *name;				/**< type name of the protocol */

	struct spa_list marshal_list;		/**< list of marshallers for supported interfaces */
	struct spa_list connection_list;	/**< list of current connections */
	struct spa_list listener_list;		/**< list of current listeners */

	const struct pw_protocol_implementaton *implementation;	/**< implementation of the protocol */

	const void *extension;	/**< extension API */

	void *user_data;	/**< user data for the implementation */
        pw_destroy_t destroy;	/**< function to clean up the object */
};

#define pw_protocol_new_connection(p,...)	((p)->implementation->new_connection(p,__VA_ARGS__))
#define pw_protocol_add_listener(p,...)		((p)->implementation->add_listener(p,__VA_ARGS__))

#define pw_protocol_ext(p,type,method,...)	(((type*)(p)->extension)->method( __VA_ARGS__))

struct pw_protocol *pw_protocol_new(struct pw_core *core, const char *name, size_t user_data_size);

void *pw_protocol_get_user_data(struct pw_protocol *protocol);

/** \class pw_protocol
 *
 * \brief Manages protocols and their implementation
 */
void pw_protocol_add_marshal(struct pw_protocol *protocol,
			      const struct pw_protocol_marshal *marshal);

const struct pw_protocol_marshal *
pw_protocol_get_marshal(struct pw_protocol *protocol, uint32_t type);

struct pw_protocol * pw_core_find_protocol(struct pw_core *core, const char *name);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PIPEWIRE_PROTOCOL_H__ */
