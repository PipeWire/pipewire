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

#define PW_TYPE__Port                          "PipeWire:Object:Port"
#define PW_TYPE_PORT_BASE                      PW_TYPE__Port ":"

#include <spa/node.h>

#include <pipewire/utils.h>
#include <pipewire/introspect.h>
#include <pipewire/mem.h>

#include <pipewire/core.h>
#include <pipewire/link.h>

enum pw_port_state {
	PW_PORT_STATE_ERROR = -1,
	PW_PORT_STATE_INIT = 0,
	PW_PORT_STATE_CONFIGURE = 1,
	PW_PORT_STATE_READY = 2,
	PW_PORT_STATE_PAUSED = 3,
	PW_PORT_STATE_STREAMING = 4,
};

struct pw_port;

struct pw_port_implementation {
#define PW_VERSION_PORT_IMPLEMENTATION 0
	uint32_t version;

	int (*enum_formats) (struct pw_port *port,
			     struct spa_format **format,
			     const struct spa_format *filter,
			     int32_t index);

	int (*set_format) (struct pw_port *port, uint32_t flags, struct spa_format *format);

	int (*get_format) (struct pw_port *port, const struct spa_format **format);

	int (*get_info) (struct pw_port *port, const struct spa_port_info **info);

	int (*enum_params) (struct pw_port *port, uint32_t index, struct spa_param **param);

	int (*set_param) (struct pw_port *port, struct spa_param *param);

	int (*use_buffers) (struct pw_port *port, struct spa_buffer **buffers, uint32_t n_buffers);

	int (*alloc_buffers) (struct pw_port *port,
			      struct spa_param **params, uint32_t n_params,
			      struct spa_buffer **buffers, uint32_t *n_buffers);
	int (*reuse_buffer) (struct pw_port *port, uint32_t buffer_id);

	int (*send_command) (struct pw_port *port, struct spa_command *command);
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
	/** Emited when the port state changes */
	PW_SIGNAL(state_changed, (struct pw_listener *listener, struct pw_port *port));

	const struct pw_port_implementation *implementation;

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

        void *user_data;                /**< extra user data */
        pw_destroy_t destroy;           /**< function to clean up the object */
};

/** Create a new port \memberof pw_port
 * \return a newly allocated port */
struct pw_port *
pw_port_new(enum pw_direction direction,
	    uint32_t port_id,
	    size_t user_data_size);

/** Add a port to a node \memberof pw_port */
void pw_port_add(struct pw_port *port, struct pw_node *node);

/** Destroy a port \memberof pw_port */
void pw_port_destroy(struct pw_port *port);

/** Get the current format on a port \memberof pw_port */
int pw_port_enum_formats(struct pw_port *port,
			 struct spa_format **format,
			 const struct spa_format *filter,
			 int32_t index);

/** Set a format on a port \memberof pw_port */
int pw_port_set_format(struct pw_port *port, uint32_t flags, struct spa_format *format);

/** Get the current format on a port \memberof pw_port */
int pw_port_get_format(struct pw_port *port, const struct spa_format **format);

/** Get the info on a port \memberof pw_port */
int pw_port_get_info(struct pw_port *port, const struct spa_port_info **info);

/** Enumerate the port parameters \memberof pw_port */
int pw_port_enum_params(struct pw_port *port, uint32_t index, struct spa_param **param);

/** Set a port parameter \memberof pw_port */
int pw_port_set_param(struct pw_port *port, struct spa_param *param);

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
