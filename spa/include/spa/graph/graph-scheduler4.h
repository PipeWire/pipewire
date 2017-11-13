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

static inline int spa_graph_impl_need_input(void *data, struct spa_graph_node *node);
static inline int spa_graph_impl_have_output(void *data, struct spa_graph_node *node);

static inline void check_input(struct spa_graph_node *node)
{
	struct spa_graph_port *p;

	node->ready[SPA_DIRECTION_INPUT] = 0;
	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		struct spa_graph_port *pport;
		struct spa_graph_node *pnode;

		if ((pport = p->peer) == NULL)
			continue;

		pnode = pport->node;
		debug("node %p input peer %p io %d %d\n", node, pnode, pport->io->status, pport->io->buffer_id);

		pnode->ready[SPA_DIRECTION_OUTPUT]++;
		if (pport->io->status == SPA_STATUS_OK)
			node->ready[SPA_DIRECTION_INPUT]++;

		debug("node %p input peer %p out %d %d\n", node, pnode,
				pnode->required[SPA_DIRECTION_OUTPUT],
				pnode->ready[SPA_DIRECTION_OUTPUT]);
	}
}

static inline void check_output(struct spa_graph_node *node)
{
	struct spa_graph_port *p;

	node->ready[SPA_DIRECTION_OUTPUT] = 0;
	node->required[SPA_DIRECTION_OUTPUT] = 0;

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link) {
		struct spa_graph_port *pport;
		struct spa_graph_node *pnode;

		if ((pport = p->peer) == NULL)
			continue;

		pnode = pport->node;
		debug("node %p output peer %p io %d %d\n", node, pnode, pport->io->status, pport->io->buffer_id);

		if (pport->io->status == SPA_STATUS_HAVE_BUFFER) {
			pnode->ready[SPA_DIRECTION_INPUT]++;
			node->required[SPA_DIRECTION_OUTPUT]++;
		}
		debug("node %p output peer %p out %d %d\n", node, pnode,
				pnode->required[SPA_DIRECTION_INPUT],
				pnode->ready[SPA_DIRECTION_INPUT]);
	}
}


static inline void spa_graph_impl_activate(void *data, struct spa_graph_node *node)
{
	int res;

	debug("node %p activate %d\n", node, node->state);
	if (node->state == SPA_STATUS_NEED_BUFFER) {
                res = spa_node_process_input(node->implementation);
		debug("node %p process in %d\n", node, res);
	}
	else if (node->state == SPA_STATUS_HAVE_BUFFER) {
                res = spa_node_process_output(node->implementation);
		debug("node %p process out %d\n", node, res);
	}
	else
		return;

	if (res == SPA_STATUS_NEED_BUFFER || (res == SPA_STATUS_OK && node->state == SPA_STATUS_NEED_BUFFER)) {
		check_input(node);
	}
	else if (res == SPA_STATUS_HAVE_BUFFER) {
		check_output(node);
	}
	node->state = res;

	debug("node %p activate end %d\n", node, res);
}

static inline int spa_graph_impl_need_input(void *data, struct spa_graph_node *node)
{
	struct spa_graph_port *p;

	debug("node %p start pull\n", node);

	node->state = SPA_STATUS_NEED_BUFFER;
	node->ready[SPA_DIRECTION_INPUT] = 0;

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		struct spa_graph_port *pport;
		struct spa_graph_node *pnode;
		uint32_t prequired;

		if ((pport = p->peer) == NULL)
			continue;
		pnode = pport->node;
		prequired = pnode->required[SPA_DIRECTION_OUTPUT];
		debug("node %p pull peer %p io %d %d\n", node, pnode, pport->io->status, pport->io->buffer_id);

		pnode->ready[SPA_DIRECTION_OUTPUT]++;
		if (pport->io->status == SPA_STATUS_OK)
			node->ready[SPA_DIRECTION_INPUT]++;

		debug("node %p pull peer %p out %d %d\n", node, pnode, prequired, pnode->ready[SPA_DIRECTION_OUTPUT]);
		if (prequired > 0 && pnode->ready[SPA_DIRECTION_OUTPUT] >= prequired) {
			pnode->state = SPA_STATUS_HAVE_BUFFER;
			spa_graph_impl_activate(data, pnode);
		}
	}

	debug("node %p end pull\n", node);

	return 0;
}

static inline int spa_graph_impl_have_output(void *data, struct spa_graph_node *node)
{
	struct spa_graph_port *p;
	uint32_t required;

	debug("node %p start push\n", node);

	node->state = SPA_STATUS_HAVE_BUFFER;

	node->ready[SPA_DIRECTION_OUTPUT] = 0;
	node->required[SPA_DIRECTION_OUTPUT] = 0;

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link) {
		struct spa_graph_port *pport;
		struct spa_graph_node *pnode;
		uint32_t prequired;

		if ((pport = p->peer) == NULL)
			continue;

		pnode = pport->node;
		prequired = pnode->required[SPA_DIRECTION_INPUT];
		debug("node %p push peer %p io %d %d\n", node, pnode, pport->io->status, pport->io->buffer_id);

		if (pport->io->status == SPA_STATUS_HAVE_BUFFER) {
			pnode->ready[SPA_DIRECTION_INPUT]++;
			node->required[SPA_DIRECTION_OUTPUT]++;
		}
		debug("node %p push peer %p in %d %d\n", node, pnode, prequired, pnode->ready[SPA_DIRECTION_INPUT]);
		if (prequired > 0 && pnode->ready[SPA_DIRECTION_INPUT] >= prequired) {
			pnode->state = SPA_STATUS_NEED_BUFFER;
			spa_graph_impl_activate(data, pnode);
		}
	}
	required = node->required[SPA_DIRECTION_OUTPUT];
	if (required > 0 && node->ready[SPA_DIRECTION_OUTPUT] >= required) {

	}
	debug("node %p end push\n", node);

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
