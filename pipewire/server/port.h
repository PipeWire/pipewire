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

#ifndef __PIPEWIRE_PORT_H__
#define __PIPEWIRE_PORT_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PIPEWIRE_TYPE__Port                          "PipeWire:Object:Port"
#define PIPEWIRE_TYPE_PORT_BASE                      PIPEWIRE_TYPE__Port ":"

#include <spa/node.h>

#include <pipewire/client/introspect.h>
#include <pipewire/client/mem.h>

#include <pipewire/server/core.h>
#include <pipewire/server/link.h>

enum pw_port_state {
	PW_PORT_STATE_ERROR = -1,
	PW_PORT_STATE_INIT = 0,
	PW_PORT_STATE_CONFIGURE = 1,
	PW_PORT_STATE_READY = 2,
	PW_PORT_STATE_PAUSED = 3,
	PW_PORT_STATE_STREAMING = 4,
};

/** \page page_port Port
 *
 * \section page_node_overview Overview
 *
 * A port can be used to link two nodes.
 */
/** \class pw_port
 *
 * The port object
 */
struct pw_port {
	struct spa_list link;		/**< link in node port_list */

	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_port *));

	struct pw_node *node;		/**< owner node */
	enum pw_direction direction;	/**< port direction */
	uint32_t port_id;		/**< port id */
	enum pw_port_state state;	/**< state of the port */
	struct spa_port_io io;		/**< io area of the port */

	bool allocated;			/**< if buffers are allocated */
	struct pw_memblock buffer_mem;	/**< allocated buffer memory */
	struct spa_buffer **buffers;	/**< port buffers */
	uint32_t n_buffers;		/**< number of port buffers */

	struct spa_list links;		/**< list of \ref pw_link */

	void *mix;			/**< optional port buffer mix/split */

	struct {
		struct spa_graph *graph;
		struct spa_graph_port port;
		struct spa_graph_port mix_port;
		struct spa_graph_node mix_node;
	} rt;				/**< data only accessed from the data thread */
};

/** Create a new port \memberof pw_port
 * \return a newly allocated port */
struct pw_port *
pw_port_new(struct pw_node *node, enum pw_direction direction, uint32_t port_id);

/** Destroy a port \memberof pw_port */
void pw_port_destroy(struct pw_port *port);

/** Set a format on a port \memberof pw_port */
int pw_port_set_format(struct pw_port *port, uint32_t flags, struct spa_format *format);

/** Use buffers on a port \memberof pw_port */
int pw_port_use_buffers(struct pw_port *port, struct spa_buffer **buffers, uint32_t n_buffers);

/** Allocate memory for buffers on a port \memberof pw_port */
int pw_port_alloc_buffers(struct pw_port *port,
			  struct spa_param **params, uint32_t n_params,
			  struct spa_buffer **buffers, uint32_t *n_buffers);


#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_PORT_H__ */
