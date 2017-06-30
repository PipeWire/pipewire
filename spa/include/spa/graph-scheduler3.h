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

struct spa_graph_scheduler {
	struct spa_graph *graph;
        struct spa_list pending;
        struct spa_graph_node *node;
};

static inline void spa_graph_scheduler_init(struct spa_graph_scheduler *sched,
					    struct spa_graph *graph)
{
	sched->graph = graph;
	spa_list_init(&sched->pending);
	sched->node = NULL;
}

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

static inline void spa_graph_scheduler_pull(struct spa_graph_scheduler *sched, struct spa_graph_node *node)
{
	struct spa_graph_port *p;
	struct spa_graph_node *n, *t;
	struct spa_list ready;

	debug("node %p start pull\n", node);

	spa_list_init(&ready);

	node->ready_in = 0;
	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		struct spa_graph_port *pport = p->peer;
		struct spa_graph_node *pnode = pport->node;
		debug("node %p peer %p io %d\n", node, pnode, pport->io->status);
		if (pport->io->status == SPA_RESULT_NEED_BUFFER) {
			spa_list_insert(ready.prev, &pnode->ready_link);
		}
		else if (pport->io->status == SPA_RESULT_OK && !(pnode->flags & SPA_GRAPH_NODE_FLAG_ASYNC))
			node->ready_in++;
	}

	spa_list_for_each_safe(n, t, &ready, ready_link) {
		n->action = SPA_GRAPH_ACTION_OUT;
		n->state = n->schedule(n);
		debug("peer %p scheduled %d %d\n", n, n->action, n->state);
		if (n->state == SPA_RESULT_NEED_BUFFER)
			spa_graph_scheduler_pull(sched, n);
		else {
			spa_list_for_each(p, &n->ports[SPA_DIRECTION_OUTPUT], link) {
				if (p->io->status == SPA_RESULT_HAVE_BUFFER)
			                node->ready_in++;
			}
		}
		spa_list_remove(&n->ready_link);
		n->ready_link.next = NULL;
	}

	debug("node %p %d %d\n", node, node->ready_in, node->required_in);

	if (node->required_in > 0 && node->ready_in == node->required_in) {
		node->action = SPA_GRAPH_ACTION_IN;
		node->state = node->schedule(node);
		debug("node %p scheduled %d %d\n", node, node->action, node->state);
		if (node->state == SPA_RESULT_HAVE_BUFFER) {
			spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link) {
				if (p->io->status == SPA_RESULT_HAVE_BUFFER)
			                p->peer->node->ready_in++;
			}
		}
	}
}

static inline bool spa_graph_scheduler_iterate(struct spa_graph_scheduler *sched)
{
	return false;
}


static inline void spa_graph_scheduler_push(struct spa_graph_scheduler *sched, struct spa_graph_node *node)
{
	struct spa_graph_port *p;
	struct spa_graph_node *n, *t;
	struct spa_list ready;

	debug("node %p start push\n", node);

	spa_list_init(&ready);

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link) {
		struct spa_graph_port *pport = p->peer;
		struct spa_graph_node *pnode = pport->node;
		if (pport->io->status == SPA_RESULT_HAVE_BUFFER)
			pnode->ready_in++;

		debug("node %p peer %p io %d %d %d\n", node, pnode, pport->io->status,
				pnode->ready_in, pnode->required_in);

		if (pnode->required_in > 0 && pnode->ready_in == pnode->required_in)
                        spa_list_insert(ready.prev, &pnode->ready_link);
	}

	spa_list_for_each_safe(n, t, &ready, ready_link) {
		n->action = SPA_GRAPH_ACTION_IN;
		n->state = n->schedule(n);
		debug("peer %p scheduled %d %d\n", n, n->action, n->state);
		if (n->state == SPA_RESULT_HAVE_BUFFER)
			spa_graph_scheduler_push(sched, n);
		else {
			n->ready_in = 0;
			spa_list_for_each(p, &n->ports[SPA_DIRECTION_INPUT], link) {
				if (p->io->status == SPA_RESULT_OK && !(n->flags & SPA_GRAPH_NODE_FLAG_ASYNC))
			                node->ready_in++;
			}
		}
		spa_list_remove(&n->ready_link);
		n->ready_link.next = NULL;
	}

	node->action = SPA_GRAPH_ACTION_OUT;
	node->state = node->schedule(node);
	debug("node %p scheduled %d %d\n", node, node->action, node->state);
	if (node->state == SPA_RESULT_NEED_BUFFER) {
		node->ready_in = 0;
		spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
			if (p->io->status == SPA_RESULT_OK && !(n->flags & SPA_GRAPH_NODE_FLAG_ASYNC))
		                p->peer->node->ready_in++;
		}
	}
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_GRAPH_SCHEDULER_H__ */
