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

static inline int spa_graph_impl_need_input(void *data, struct spa_graph_node *node)
{
	struct spa_graph_port *p;
	struct spa_graph_node *n, *t;
	struct spa_list ready;

	spa_debug("node %p start pull", node);

	spa_list_init(&ready);

	node->ready[SPA_DIRECTION_INPUT] = 0;
	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		struct spa_graph_port *pport;
		struct spa_graph_node *pnode;
		if ((pport = p->peer) == NULL)
			continue;
		pnode = pport->node;
		spa_debug("node %p peer %p io %d %d", node, pnode, pport->io->status, pport->io->buffer_id);
		if (pport->io->status == SPA_STATUS_NEED_BUFFER) {
			if (pnode->ready_link.next == NULL)
				spa_list_append(&ready, &pnode->ready_link);
		}
		else if (pport->io->status == SPA_STATUS_OK &&
		    !(pnode->flags & SPA_GRAPH_NODE_FLAG_ASYNC))
			node->ready[SPA_DIRECTION_INPUT]++;
	}

	spa_list_for_each_safe(n, t, &ready, ready_link) {
		n->state = spa_node_process_output(n->implementation);
		spa_debug("peer %p processed out %d", n, n->state);
		if (n->state == SPA_STATUS_NEED_BUFFER)
			spa_graph_need_input(n->graph, n);
		else {
			spa_list_for_each(p, &n->ports[SPA_DIRECTION_OUTPUT], link) {
				if (p->io->status == SPA_STATUS_HAVE_BUFFER)
			                node->ready[SPA_DIRECTION_INPUT]++;
			}
		}
		spa_list_remove(&n->ready_link);
		n->ready_link.next = NULL;
	}

	spa_debug("node %p ready:%d required:%d", node, node->ready[SPA_DIRECTION_INPUT], node->required[SPA_DIRECTION_INPUT]);

	if (node->required[SPA_DIRECTION_INPUT] > 0 &&
	    node->ready[SPA_DIRECTION_INPUT] == node->required[SPA_DIRECTION_INPUT]) {
		node->state = spa_node_process_input(node->implementation);
		spa_debug("node %p processed in %d", node, node->state);
		if (node->state == SPA_STATUS_HAVE_BUFFER) {
			spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link) {
				if (p->io->status == SPA_STATUS_HAVE_BUFFER)
					if (p->peer)
				                p->peer->node->ready[SPA_DIRECTION_INPUT]++;
			}
		}
	}
	return 0;
}

static inline int spa_graph_impl_have_output(void *data, struct spa_graph_node *node)
{
	struct spa_graph_port *p;
	struct spa_list ready;
	struct spa_graph_node *n, *t;

	spa_debug("node %p start push", node);

	spa_list_init(&ready);

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link) {
		struct spa_graph_port *pport;
		struct spa_graph_node *pnode;
		uint32_t prequired, pready;

		if ((pport = p->peer) == NULL) {
			spa_debug("node %p port %p has no peer", node, p);
			continue;
		}

		pnode = pport->node;
		if (pport->io->status == SPA_STATUS_HAVE_BUFFER)
			pnode->ready[SPA_DIRECTION_INPUT]++;

		pready = pnode->ready[SPA_DIRECTION_INPUT];
		prequired = pnode->required[SPA_DIRECTION_INPUT];

		spa_debug("node %p peer %p io %d %d %d", node, pnode, pport->io->status,
				pready, prequired);

		if (prequired > 0 && pready == prequired)
			if (pnode->ready_link.next == NULL)
				spa_list_append(&ready, &pnode->ready_link);
	}

	spa_list_for_each_safe(n, t, &ready, ready_link) {
		n->state = spa_node_process_input(n->implementation);
		spa_debug("node %p chain processed in %d", n, n->state);
		if (n->state == SPA_STATUS_HAVE_BUFFER)
			spa_graph_have_output(n->graph, n);
		else {
			n->ready[SPA_DIRECTION_INPUT] = 0;
			spa_list_for_each(p, &n->ports[SPA_DIRECTION_INPUT], link) {
				if (p->io->status == SPA_STATUS_OK &&
				    !(n->flags & SPA_GRAPH_NODE_FLAG_ASYNC))
			                n->ready[SPA_DIRECTION_INPUT]++;
			}
		}
		spa_list_remove(&n->ready_link);
		n->ready_link.next = NULL;
	}

	node->state = spa_node_process_output(node->implementation);
	spa_debug("node %p processed out %d", node, node->state);
	if (node->state == SPA_STATUS_NEED_BUFFER) {
		node->ready[SPA_DIRECTION_INPUT] = 0;
		spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
			if (p->io->status == SPA_STATUS_OK && !(node->flags & SPA_GRAPH_NODE_FLAG_ASYNC))
				node->ready[SPA_DIRECTION_INPUT]++;
		}
	}
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
