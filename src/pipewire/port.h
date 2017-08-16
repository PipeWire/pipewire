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

#include <spa/hook.h>
#include <spa/node.h>
#include <spa/buffer.h>

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
struct pw_port;
struct pw_link;

#include <pipewire/core.h>
#include <pipewire/introspect.h>
#include <pipewire/node.h>

enum pw_port_state {
	PW_PORT_STATE_ERROR = -1,
	PW_PORT_STATE_INIT = 0,
	PW_PORT_STATE_CONFIGURE = 1,
	PW_PORT_STATE_READY = 2,
	PW_PORT_STATE_PAUSED = 3,
	PW_PORT_STATE_STREAMING = 4,
};

struct pw_port_implementation {
#define PW_VERSION_PORT_IMPLEMENTATION 0
	uint32_t version;

	int (*set_io) (void *data, struct spa_port_io *io);

	int (*enum_formats) (void *data,
			     struct spa_format **format,
			     const struct spa_format *filter,
			     int32_t index);

	int (*set_format) (void *data, uint32_t flags, const struct spa_format *format);

	int (*get_format) (void *data, const struct spa_format **format);

	int (*get_info) (void *data, const struct spa_port_info **info);

	int (*enum_params) (void *data, uint32_t index, struct spa_param **param);

	int (*set_param) (void *data, struct spa_param *param);

	int (*use_buffers) (void *data, struct spa_buffer **buffers, uint32_t n_buffers);

	int (*alloc_buffers) (void *data,
			      struct spa_param **params, uint32_t n_params,
			      struct spa_buffer **buffers, uint32_t *n_buffers);
	int (*reuse_buffer) (void *data, uint32_t buffer_id);

	int (*send_command) (void *data, struct spa_command *command);
};

struct pw_port_events {
#define PW_VERSION_PORT_EVENTS 0
	uint32_t version;

	void (*destroy) (void *data);

	void (*link_added) (void *data, struct pw_link *link);

	void (*link_removed) (void *data, struct pw_link *link);

	void (*state_changed) (void *data, enum pw_port_state state);

	void (*properties_changed) (void *data, const struct pw_properties *properties);
};

/** Create a new port \memberof pw_port
 * \return a newly allocated port */
struct pw_port *
pw_port_new(enum pw_direction direction,
	    uint32_t port_id,
	    struct pw_properties *properties,
	    size_t user_data_size);

/** Get the port direction */
enum pw_direction pw_port_get_direction(struct pw_port *port);

const struct pw_properties *pw_port_get_properties(struct pw_port *port);

void pw_port_update_properties(struct pw_port *port, const struct spa_dict *dict);

/** Get the port id */
uint32_t pw_port_get_id(struct pw_port *port);

/** Get the port parent node or NULL when not yet set */
struct pw_node *pw_port_get_node(struct pw_port *port);

/** Add a port to a node \memberof pw_port */
void pw_port_add(struct pw_port *port, struct pw_node *node);

void pw_port_set_implementation(struct pw_port *port,
				const struct pw_port_implementation *implementation,
				void *data);

void pw_port_add_listener(struct pw_port *port,
			  struct spa_hook *listener,
			  const struct pw_port_events *events,
			  void *data);

/** Destroy a port \memberof pw_port */
void pw_port_destroy(struct pw_port *port);

void * pw_port_get_user_data(struct pw_port *port);

/** Get the current format on a port \memberof pw_port */
int pw_port_enum_formats(struct pw_port *port,
			 struct spa_format **format,
			 const struct spa_format *filter,
			 int32_t index);

/** Set a format on a port \memberof pw_port */
int pw_port_set_format(struct pw_port *port, uint32_t flags, const struct spa_format *format);

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
