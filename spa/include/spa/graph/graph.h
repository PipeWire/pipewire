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
struct spa_graph_link;
struct spa_graph_port;

struct spa_graph_state {
	int status;			/**< current status */
	uint32_t required;		/**< required number of signals */
	uint32_t pending;		/**< number of pending signals */
};

static inline void spa_graph_state_reset(struct spa_graph_state *state)
{
	state->pending = state->required;
}

struct spa_graph_link {
	struct spa_list link;
	struct spa_graph_state *state;
	int (*signal) (void *data, void *target);
	void *signal_data;
};

#define spa_graph_link_signal(l,t)	((l)->signal((l)->signal_data,(t)))

static inline int spa_graph_link_trigger(struct spa_graph_link *link, void *target)
{
	struct spa_graph_state *state = link->state;

	if (state->pending == 0) {
		spa_debug("link %p: nothing pending", link);
	} else {
		spa_debug("link %p: pending %d required %d", link,
	                        state->pending, state->required);
		if (__atomic_sub_fetch(&state->pending, 1, __ATOMIC_SEQ_CST) == 0)
			spa_graph_link_signal(link, target);
	}
        return state->status;
}

struct spa_graph_callbacks {
#define SPA_VERSION_GRAPH_CALLBACKS	0
	uint32_t version;

	int (*run) (void *data);
	int (*finish) (void *data);
};
#define spa_graph_run(g)		((g)->callbacks->run((g)->callbacks_data))
#define spa_graph_finish(g)		((g)->callbacks->finish((g)->callbacks_data))

struct spa_graph {
#define SPA_GRAPH_FLAG_DRIVER		(1 << 0)
	uint32_t flags;			/* flags */
	struct spa_graph_node *parent;	/* parent node or NULL when driver */
	struct spa_graph_state *state;	/* state of graph */
	struct spa_list nodes;		/* list of nodes of this graph */
	const struct spa_graph_callbacks *callbacks;
	void *callbacks_data;
};

struct spa_graph_node_callbacks {
#define SPA_VERSION_GRAPH_NODE_CALLBACKS	0
	uint32_t version;

	int (*trigger) (void *data, struct spa_graph_node *node);
	int (*process) (void *data, struct spa_graph_node *node);
	int (*reuse_buffer) (void *data, struct spa_graph_node *node,
			uint32_t port_id, uint32_t buffer_id);
};
#define spa_graph_node_trigger(n)	((n)->callbacks->trigger((n)->callbacks_data,(n)))
#define spa_graph_node_process(n)	((n)->callbacks->process((n)->callbacks_data,(n)))
#define spa_graph_node_reuse_buffer(n,p,i) ((n)->callbacks->reuse_buffer((n)->callbacks_data,(n),(p),(i)))

struct spa_graph_node {
	struct spa_list link;		/**< link in graph nodes list */
	struct spa_graph *graph;	/**< owner graph */
	struct spa_list ports[2];	/**< list of input and output ports */
	struct spa_list links;		/**< list of links */
	uint32_t flags;			/**< node flags */
	struct spa_graph_state *state;	/**< state of the node */
	struct spa_graph_link graph_link;	/**< link in graph */
	struct spa_graph *subgraph;	/**< subgraph or NULL */
	const struct spa_graph_node_callbacks *callbacks;
	void *callbacks_data;
	struct spa_list sched_link;	/**< link for scheduler */
};


struct spa_graph_port {
	struct spa_list link;		/**< link in node port list */
	struct spa_graph_node *node;	/**< owner node */
	enum spa_direction direction;	/**< port direction */
	uint32_t port_id;		/**< port id */
	uint32_t flags;			/**< port flags */
	struct spa_io_buffers *io;	/**< io area of the port */
	struct spa_graph_port *peer;	/**< peer */
};

static inline int spa_graph_link_signal_node(void *data, void *arg)
{
	struct spa_graph_node *node = data;
	return spa_graph_node_process(node);
}

static inline int spa_graph_link_signal_graph(void *data, void *arg)
{
	struct spa_graph_node *node = data;
	if (node->graph)
		spa_graph_finish(node->graph);
	return 0;
}

