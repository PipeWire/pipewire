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

#ifndef __SPA_GRAPH_SCHEDULER_H__
#define __SPA_GRAPH_SCHEDULER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/graph.h>

static inline int spa_graph_scheduler_default(struct spa_graph_node *node)
{
	int res;
	struct spa_node *n = node->user_data;

	if (node->action == SPA_GRAPH_ACTION_IN)
		res = spa_node_process_input(n);
	else if (node->action == SPA_GRAPH_ACTION_OUT)
		res = spa_node_process_output(n);
	else
		res = SPA_RESULT_ERROR;

	return res;
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

static inline bool spa_graph_scheduler_iterate(struct spa_graph *graph)
{
	bool empty;
	struct spa_graph_port *p;
	struct spa_graph_node *n;
	int iter = 1;
	uint32_t state;

next:
	empty = spa_list_is_empty(&graph->ready);
	if (empty && !spa_list_is_empty(&graph->pending)) {
		debug("copy pending\n");
		spa_list_insert_list(&graph->ready, &graph->pending);
		spa_list_init(&graph->pending);
		empty = false;
	}
	if (iter-- == 0 || empty)
		return !empty;

	n = spa_list_first(&graph->ready, struct spa_graph_node, ready_link);
	spa_list_remove(&n->ready_link);
	n->ready_link.next = NULL;

	debug("node %p state %d\n", n, n->state);

	switch (n->state) {
	case SPA_GRAPH_STATE_IN:
	case SPA_GRAPH_STATE_OUT:
	case SPA_GRAPH_STATE_END:
		if (n->state == SPA_GRAPH_STATE_END)
			n->state = SPA_GRAPH_STATE_OUT;

		state = n->schedule(n);
		debug("node %p schedule %d res %d\n", n, action, state);

		if (n->state == SPA_GRAPH_STATE_IN && n == graph->node)
			break;

		if (n->state != SPA_GRAPH_STATE_END) {
			debug("node %p add ready for CHECK\n", n);
			if (state == SPA_RESULT_NEED_BUFFER)
				n->state = SPA_GRAPH_STATE_CHECK_IN;
			else if (state == SPA_RESULT_HAVE_BUFFER)
				n->state = SPA_GRAPH_STATE_CHECK_OUT;
			else if (state == SPA_RESULT_OK)
				n->state = SPA_GRAPH_STATE_CHECK_OK;
			spa_list_insert(graph->ready.prev, &n->ready_link);
		}
		else {
			spa_graph_node_update(graph, n);
		}
		break;

	case SPA_GRAPH_STATE_CHECK_IN:
		n->ready_in = 0;
		spa_list_for_each(p, &n->ports[SPA_DIRECTION_INPUT], link) {
			struct spa_graph_node *pn = p->peer->node;
			if (p->io->status == SPA_RESULT_NEED_BUFFER) {
				if (pn != graph->node
				    || pn->flags & SPA_GRAPH_NODE_FLAG_ASYNC) {
					pn->state = SPA_GRAPH_STATE_OUT;
					debug("node %p add ready OUT\n", n);
					spa_list_insert(graph->ready.prev,
							&pn->ready_link);
				}
			} else if (p->io->status == SPA_RESULT_OK)
				n->ready_in++;
		}
		break;

	case SPA_GRAPH_STATE_CHECK_OUT:
		spa_list_for_each(p, &n->ports[SPA_DIRECTION_OUTPUT], link)
			spa_graph_port_check(graph, p->peer);

		debug("node %p add pending\n", n);
		n->state = SPA_GRAPH_STATE_END;
		spa_list_insert(&graph->pending, &n->ready_link);
		break;

	case SPA_GRAPH_STATE_CHECK_OK:
		spa_graph_node_update(graph, n);
		break;

	default:
		break;
	}
	goto next;
}

static inline void spa_graph_scheduler_pull(struct spa_graph *graph, struct spa_graph_node *node)
{
	node->action = SPA_GRAPH_ACTION_CHECK;
	node->state = SPA_RESULT_NEED_BUFFER;
	graph->node = node;
	debug("node %p start pull\n", node);
	if (node->ready_link.next == NULL)
		spa_list_insert(graph->ready.prev, &node->ready_link);
}

static inline void spa_graph_scheduler_push(struct spa_graph *graph, struct spa_graph_node *node)
{
	node->action = SPA_GRAPH_ACTION_OUT;
	graph->node = node;
	debug("node %p start push\n", node);
	if (node->ready_link.next == NULL)
		spa_list_insert(graph->ready.prev, &node->ready_link);
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_GRAPH_SCHEDULER_H__ */
