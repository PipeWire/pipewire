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

#include <spa/graph/graph.h>

struct spa_graph_data {
	struct spa_graph *graph;
};

static inline void spa_graph_data_init(struct spa_graph_data *data,
				       struct spa_graph *graph)
{
	data->graph = graph;
}

static inline int spa_graph_trigger(struct spa_graph *g, struct spa_graph_node *node)
{
	uint32_t val;

	spa_debug("node %p: pending %d required %d", node,
                        node->state->pending, node->state->required);

	if (node->state->pending == 0) {
		spa_debug("node %p: nothing pending", node);
		return node->state->status;
	}

	val = __atomic_sub_fetch(&node->state->pending, 1, __ATOMIC_SEQ_CST);
        if (val == 0)
		spa_graph_node_process(node);

        return node->state->status;
}

static inline int spa_graph_impl_need_input(void *data, struct spa_graph_node *node)
{
#if 0
	struct spa_graph_data *d = (struct spa_graph_data *) data;
	struct spa_list queue, pending;
	struct spa_graph_node *n, *pn;
	struct spa_graph_port *p, *pp;

	spa_debug("node %p start pull", node);

	spa_list_init(&queue);
	spa_list_init(&pending);

	node->state->status = SPA_STATUS_NEED_BUFFER;

	if (node->sched_link.next == NULL)
		spa_list_append(&queue, &node->sched_link);

	while (!spa_list_is_empty(&queue)) {

		n = spa_list_first(&queue, struct spa_graph_node, sched_link);
		spa_list_remove(&n->sched_link);
		n->sched_link.next = NULL;

		n->state->pending = n->state->required + 1;
                spa_debug("node %p: add %d %d status %d", n,
                                n->state->pending, n->state->required,
                                n->state->status);

                spa_list_prepend(&pending, &n->sched_link);

                if (n->state->status == SPA_STATUS_HAVE_BUFFER)
                        continue;

		spa_list_for_each(p, &n->ports[SPA_DIRECTION_INPUT], link) {
			pp = p->peer;
			if (pp == NULL)
				continue;
			pn = pp->node;

                        spa_debug("node %p: %p in io:%d state:%d %p", n, pn, pp->io->status,
                                        pn->state->status, pn->sched_link.next);

                        if (pn->sched_link.next != NULL)
                                continue;

                        if (pp->io->status == SPA_STATUS_NEED_BUFFER &&
			    pn->state->status == SPA_STATUS_HAVE_BUFFER) {
				pn->state->status = spa_graph_node_process(pn);
                        } else {
                                n->state->pending--;
                        }
                        spa_list_append(&queue, &pn->sched_link);
		}
	}
	while (!spa_list_is_empty(&pending)) {
		n = spa_list_first(&pending, struct spa_graph_node, sched_link);
		spa_list_remove(&n->sched_link);
                n->sched_link.next = NULL;

                spa_debug("schedule node %p: %d", n, n->state->status);
		spa_graph_trigger(d->graph, n);
        }
#endif
	return 0;
}

static inline int spa_graph_impl_have_output(void *data, struct spa_graph_node *node)
{
	struct spa_graph_data *d = (struct spa_graph_data *) data;
	struct spa_graph_port *p;

	spa_debug("node %p start push", node);

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link)
		if (p->peer)
			spa_graph_trigger(d->graph, p->peer->node);

	return 0;
}

static inline int spa_graph_impl_run(void *data)
{
	struct spa_graph_data *d = (struct spa_graph_data *) data;
	struct spa_graph *g = d->graph;
	struct spa_graph_node *n;

	spa_debug("graph %p run", d->graph);

	spa_list_for_each(n, &g->nodes, link) {
		n->state->pending = n->state->required + 1;
                spa_debug("node %p: add %d %d status %d", n,
                                n->state->pending, n->state->required,
                                n->state->status);
	}
	spa_list_for_each(n, &g->nodes, link)
		spa_graph_trigger(d->graph, n);

	return 0;
}

static const struct spa_graph_callbacks spa_graph_impl_default = {
	SPA_VERSION_GRAPH_CALLBACKS,
	.need_input = spa_graph_impl_need_input,
	.have_output = spa_graph_impl_have_output,
	.run = spa_graph_impl_run,
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_GRAPH_SCHEDULER_H__ */
