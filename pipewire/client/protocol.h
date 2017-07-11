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

#include <pipewire/client/subscribe.h>
#include <pipewire/client/type.h>
#include <pipewire/client/properties.h>

#define PW_TYPE_PROTOCOL__Native	PW_TYPE_PROTOCOL_BASE "Native"

struct pw_protocol_connection {
	struct spa_list link;
	struct pw_remote *remote;

	int (*connect) (struct pw_protocol_connection *conn);
	int (*connect_fd) (struct pw_protocol_connection *conn, int fd);
	int (*disconnect) (struct pw_protocol_connection *conn);
	int (*destroy) (struct pw_protocol_connection *conn);
};

struct pw_protocol_listener {
	struct spa_list link;
	struct pw_core *core;

	int (*destroy) (struct pw_protocol_listener *listen);
};

struct pw_protocol_iface {
	struct spa_list link;
	const struct pw_interface *client_iface;
	const struct pw_interface *server_iface;
};

struct pw_protocol {
	struct spa_list link;
	const char *name;
	struct spa_list iface_list;
	struct spa_list connection_list;
	struct spa_list listener_list;

	struct pw_protocol_connection * (*new_connection) (struct pw_protocol *protocol,
							   struct pw_remote *remote,
							   struct pw_properties *properties);
	struct pw_protocol_listener * (*add_listener) (struct pw_protocol *protocol,
						       struct pw_core *core,
						       struct pw_properties *properties);
	void *protocol_private;
};

struct pw_protocol *pw_protocol_get(const char *name);

/** \class pw_protocol
 *
 * \brief Manages protocols and their implementation
 */
void
pw_protocol_add_interfaces(struct pw_protocol *protocol,
			   const struct pw_interface *client_iface,
			   const struct pw_interface *server_iface);

const struct pw_interface *
pw_protocol_get_interface(struct pw_protocol *protocol,
			  const char *type,
			  bool server);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PIPEWIRE_PROTOCOL_H__ */
