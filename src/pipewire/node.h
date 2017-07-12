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

#define PW_TYPE__Node                          PW_TYPE_OBJECT_BASE "Node"
#define PW_TYPE_NODE_BASE                      PW_TYPE__Node ":"

#include <spa/clock.h>
#include <spa/node.h>

#include <pipewire/mem.h>
#include <pipewire/introspect.h>
#include <pipewire/transport.h>

#include <pipewire/core.h>
#include <pipewire/port.h>
#include <pipewire/link.h>
#include <pipewire/client.h>
#include <pipewire/data-loop.h>

struct pw_node;


#define PW_VERSION_NODE_IMPLEMENTATION      0

struct pw_node_implementation {
	uint32_t version;

        int (*get_props) (struct pw_node *node, struct spa_props **props);

        int (*set_props) (struct pw_node *node, const struct spa_props *props);

        int (*send_command) (struct pw_node *node,
			     struct spa_command *command);

	struct pw_port* (*add_port) (struct pw_node *node,
				     enum pw_direction direction,
				     uint32_t port_id);

        int (*process_input) (struct pw_node *node);

        int (*process_output) (struct pw_node *node);
};

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

	bool live;			/**< if the node is live */
	struct spa_clock *clock;	/**< handle to SPA clock if any */

	struct spa_list resource_list;	/**< list of resources for this node */

	/** Implementation of core node functions */
	const struct pw_node_implementation *implementation;

	/** Emited when the node is initialized */
	PW_SIGNAL(initialized, (struct pw_listener *listener, struct pw_node *object));

	struct spa_list input_ports;		/**< list of input ports */
	struct pw_map input_port_map;		/**< map from port_id to port */
	uint32_t n_used_input_links;		/**< number of active input links */
	uint32_t idle_used_input_links;		/**< number of active input to be idle */

	struct spa_list output_ports;		/**< list of output ports */
	struct pw_map output_port_map;		/**< map from port_id to port */
	uint32_t n_used_output_links;		/**< number of active output links */
	uint32_t idle_used_output_links;	/**< number of active output to be idle */

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

	/** an event is emited */
	PW_SIGNAL(event, (struct pw_listener *listener,
			  struct pw_node *node, struct spa_event *event));

	struct pw_loop *data_loop;		/**< the data loop for this node */

	struct {
		struct spa_graph_scheduler *sched;
		struct spa_graph_node node;
	} rt;

        void *user_data;                /**< extra user data */
        pw_destroy_t destroy;           /**< function to clean up the object */
};

/** Create a new node \memberof pw_node */
struct pw_node *
pw_node_new(struct pw_core *core,		/**< the core */
	    struct pw_resource *owner,		/**< optional owner */
	    const char *name,			/**< node name */
	    struct pw_properties *properties,	/**< extra properties */
	    size_t user_data_size		/**< user data size */);

/** Complete initialization of the node */
void pw_node_export(struct pw_node *node);

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
