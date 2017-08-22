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

/** \page page_node Node
 *
 * \section page_node_overview Overview
 *
 * The node object processes data. The node has a list of
 * input and output ports (\ref page_port) on which it
 * will receive and send out buffers respectively.
 */
/** \class pw_node
 *
 * PipeWire node class.
 */
struct pw_node;

#include <pipewire/core.h>
#include <pipewire/global.h>
#include <pipewire/introspect.h>
#include <pipewire/port.h>
#include <pipewire/resource.h>

struct pw_node_implementation {
#define PW_VERSION_NODE_IMPLEMENTATION	0
	uint32_t version;

        int (*get_props) (void *data, struct spa_props **props);

        int (*set_props) (void *data, const struct spa_props *props);

        int (*send_command) (void *data,
			     const struct spa_command *command);

	struct pw_port* (*add_port) (void *data,
				     enum pw_direction direction,
				     uint32_t port_id);

        int (*process_input) (void *data);

        int (*process_output) (void *data);
};


struct pw_node_events {
#define PW_VERSION_NODE_EVENTS	0
	uint32_t version;

	/** the node is destroyed */
        void (*destroy) (void *data);
	/** the node is about to be freed */
        void (*free) (void *data);
	/** the node is initialized */
        void (*initialized) (void *data);

	/** a port was added */
        void (*port_added) (void *data, struct pw_port *port);
	/** a port was removed */
        void (*port_removed) (void *data, struct pw_port *port);

	/** the node info changed */
	void (*info_changed) (void *data, struct pw_node_info *info);
	/** a new state is requested on the node */
	void (*state_request) (void *data, enum pw_node_state state);
	/** the state of the node changed */
	void (*state_changed) (void *data, enum pw_node_state old,
			       enum pw_node_state state, const char *error);

	/** an async operation completed on the node */
	void (*async_complete) (void *data, uint32_t seq, int res);

        /** an event is emited */
	void (*event) (void *data, const struct spa_event *event);

        /** the node wants input */
	void (*need_input) (void *data);
        /** the node has output */
	void (*have_output) (void *data);
        /** the node has a buffer to reuse */
	void (*reuse_buffer) (void *data, uint32_t port_id, uint32_t buffer_id);
};

/** Create a new node \memberof pw_node */
struct pw_node *
pw_node_new(struct pw_core *core,		/**< the core */
	    struct pw_resource *owner,		/**< optional owner */
	    struct pw_global *parent,		/**< optional parent */
	    const char *name,			/**< node name */
	    struct pw_properties *properties,	/**< extra properties */
	    size_t user_data_size		/**< user data size */);

/** Complete initialization of the node and register */
void pw_node_register(struct pw_node *node);

/** Destroy a node */
void pw_node_destroy(struct pw_node *node);

void pw_node_set_max_ports(struct pw_node *node,
			   uint32_t max_input_ports,
			   uint32_t max_output_ports);

const struct pw_node_info *pw_node_get_info(struct pw_node *node);

void * pw_node_get_user_data(struct pw_node *node);

struct pw_core *pw_node_get_core(struct pw_node *node);

struct pw_resource *pw_node_get_owner(struct pw_node *node);

struct pw_global *pw_node_get_global(struct pw_node *node);

const struct pw_properties *pw_node_get_properties(struct pw_node *node);

void pw_node_update_properties(struct pw_node *node, const struct spa_dict *dict);

void pw_node_set_implementation(struct pw_node *node,
				const struct pw_node_implementation *implementation,
				void *data);

void pw_node_add_listener(struct pw_node *node,
			  struct spa_hook *listener,
			  const struct pw_node_events *events,
			  void *data);

struct spa_hook_list *pw_node_get_listeners(struct pw_node *node);

/** iterate the ports in the given direction */
bool pw_node_for_each_port(struct pw_node *node,
			   enum pw_direction direction,
			   bool (*callback) (void *data, struct pw_port *port),
			   void *data);


/** Find the port with direction and port_id or NULL when not found */
struct pw_port *
pw_node_find_port(struct pw_node *node, enum pw_direction direction, uint32_t port_id);

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
