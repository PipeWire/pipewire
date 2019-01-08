/* Simple Plugin API
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
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
	int (*signal) (void *data);
	void *signal_data;
};

#define spa_graph_link_signal(l)	((l)->signal((l)->signal_data))

static inline int spa_graph_link_trigger(struct spa_graph_link *link)
{
	struct spa_graph_state *state = link->state;

	spa_debug("link %p: state %p: pending %d required %d", link, state,
                        state->pending, state->required);

	if (__atomic_sub_fetch(&state->pending, 1, __ATOMIC_SEQ_CST) == 0)
		spa_graph_link_signal(link);

        return state->status;
}
struct spa_graph {
	uint32_t flags;			/* flags */
	struct spa_graph_node *parent;	/* parent node or NULL when driver */
	struct spa_graph_state *state;	/* state of graph */
	struct spa_list nodes;		/* list of nodes of this graph */
};

struct spa_graph_node_callbacks {
#define SPA_VERSION_GRAPH_NODE_CALLBACKS	0
	uint32_t version;

	int (*process) (void *data, struct spa_graph_node *node);
	int (*reuse_buffer) (void *data, struct spa_graph_node *node,
			uint32_t port_id, uint32_t buffer_id);
};
#define spa_graph_node_process(n)	((n)->callbacks->process((n)->callbacks_data,(n)))
#define spa_graph_node_reuse_buffer(n,p,i) ((n)->callbacks->reuse_buffer((n)->callbacks_data,(n),(p),(i)))

struct spa_graph_node {
	struct spa_list link;		/**< link in graph nodes list */
	struct spa_graph *graph;	/**< owner graph */
	struct spa_list ports[2];	/**< list of input and output ports */
	struct spa_list links;		/**< list of links to next nodes */
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
	struct spa_graph_port *peer;	/**< peer */
};

static inline int spa_graph_run(struct spa_graph *graph)
{
	struct spa_graph_node *n, *tmp;
	struct spa_list pending;

	spa_debug("graph %p run", graph);
	spa_graph_state_reset(graph->state);

	spa_list_init(&pending);

	spa_list_for_each(n, &graph->nodes, link) {
		struct spa_graph_state *s = n->state;

		spa_graph_state_reset(s);

		spa_debug("graph %p node %p: state %p add %d status %d", graph, n,
				s, s->pending, s->status);

		if (s->pending == 0)
			spa_list_append(&pending, &n->sched_link);
	}
	spa_list_for_each_safe(n, tmp, &pending, sched_link)
		spa_graph_node_process(n);

	return 0;
}

static inline int spa_graph_node_trigger(struct spa_graph_node *node)
{
	struct spa_graph_link *l, *t;
	spa_debug("node %p trigger", node);
	spa_list_for_each_safe(l, t, &node->links, link)
		spa_graph_link_trigger(l);
	return 0;
}

static inline int spa_graph_finish(struct spa_graph *graph)
{
	spa_debug("graph %p finish", graph);
	if (graph->parent)
		spa_graph_node_trigger(graph->parent);
	return 0;
}
static inline int spa_graph_link_signal_node(void *data)
{
	struct spa_graph_node *node = (struct spa_graph_node *)data;
	spa_debug("node %p call process", node);
	return spa_graph_node_process(node);
}

static inline int spa_graph_link_signal_graph(void *data)
{
	struct spa_graph_node *node = (struct spa_graph_node *)data;
	if (node->graph)
		spa_graph_finish(node->graph);
	return 0;
}

static inline void spa_graph_init(struct spa_graph *graph, struct spa_graph_state *state)
{
	spa_list_init(&graph->nodes);
	graph->flags = 0;
	graph->state = state;
	spa_debug("graph %p init state %p", graph, state);
}

static inline void
spa_graph_link_add(struct spa_graph_node *out,
		   struct spa_graph_state *state,
		   struct spa_graph_link *link)
{
	link->state = state;
	state->required++;
	spa_debug("node %p add link %p to state %p %d", out, link, state, state->required);
	spa_list_append(&out->links, &link->link);
}

static inline void spa_graph_link_remove(struct spa_graph_link *link)
{
	link->state->required--;
	spa_debug("link %p state %p remove %d", link, link->state, link->state->required);
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
	spa_debug("node %p init state %p", node, state);
}


static inline int spa_graph_node_impl_sub_process(void *data, struct spa_graph_node *node)
{
	struct spa_graph *graph = node->subgraph;
	spa_debug("node %p: sub process %p", node, graph);
	return spa_graph_run(graph);
}

static const struct spa_graph_node_callbacks spa_graph_node_sub_impl_default = {
	SPA_VERSION_GRAPH_NODE_CALLBACKS,
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
	spa_debug("node %p add to graph %p", node, graph);
	node->graph = graph;
	spa_list_append(&graph->nodes, &node->link);
	spa_graph_link_add(node, graph->state, &node->graph_link);
}

static inline void spa_graph_node_remove(struct spa_graph_node *node)
{
	spa_debug("node %p remove from graph %p", node, node->graph);
	spa_graph_link_remove(&node->graph_link);
	spa_list_remove(&node->link);
}


static inline void
spa_graph_port_init(struct spa_graph_port *port,
		    enum spa_direction direction,
		    uint32_t port_id,
		    uint32_t flags)
{
	spa_debug("port %p init type %d id %d", port, direction, port_id);
	port->direction = direction;
	port->port_id = port_id;
	port->flags = flags;
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
	struct spa_node *n = (struct spa_node *)data;
	struct spa_graph_state *state = node->state;

	spa_debug("node %p: process state %p: %d, node %p", node, state, state->status, n);
	if ((state->status = spa_node_process(n)) != SPA_STATUS_OK)
		spa_graph_node_trigger(node);

        return state->status;
}

static inline int spa_graph_node_impl_reuse_buffer(void *data, struct spa_graph_node *node,
		uint32_t port_id, uint32_t buffer_id)
{
	struct spa_node *n = (struct spa_node *)data;
	return spa_node_port_reuse_buffer(n, port_id, buffer_id);
}

static const struct spa_graph_node_callbacks spa_graph_node_impl_default = {
	SPA_VERSION_GRAPH_NODE_CALLBACKS,
	.process = spa_graph_node_impl_process,
	.reuse_buffer = spa_graph_node_impl_reuse_buffer,
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_GRAPH_H__ */
