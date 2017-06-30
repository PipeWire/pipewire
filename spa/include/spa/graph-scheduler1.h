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

static inline bool spa_graph_scheduler_iterate(struct spa_graph_scheduler *sched)
{
	bool res;
	struct spa_graph *graph = sched->graph;
	struct spa_graph_port *p;
	struct spa_graph_node *n;

	res = !spa_list_is_empty(&graph->ready);
	if (res) {
		n = spa_list_first(&graph->ready, struct spa_graph_node, ready_link);

		spa_list_remove(&n->ready_link);
		n->ready_link.next = NULL;

		debug("node %p action %d state %d\n", n, n->action, n->state);

		switch (n->action) {
		case SPA_GRAPH_ACTION_IN:
		case SPA_GRAPH_ACTION_OUT:
			n->state = n->schedule(n);
			debug("node %p scheduled action %d state %d\n", n, n->action, n->state);
			if (n->action == SPA_GRAPH_ACTION_IN && n == sched->node)
				break;
			n->action = SPA_GRAPH_ACTION_CHECK;
			spa_list_insert(graph->ready.prev, &n->ready_link);
			break;

		case SPA_GRAPH_ACTION_CHECK:
			if (n->state == SPA_RESULT_NEED_BUFFER) {
				n->ready_in = 0;
				spa_list_for_each(p, &n->ports[SPA_DIRECTION_INPUT], link) {
					struct spa_graph_node *pn = p->peer->node;
					if (p->io->status == SPA_RESULT_NEED_BUFFER) {
						if (pn != sched->node
						    || pn->flags & SPA_GRAPH_NODE_FLAG_ASYNC) {
							pn->action = SPA_GRAPH_ACTION_OUT;
							spa_list_insert(graph->ready.prev,
									&pn->ready_link);
						}
					} else if (p->io->status == SPA_RESULT_OK)
						n->ready_in++;
				}
			} else if (n->state == SPA_RESULT_HAVE_BUFFER) {
				spa_list_for_each(p, &n->ports[SPA_DIRECTION_OUTPUT], link)
					spa_graph_port_check(graph, p->peer);
			}
			break;

		default:
			break;
		}
		res = !spa_list_is_empty(&graph->ready);
	}
	return res;
}

static inline void spa_graph_scheduler_pull(struct spa_graph_scheduler *sched, struct spa_graph_node *node)
{
	debug("node %p start pull\n", node);
	node->action = SPA_GRAPH_ACTION_CHECK;
	node->state = SPA_RESULT_NEED_BUFFER;
	sched->node = node;
	if (node->ready_link.next == NULL)
		spa_list_insert(sched->graph->ready.prev, &node->ready_link);
}

static inline void spa_graph_scheduler_push(struct spa_graph_scheduler *sched, struct spa_graph_node *node)
{
	debug("node %p start push\n", node);
	node->action = SPA_GRAPH_ACTION_OUT;
	sched->node = node;
	if (node->ready_link.next == NULL)
		spa_list_insert(sched->graph->ready.prev, &node->ready_link);
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_GRAPH_SCHEDULER_H__ */
