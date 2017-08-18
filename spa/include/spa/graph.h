/* Simple Plugin API
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_GRAPH_H__
#define __SPA_GRAPH_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

#include <spa/defs.h>
#include <spa/list.h>
#include <spa/node.h>

#if 0
#define debug(...)	printf(__VA_ARGS__)
#else
#define debug(...)
#endif

struct spa_graph;
struct spa_graph_node;
struct spa_graph_port;

struct spa_graph {
	struct spa_list nodes;
};

struct spa_graph_node_callbacks {
#define SPA_VERSION_GRAPH_NODE_CALLBACKS	0
	uint32_t version;

	int (*process_input) (void *data);
	int (*process_output) (void *data);
};

struct spa_graph_port_callbacks {
#define SPA_VERSION_GRAPH_PORT_CALLBACKS	0
	uint32_t version;

	int (*reuse_buffer) (void *data, uint32_t buffer_id);
};

struct spa_graph_node {
	struct spa_list link;		/**< link in graph nodes list */
	struct spa_list ports[2];	/**< list of input and output ports */
	struct spa_list ready_link;	/**< link for scheduler */
#define SPA_GRAPH_NODE_FLAG_ASYNC       (1 << 0)
	uint32_t flags;			/**< node flags */
	uint32_t required_in;		/**< required number of ports */
	uint32_t ready_in;		/**< number of ports with data */
	int state;			/**< state of the node */
	/** callbacks and data */
	const struct spa_graph_node_callbacks *callbacks;
	void *callbacks_data;
	void *scheduler_data;		/**< scheduler private data */
};

struct spa_graph_port {
	struct spa_list link;		/**< link in node port list */
	struct spa_graph_node *node;	/**< owner node */
	enum spa_direction direction;	/**< port direction */
	uint32_t port_id;		/**< port id */
	uint32_t flags;			/**< port flags */
	struct spa_port_io *io;		/**< io area of the port */
	struct spa_graph_port *peer;	/**< peer */
	/** callbacks and data */
	const struct spa_graph_port_callbacks *callbacks;
	void *callbacks_data;
	void *scheduler_data;		/**< scheduler private data */
};

static inline void spa_graph_init(struct spa_graph *graph)
{
	spa_list_init(&graph->nodes);
}

static inline void
spa_graph_node_init(struct spa_graph_node *node)
{
	spa_list_init(&node->ports[SPA_DIRECTION_INPUT]);
	spa_list_init(&node->ports[SPA_DIRECTION_OUTPUT]);
	node->flags = 0;
	node->required_in = node->ready_in = 0;
	debug("node %p init\n", node);
}

static inline void
spa_graph_node_set_callbacks(struct spa_graph_node *node,
			     const struct spa_graph_node_callbacks *callbacks,
			     void *data)
{
	node->callbacks = callbacks;
	node->callbacks_data = data;
}

static inline void
spa_graph_node_add(struct spa_graph *graph,
		   struct spa_graph_node *node)
{
	node->state = SPA_RESULT_NEED_BUFFER;
	node->ready_link.next = NULL;
	spa_list_append(&graph->nodes, &node->link);
	debug("node %p add\n", node);
}

static inline void
spa_graph_port_init(struct spa_graph_port *port,
		    enum spa_direction direction,
		    uint32_t port_id,
		    uint32_t flags,
		    struct spa_port_io *io)
{
	debug("port %p init type %d id %d\n", port, direction, port_id);
	port->direction = direction;
	port->port_id = port_id;
	port->flags = flags;
	port->io = io;
}

static inline void
spa_graph_port_set_callbacks(struct spa_graph_port *port,
			     const struct spa_graph_port_callbacks *callbacks,
			     void *data)
{
	port->callbacks = callbacks;
	port->callbacks_data = data;
}

static inline void
spa_graph_port_add(struct spa_graph_node *node,
		   struct spa_graph_port *port)
{
	debug("port %p add to node %p\n", port, node);
	port->node = node;
	spa_list_append(&node->ports[port->direction], &port->link);
	if (!(port->flags & SPA_PORT_INFO_FLAG_OPTIONAL) && port->direction == SPA_DIRECTION_INPUT)
		node->required_in++;
}

static inline void spa_graph_node_remove(struct spa_graph_node *node)
{
	debug("node %p remove\n", node);
	spa_list_remove(&node->link);
	if (node->ready_link.next)
		spa_list_remove(&node->ready_link);
}

static inline void spa_graph_port_remove(struct spa_graph_port *port)
{
	debug("port %p remove\n", port);
	spa_list_remove(&port->link);
	if (!(port->flags & SPA_PORT_INFO_FLAG_OPTIONAL) && port->direction == SPA_DIRECTION_INPUT)
		port->node->required_in--;
}

static inline void
spa_graph_port_link(struct spa_graph_port *out, struct spa_graph_port *in)
{
	debug("port %p link to %p \n", out, in);
	out->peer = in;
	in->peer = out;
}

static inline void
spa_graph_port_unlink(struct spa_graph_port *port)
{
	debug("port %p unlink from %p \n", port, port->peer);
	if (port->peer) {
		port->peer->peer = NULL;
		port->peer = NULL;
	}
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_GRAPH_H__ */
