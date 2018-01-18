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

static inline int spa_graph_impl_need_input(void *data, struct spa_graph_node *node)
{
	struct spa_graph_port *p;

	spa_debug("node %p start pull", node);

	node->ready[SPA_DIRECTION_INPUT] = 0;
	node->required[SPA_DIRECTION_INPUT] = 0;
	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		struct spa_graph_port *pport;
		struct spa_graph_node *pnode;
		uint32_t prequired, pready;

		if ((pport = p->peer) == NULL || (pport->flags & SPA_GRAPH_PORT_FLAG_DISABLED)) {
			spa_debug("node %p port %p has no peer", node, p);
			continue;
		}
		pnode = pport->node;

		if (pport->io->status == SPA_STATUS_NEED_BUFFER) {
			pnode->ready[SPA_DIRECTION_OUTPUT]++;
			if (!(p->flags & SPA_PORT_INFO_FLAG_OPTIONAL))
				node->required[SPA_DIRECTION_INPUT]++;
		}

		pready = pnode->ready[SPA_DIRECTION_OUTPUT];
		prequired = pnode->required[SPA_DIRECTION_OUTPUT];

		spa_debug("node %p peer %p io %d %d %d %d", node, pnode, pport->io->status,
				pport->io->buffer_id, pready, prequired);

		if (prequired > 0 && pready >= prequired) {
			pnode->state = spa_node_process_output(pnode->implementation);

			spa_debug("peer %p processed out %d", pnode, pnode->state);
			if (pnode->state == SPA_STATUS_HAVE_BUFFER)
				spa_graph_have_output(pnode->graph, pnode);
			else if (pnode->state == SPA_STATUS_NEED_BUFFER)
				spa_graph_need_input(pnode->graph, pnode);
		}
	}
	spa_debug("node %p end pull", node);
	return 0;
}

static inline int spa_graph_impl_have_output(void *data, struct spa_graph_node *node)
{
	struct spa_graph_port *p;

	spa_debug("node %p start push", node);

	node->ready[SPA_DIRECTION_OUTPUT] = 0;
	node->required[SPA_DIRECTION_OUTPUT] = 0;
	spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link) {
		struct spa_graph_port *pport;
		struct spa_graph_node *pnode;
		uint32_t prequired, pready;

		if ((pport = p->peer) == NULL || (pport->flags & SPA_GRAPH_PORT_FLAG_DISABLED)) {
			spa_debug("node %p port %p has no peer", node, p);
			continue;
		}
		pnode = pport->node;

		if (pport->io->status == SPA_STATUS_HAVE_BUFFER) {
			pnode->ready[SPA_DIRECTION_INPUT]++;
			if (!(p->flags & SPA_PORT_INFO_FLAG_OPTIONAL))
				node->required[SPA_DIRECTION_OUTPUT]++;
		}

		pready = pnode->ready[SPA_DIRECTION_INPUT];
		prequired = pnode->required[SPA_DIRECTION_INPUT];

		spa_debug("node %p peer %p io %d %d %d %d", node, pnode, pport->io->status,
				pport->io->buffer_id, pready, prequired);

		if (prequired > 0 && pready >= prequired) {
			pnode->state = spa_node_process_input(pnode->implementation);

			spa_debug("peer %p processed in %d", pnode, pnode->state);
			if (pnode->state == SPA_STATUS_HAVE_BUFFER)
				spa_graph_have_output(pnode->graph, pnode);
			else if (pnode->state == SPA_STATUS_NEED_BUFFER)
				spa_graph_need_input(pnode->graph, pnode);
		}
	}
	spa_debug("node %p end push", node);
	return 0;
}

static const struct spa_graph_callbacks spa_graph_impl_default = {
	SPA_VERSION_GRAPH_CALLBACKS,
	.need_input = spa_graph_impl_need_input,
	.have_output = spa_graph_impl_have_output,
};


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_GRAPH_SCHEDULER_H__ */
