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

#ifndef __PIPEWIRE_PROTOCOL_NATIVE_CONNECTION_H__
#define __PIPEWIRE_PROTOCOL_NATIVE_CONNECTION_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/utils/hook.h>

struct pw_protocol_native_connection_events {
#define PW_VERSION_PROTOCOL_NATIVE_CONNECTION_EVENTS	0
	uint32_t version;

	void (*destroy) (void *data);

	void (*error) (void *data, int error);

	void (*need_flush) (void *data);
};

/** \class pw_protocol_native_connection
 *
 * \brief Manages the connection between client and server
 *
 * The \ref pw_protocol_native_connection handles the connection between client
 * and server on a given socket.
 */
struct pw_protocol_native_connection {
	int fd;	/**< the socket */

	struct spa_hook_list listener_list;
};

static inline void
pw_protocol_native_connection_add_listener(struct pw_protocol_native_connection *conn,
					   struct spa_hook *listener,
					   const struct pw_protocol_native_connection_events *events,
					   void *data)
{
	spa_hook_list_append(&conn->listener_list, listener, events, data);
}

struct pw_protocol_native_connection *
pw_protocol_native_connection_new(struct pw_core *core, int fd);

void
pw_protocol_native_connection_destroy(struct pw_protocol_native_connection *conn);

bool
pw_protocol_native_connection_get_next(struct pw_protocol_native_connection *conn,
				       uint8_t *opcode,
				       uint32_t *dest_id,
				       void **data, uint32_t *size);

uint32_t pw_protocol_native_connection_add_fd(struct pw_protocol_native_connection *conn, int fd);

int pw_protocol_native_connection_get_fd(struct pw_protocol_native_connection *conn, uint32_t index);

struct spa_pod_builder *
pw_protocol_native_connection_begin_resource(struct pw_protocol_native_connection *conn,
                                             struct pw_resource *resource,
                                             uint8_t opcode);

struct spa_pod_builder *
pw_protocol_native_connection_begin_proxy(struct pw_protocol_native_connection *conn,
                                          struct pw_proxy *proxy,
                                          uint8_t opcode);
void
pw_protocol_native_connection_end(struct pw_protocol_native_connection *conn,
                                  struct spa_pod_builder *builder);

bool
pw_protocol_native_connection_flush(struct pw_protocol_native_connection *conn);

bool
pw_protocol_native_connection_clear(struct pw_protocol_native_connection *conn);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PIPEWIRE_PROTOCOL_NATIVE_CONNECTION_H__ */
