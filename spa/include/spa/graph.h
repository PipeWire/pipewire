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
	struct spa_list ready;
};

typedef int (*spa_graph_node_func_t) (struct spa_graph_node * node);

struct spa_graph_node {
	struct spa_list link;
	struct spa_list ready_link;
	struct spa_list ports[2];
#define SPA_GRAPH_NODE_FLAG_ASYNC       (1 << 0)
	uint32_t flags;
	int state;
#define SPA_GRAPH_ACTION_CHECK   0
#define SPA_GRAPH_ACTION_IN      1
#define SPA_GRAPH_ACTION_OUT     2
	uint32_t action;
	spa_graph_node_func_t schedule;
	void *user_data;
	uint32_t max_in;
	uint32_t required_in;
	uint32_t ready_in;
};

struct spa_graph_port {
	struct spa_list link;
	struct spa_graph_node *node;
	enum spa_direction direction;
	uint32_t port_id;
	uint32_t flags;
	struct spa_port_io *io;
	struct spa_graph_port *peer;
};

static inline void spa_graph_init(struct spa_graph *graph)
{
	spa_list_init(&graph->nodes);
	spa_list_init(&graph->ready);
}

static inline void
spa_graph_node_add(struct spa_graph *graph, struct spa_graph_node *node,
		   spa_graph_node_func_t schedule, void *user_data)
{
	spa_list_init(&node->ports[SPA_DIRECTION_INPUT]);
	spa_list_init(&node->ports[SPA_DIRECTION_OUTPUT]);
	node->flags = 0;
	node->state = SPA_RESULT_NEED_BUFFER;
	node->action = SPA_GRAPH_ACTION_OUT;
	node->schedule = schedule;
	node->user_data = user_data;
	node->ready_link.next = NULL;
	spa_list_insert(graph->nodes.prev, &node->link);
	node->max_in = node->required_in = node->ready_in = 0;
	debug("node %p add\n", node);
}

static inline void spa_graph_port_check(struct spa_graph *graph, struct spa_graph_port *port)
{
	struct spa_graph_node *node = port->node;

	if (port->io->status == SPA_RESULT_HAVE_BUFFER)
		node->ready_in++;

	debug("port %p node %p check %d %d %d\n", port, node, port->io->status, node->ready_in, node->required_in);

	if (node->required_in > 0 && node->ready_in == node->required_in) {
		node->action = SPA_GRAPH_ACTION_IN;
		if (node->ready_link.next == NULL)
			spa_list_insert(graph->ready.prev, &node->ready_link);
	} else if (node->ready_link.next) {
		spa_list_remove(&node->ready_link);
		node->ready_link.next = NULL;
	}
}

static inline void spa_graph_node_update(struct spa_graph *graph, struct spa_graph_node *node) {
	struct spa_graph_port *p;

	node->ready_in = 0;
	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		if (p->io->status == SPA_RESULT_OK && !(node->flags & SPA_GRAPH_NODE_FLAG_ASYNC))
			node->ready_in++;
	}
	debug("node %p update %d ready\n", node, node->ready_in);
}

static inline void
spa_graph_port_add(struct spa_graph *graph,
		   struct spa_graph_node *node,
		   struct spa_graph_port *port,
		   enum spa_direction direction,
		   uint32_t port_id,
		   uint32_t flags,
		   struct spa_port_io *io)
{
	debug("port %p add %d to node %p \n", port, direction, node);
	port->node = node;
	port->direction = direction;
	port->port_id = port_id;
	port->flags = flags;
	port->io = io;
	spa_list_insert(node->ports[direction].prev, &port->link);
	node->max_in++;
	if (!(port->flags & SPA_PORT_INFO_FLAG_OPTIONAL) && direction == SPA_DIRECTION_INPUT)
		node->required_in++;
	spa_graph_port_check(graph, port);
}

static inline void spa_graph_node_remove(struct spa_graph *graph, struct spa_graph_node *node)
{
	debug("node %p remove\n", node);
	spa_list_remove(&node->link);
	if (node->ready_link.next)
		spa_list_remove(&node->ready_link);
}

static inline void spa_graph_port_remove(struct spa_graph *graph, struct spa_graph_port *port)
{
	debug("port %p remove\n", port);
	spa_list_remove(&port->link);
	if (!(port->flags & SPA_PORT_INFO_FLAG_OPTIONAL) && port->direction == SPA_DIRECTION_INPUT)
		port->node->required_in--;
}

static inline void
spa_graph_port_link(struct spa_graph *graph, struct spa_graph_port *out, struct spa_graph_port *in)
{
	debug("port %p link to %p \n", out, in);
	out->peer = in;
	in->peer = out;
}

static inline void
spa_graph_port_unlink(struct spa_graph *graph, struct spa_graph_port *port)
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
