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

#include <spa/utils/defs.h>
#include <spa/utils/list.h>
#include <spa/node/node.h>
#include <spa/node/io.h>

#ifndef spa_debug
#define spa_debug(...)
#endif

struct spa_graph;
struct spa_graph_node;
struct spa_graph_port;

struct spa_graph_callbacks {
#define SPA_VERSION_GRAPH_CALLBACKS	0
	uint32_t version;

	int (*need_input) (void *data, struct spa_graph_node *node);
	int (*have_output) (void *data, struct spa_graph_node *node);
};

struct spa_graph {
	struct spa_list nodes;
	const struct spa_graph_callbacks *callbacks;
	void *callbacks_data;
};

#define spa_graph_need_input(g,n)	((g)->callbacks->need_input((g)->callbacks_data, (n)))
#define spa_graph_have_output(g,n)	((g)->callbacks->have_output((g)->callbacks_data, (n)))
#define spa_graph_reuse_buffer(g,n,p,i)	((g)->callbacks->reuse_buffer((g)->callbacks_data, (n),(p),(i)))

struct spa_graph_node {
	struct spa_list link;		/**< link in graph nodes list */
	struct spa_graph *graph;	/**< owner graph */
	struct spa_list ports[2];	/**< list of input and output ports */
	struct spa_list ready_link;	/**< link for scheduler */
#define SPA_GRAPH_NODE_FLAG_ASYNC	(1 << 0)
	uint32_t flags;			/**< node flags */
	uint32_t required[2];		/**< required number of ports */
	uint32_t ready[2];		/**< number of ports with data */
	int state;			/**< state of the node */
	struct spa_node *implementation;/**< node implementation */
	void *scheduler_data;		/**< scheduler private data */
};

struct spa_graph_port {
	struct spa_list link;		/**< link in node port list */
	struct spa_graph_node *node;	/**< owner node */
	enum spa_direction direction;	/**< port direction */
	uint32_t port_id;		/**< port id */
#define SPA_GRAPH_PORT_FLAG_DISABLED	(1 << 0)
	uint32_t flags;			/**< port flags */
	struct spa_io_buffers *io;	/**< io area of the port */
	struct spa_graph_port *peer;	/**< peer */
	void *scheduler_data;		/**< scheduler private data */
};

static inline void spa_graph_init(struct spa_graph *graph)
{
	spa_list_init(&graph->nodes);
}

static inline void
spa_graph_set_callbacks(struct spa_graph *graph,
			const struct spa_graph_callbacks *callbacks,
			void *data)
{
	graph->callbacks = callbacks;
	graph->callbacks_data = data;
}

static inline void
spa_graph_node_init(struct spa_graph_node *node)
{
	spa_list_init(&node->ports[SPA_DIRECTION_INPUT]);
	spa_list_init(&node->ports[SPA_DIRECTION_OUTPUT]);
	node->flags = 0;
	node->required[SPA_DIRECTION_INPUT] = node->ready[SPA_DIRECTION_INPUT] = 0;
	node->required[SPA_DIRECTION_OUTPUT] = node->ready[SPA_DIRECTION_OUTPUT] = 0;
	spa_debug("node %p init", node);
}

static inline void
spa_graph_node_set_implementation(struct spa_graph_node *node,
				  struct spa_node *implementation)
{
	node->implementation = implementation;
}

static inline void
spa_graph_node_add(struct spa_graph *graph,
		   struct spa_graph_node *node)
{
	node->graph = graph;
	node->state = SPA_STATUS_OK;
	node->ready_link.next = NULL;
	spa_list_append(&graph->nodes, &node->link);
	spa_debug("node %p add", node);
}

static inline void
spa_graph_port_init(struct spa_graph_port *port,
		    enum spa_direction direction,
		    uint32_t port_id,
		    uint32_t flags,
		    struct spa_io_buffers *io)
{
	spa_debug("port %p init type %d id %d", port, direction, port_id);
	port->direction = direction;
	port->port_id = port_id;
	port->flags = flags;
	port->io = io;
}

static inline void
spa_graph_port_add(struct spa_graph_node *node,
		   struct spa_graph_port *port)
{
	spa_debug("port %p add to node %p", port, node);
	port->node = node;
	spa_list_append(&node->ports[port->direction], &port->link);
	if (!(port->flags & SPA_PORT_INFO_FLAG_OPTIONAL))
		node->required[port->direction]++;
}

static inline void spa_graph_node_remove(struct spa_graph_node *node)
{
	spa_debug("node %p remove", node);
	spa_list_remove(&node->link);
	if (node->ready_link.next)
		spa_list_remove(&node->ready_link);
}

static inline void spa_graph_port_remove(struct spa_graph_port *port)
{
	spa_debug("port %p remove", port);
	spa_list_remove(&port->link);
	if (!(port->flags & SPA_PORT_INFO_FLAG_OPTIONAL) &&
	    port->node->required[port->direction] > 0) {
		port->node->required[port->direction]--;
	}
}

static inline void
spa_graph_port_link(struct spa_graph_port *out, struct spa_graph_port *in)
{
	spa_debug("port %p link to %p", out, in);
	out->peer = in;
	in->peer = out;
}

static inline void
spa_graph_port_unlink(struct spa_graph_port *port)
{
	spa_debug("port %p unlink from %p", port, port->peer);
	if (port->peer) {
		port->peer->peer = NULL;
		port->peer = NULL;
	}
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_GRAPH_H__ */
