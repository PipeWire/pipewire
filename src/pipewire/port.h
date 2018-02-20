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

#include <spa/utils/hook.h>

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
struct pw_control;

#include <pipewire/core.h>
#include <pipewire/introspect.h>
#include <pipewire/node.h>

enum pw_port_state {
	PW_PORT_STATE_ERROR = -1,	/**< the port is in error */
	PW_PORT_STATE_INIT = 0,		/**< the port is being created */
	PW_PORT_STATE_CONFIGURE = 1,	/**< the port is ready for format negotiation */
	PW_PORT_STATE_READY = 2,	/**< the port is ready for buffer allocation */
	PW_PORT_STATE_PAUSED = 3,	/**< the port is paused */
	PW_PORT_STATE_STREAMING = 4,	/**< the port is streaming */
};

/** Port events, use \ref pw_port_add_listener */
struct pw_port_events {
#define PW_VERSION_PORT_EVENTS 0
	uint32_t version;

	/** The port is destroyed */
	void (*destroy) (void *data);

	/** The port is freed */
	void (*free) (void *data);

	/** the port info changed */
	void (*info_changed) (void *data, struct pw_port_info *info);

	/** a new link is added on this port */
	void (*link_added) (void *data, struct pw_link *link);

	/** a link is removed from this port */
	void (*link_removed) (void *data, struct pw_link *link);

	/** the state of the port changed */
	void (*state_changed) (void *data, enum pw_port_state state);

	/** a control was added to the port */
	void (*control_added) (void *data, struct pw_control *control);

	/** a control was removed from the port */
	void (*control_removed) (void *data, struct pw_control *control);
};

/** Get the port direction */
enum pw_direction pw_port_get_direction(struct pw_port *port);

/** Get the port properties */
const struct pw_properties *pw_port_get_properties(struct pw_port *port);

/** Update the port properties */
int pw_port_update_properties(struct pw_port *port, const struct spa_dict *dict);

/** Get the port id */
uint32_t pw_port_get_id(struct pw_port *port);

/** Get the port parent node or NULL when not yet set */
struct pw_node *pw_port_get_node(struct pw_port *port);

/** Add an event listener on the port */
void pw_port_add_listener(struct pw_port *port,
			  struct spa_hook *listener,
			  const struct pw_port_events *events,
			  void *data);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_PORT_H__ */
