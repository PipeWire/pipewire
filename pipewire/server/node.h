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

#ifndef __PIPEWIRE_NODE_H__
#define __PIPEWIRE_NODE_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PIPEWIRE_TYPE__Node                          "PipeWire:Object:Node"
#define PIPEWIRE_TYPE_NODE_BASE                      PIPEWIRE_TYPE__Node ":"

#include <spa/clock.h>
#include <spa/node.h>

#include <pipewire/client/mem.h>
#include <pipewire/client/transport.h>

#include <pipewire/server/core.h>
#include <pipewire/server/port.h>
#include <pipewire/server/link.h>
#include <pipewire/server/client.h>
#include <pipewire/server/data-loop.h>

/** \page page_node Node
 *
 * \section page_node_overview Overview
 *
 * The node object processes data. The node has a list of
 * input and output ports (\ref page_port) on which it
 * will receive and send out buffers respectively.
 *
 * The node wraps an SPA node object.
 */
/** \class pw_node
 *
 * PipeWire node class.
 */
struct pw_node {
	struct pw_core *core;		/**< core object */
	struct spa_list link;		/**< link in core node_list */
	struct pw_global *global;	/**< global for this node */

	struct pw_resource *owner;		/**< owner resource if any */
	struct pw_properties *properties;	/**< properties of the node */

	struct pw_node_info info;		/**< introspectable node info */

	/** Emited when a state change is started */
	PW_SIGNAL(state_request, (struct pw_listener *listener,
				  struct pw_node *object, enum pw_node_state state));
	/** Emited when a stat change is completed */
	PW_SIGNAL(state_changed, (struct pw_listener *listener,
				  struct pw_node *object,
				  enum pw_node_state old, enum pw_node_state state));

	struct spa_handle *handle;	/**< handle to SPA factory */
	struct spa_node *node;		/**< handle to SPA node */
	bool live;			/**< if the node is live */
	struct spa_clock *clock;	/**< handle to SPA clock if any */

	struct spa_list resource_list;	/**< list of resources for this node */

	/** Emited when the node is initialized */
	PW_SIGNAL(initialized, (struct pw_listener *listener, struct pw_node *object));

	struct spa_list input_ports;		/**< list of input ports */
	struct pw_port **input_port_map;	/**< map from port_id to port */
	uint32_t n_used_input_links;		/**< number of active input links */

	struct spa_list output_ports;		/**< list of output ports */
	struct pw_port **output_port_map;	/**< map from port_id to port */
	uint32_t n_used_output_links;		/**< number of active output links */

	/** Emited when a new port is added */
	PW_SIGNAL(port_added, (struct pw_listener *listener,
			       struct pw_node *node, struct pw_port *port));
	/** Emited when a port is removed */
	PW_SIGNAL(port_removed, (struct pw_listener *listener,
				 struct pw_node *node, struct pw_port *port));

	/** Emited when the node is destroyed */
	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_node *object));
	/** Emited when the node is free */
	PW_SIGNAL(free_signal, (struct pw_listener *listener, struct pw_node *object));

	/** an async operation on the node completed */
	PW_SIGNAL(async_complete, (struct pw_listener *listener,
				   struct pw_node *node, uint32_t seq, int res));

	struct pw_data_loop *data_loop;		/**< the data loop for this node */
};

/** Create a new node \memberof pw_node */
struct pw_node *
pw_node_new(struct pw_core *core,		/**< the core */
	    struct pw_resource *owner,		/**< optional owner */
	    const char *name,			/**< node name */
	    bool async,				/**< if the node will initialize async */
	    struct spa_node *node,		/**< the node */
	    struct spa_clock *clock,		/**< optional clock */
	    struct pw_properties *properties	/**< extra properties */);

/** Destroy a node */
void pw_node_destroy(struct pw_node *node);

/** Get a free unused port from the node */
struct pw_port *
pw_node_get_free_port(struct pw_node *node, enum pw_direction direction);

/** Change the state of the node */
int pw_node_set_state(struct pw_node *node, enum pw_node_state state);

/** Update the state of the node, mostly used by node implementations */
void pw_node_update_state(struct pw_node *node, enum pw_node_state state, char *error);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_NODE_H__ */
