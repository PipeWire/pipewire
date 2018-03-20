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

static inline int spa_graph_impl_run(void *data)
{
	struct spa_graph_data *d = (struct spa_graph_data *) data;
	struct spa_graph *g = d->graph;
	struct spa_graph_node *n, *tmp;
	struct spa_list pending;

	spa_debug("graph %p run", d->graph);
	spa_graph_state_reset(g->state);

	spa_list_init(&pending);

	spa_list_for_each(n, &g->nodes, link) {
		struct spa_graph_state *s = n->state;

		spa_graph_state_reset(s);

		spa_debug("graph %p node %p: add %d status %d", g, n, s->pending, s->status);

		if (s->pending == 0)
			spa_list_append(&pending, &n->sched_link);
	}
	spa_list_for_each_safe(n, tmp, &pending, sched_link)
		spa_graph_node_process(n);

	return 0;
}

static inline int spa_graph_impl_finish(void *data)
{
	struct spa_graph_data *d = (struct spa_graph_data *) data;
	struct spa_graph *g = d->graph;

	spa_debug("graph %p finish", d->graph);

	if (g->parent)
		spa_graph_node_trigger(g->parent);

	return 0;
}

static const struct spa_graph_callbacks spa_graph_impl_default = {
	SPA_VERSION_GRAPH_CALLBACKS,
	.run = spa_graph_impl_run,
	.finish = spa_graph_impl_finish,
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_GRAPH_SCHEDULER_H__ */
