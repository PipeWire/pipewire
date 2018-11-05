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

		spa_debug("graph %p node %p: state %p add %d status %d", g, n,
				s, s->pending, s->status);

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
