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

#ifndef __PIPEWIRE_CONNECTION_H__
#define __PIPEWIRE_CONNECTION_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/defs.h>
#include <pipewire/client/sig.h>

/** \class pw_connection
 *
 * \brief Manages the connection between client and server
 *
 * The \ref pw_connection handles the connection between client
 * and server on a given socket.
 */
struct pw_connection {
	int fd;	/**< the socket */

	/** Emited when data has been written that needs to be flushed */
	PW_SIGNAL(need_flush,     (struct pw_listener *listener, struct pw_connection *conn));
	/** Emited when the connection is destroyed */
	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_connection *conn));
};

struct pw_connection *
pw_connection_new(int fd);

void
pw_connection_destroy(struct pw_connection *conn);

uint32_t
pw_connection_add_fd(struct pw_connection *conn, int fd);

int
pw_connection_get_fd(struct pw_connection *conn, uint32_t index);

bool
pw_connection_get_next(struct pw_connection *conn,
		       uint8_t *opcode,
		       uint32_t *dest_id,
		       void **data, uint32_t *size);

struct spa_pod_builder *
pw_connection_begin_write_resource(struct pw_connection *conn,
				   struct pw_resource *resource,
				   uint8_t opcode);

struct spa_pod_builder *
pw_connection_begin_write_proxy(struct pw_connection *conn,
				struct pw_proxy *proxy,
				uint8_t opcode);


void
pw_connection_end_write(struct pw_connection *conn,
			struct spa_pod_builder *builder);

bool
pw_connection_flush(struct pw_connection *conn);

bool
pw_connection_clear(struct pw_connection *conn);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PIPEWIRE_CONNECTION_H__ */