static inline void spa_graph_init(struct spa_graph *graph, struct spa_graph_state *state)
{
	spa_list_init(&graph->nodes);
	graph->flags = 0;
	graph->state = state;
	spa_debug("graph %p init", graph);
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
spa_graph_link_add(struct spa_graph_node *out,
		   struct spa_graph_state *state,
		   struct spa_graph_link *link)
{
	spa_debug("node %p add link %p to node %p", out, link, state);
	link->state = state;
	state->required++;
	spa_list_append(&out->links, &link->link);
}

static inline void spa_graph_link_remove(struct spa_graph_link *link)
{
	spa_debug("link %p remove", link);
	link->state->required--;
	spa_list_remove(&link->link);
}

static inline void
spa_graph_node_init(struct spa_graph_node *node, struct spa_graph_state *state)
{
	spa_list_init(&node->ports[SPA_DIRECTION_INPUT]);
	spa_list_init(&node->ports[SPA_DIRECTION_OUTPUT]);
	spa_list_init(&node->links);
	node->flags = 0;
	node->subgraph = NULL;
	node->state = state;
	node->state->required = node->state->pending = 0;
	node->state->status = SPA_STATUS_OK;
	node->graph_link.signal = spa_graph_link_signal_graph;
	node->graph_link.signal_data = node;
	spa_debug("node %p init", node);
}

static inline int spa_graph_node_impl_trigger(void *data, struct spa_graph_node *node)
{
	struct spa_graph_link *l, *t;
	spa_debug("node %p trigger", node);
	spa_list_for_each_safe(l, t, &node->links, link)
		spa_graph_link_trigger(l, node);
	return 0;
}

static inline int spa_graph_node_impl_sub_process(void *data, struct spa_graph_node *node)
{
	struct spa_graph *graph = node->subgraph;
	spa_debug("node %p: sub process %p", node, graph);
	return spa_graph_run(graph);
}

static const struct spa_graph_node_callbacks spa_graph_node_sub_impl_default = {
	SPA_VERSION_GRAPH_NODE_CALLBACKS,
	.trigger = spa_graph_node_impl_trigger,
	.process = spa_graph_node_impl_sub_process,
};

static inline void spa_graph_node_set_subgraph(struct spa_graph_node *node,
		struct spa_graph *subgraph)
{
	node->subgraph = subgraph;
	subgraph->parent = node;
	spa_debug("node %p set subgraph %p", node, subgraph);
}

static inline void
spa_graph_node_set_callbacks(struct spa_graph_node *node,
		const struct spa_graph_node_callbacks *callbacks,
		void *callbacks_data)
{
	node->callbacks = callbacks;
	node->callbacks_data = callbacks_data;
}

static inline void
spa_graph_node_add(struct spa_graph *graph,
		   struct spa_graph_node *node)
{
	node->graph = graph;
	spa_list_append(&graph->nodes, &node->link);
	spa_graph_link_add(node, graph->state, &node->graph_link);
	spa_debug("node %p add to graph %p", node, graph);
}

static inline void spa_graph_node_remove(struct spa_graph_node *node)
{
	spa_debug("node %p remove", node);
	spa_graph_link_remove(&node->graph_link);
	spa_list_remove(&node->link);
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
}

static inline void spa_graph_port_remove(struct spa_graph_port *port)
{
	spa_debug("port %p remove", port);
	spa_list_remove(&port->link);
}

static inline void
spa_graph_port_link(struct spa_graph_port *out, struct spa_graph_port *in)
{
	spa_debug("port %p link to %p %p %p", out, in, in->node, in->node->state);
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

static inline int spa_graph_node_impl_process(void *data, struct spa_graph_node *node)
{
	struct spa_node *n = data;

	spa_debug("node %p: process %d", node, node->state->status);
	if ((node->state->status = spa_node_process(n)) == SPA_STATUS_HAVE_BUFFER)
		spa_graph_node_trigger(node);

        return node->state->status;
}

static inline int spa_graph_node_impl_reuse_buffer(void *data, struct spa_graph_node *node,
		uint32_t port_id, uint32_t buffer_id)
{
	struct spa_node *n = data;
	return spa_node_port_reuse_buffer(n, port_id, buffer_id);
}

static const struct spa_graph_node_callbacks spa_graph_node_impl_default = {
	SPA_VERSION_GRAPH_NODE_CALLBACKS,
	.trigger = spa_graph_node_impl_trigger,
	.process = spa_graph_node_impl_process,
	.reuse_buffer = spa_graph_node_impl_reuse_buffer,
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_GRAPH_H__ */
