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
        struct spa_graph_node *node;
};

static inline void spa_graph_scheduler_init(struct spa_graph_scheduler *sched,
					    struct spa_graph *graph)
{
	sched->graph = graph;
	sched->node = NULL;
}

static inline int spa_graph_node_scheduler_input(void *data)
{
	struct spa_node *n = data;
	return spa_node_process_input(n);
}

static inline int spa_graph_node_scheduler_output(void *data)
{
	struct spa_node *n = data;
	return spa_node_process_output(n);
}


static const struct spa_graph_node_callbacks spa_graph_node_scheduler_default = {
	SPA_VERSION_GRAPH_NODE_CALLBACKS,
	spa_graph_node_scheduler_input,
	spa_graph_node_scheduler_output,
};

static inline int spa_graph_port_scheduler_reuse_buffer(void *data,
							uint32_t buffer_id)
{
	struct spa_graph_port *port = data;
	struct spa_node *node = port->node->callbacks_data;
	debug("port %p reuse buffer %d\n", port, buffer_id);
	return spa_node_port_reuse_buffer(node, port->port_id, buffer_id);
}

static const struct spa_graph_port_callbacks spa_graph_port_scheduler_default = {
	SPA_VERSION_GRAPH_PORT_CALLBACKS,
	spa_graph_port_scheduler_reuse_buffer,
};

static inline void spa_graph_scheduler_pull(struct spa_graph_scheduler *sched, struct spa_graph_node *node)
{
	struct spa_graph_port *p;
	struct spa_graph_node *n, *t;
	struct spa_list ready;

	debug("node %p start pull\n", node);

	spa_list_init(&ready);

	node->ready_in = 0;
	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		struct spa_graph_port *pport;
		struct spa_graph_node *pnode;
		if ((pport = p->peer) == NULL)
			continue;
		pnode = pport->node;
		debug("node %p peer %p io %d\n", node, pnode, pport->io->status);
		if (pport->io->status == SPA_RESULT_NEED_BUFFER) {
			spa_list_insert(ready.prev, &pnode->ready_link);
		}
		else if (pport->io->status == SPA_RESULT_OK && !(pnode->flags & SPA_GRAPH_NODE_FLAG_ASYNC))
			node->ready_in++;
	}

	spa_list_for_each_safe(n, t, &ready, ready_link) {
		n->state = n->callbacks->process_output(n->callbacks_data);
		debug("peer %p processed out %d\n", n, n->state);
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
		node->state = node->callbacks->process_input(node->callbacks_data);
		debug("node %p processed in %d\n", node, node->state);
		if (node->state == SPA_RESULT_HAVE_BUFFER) {
			spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link) {
				if (p->io->status == SPA_RESULT_HAVE_BUFFER)
					if (p->peer)
				                p->peer->node->ready_in++;
			}
		}
	}
}

static inline bool spa_graph_scheduler_iterate(struct spa_graph_scheduler *sched)
{
	return false;
}

static inline void spa_graph_scheduler_push(struct spa_graph_scheduler *sched, struct spa_graph_node *node);

static inline void spa_graph_scheduler_chain(struct spa_graph_scheduler *sched,
					     struct spa_list *ready)
{
	struct spa_graph_node *n, *t;
	struct spa_graph_port *p;


	spa_list_for_each_safe(n, t, ready, ready_link) {
		n->state = n->callbacks->process_input(n->callbacks_data);
		debug("node %p chain processed in %d\n", n, n->state);
		if (n->state == SPA_RESULT_HAVE_BUFFER)
			spa_graph_scheduler_push(sched, n);
		else {
			n->ready_in = 0;
			spa_list_for_each(p, &n->ports[SPA_DIRECTION_INPUT], link) {
				if (p->io->status == SPA_RESULT_OK && !(n->flags & SPA_GRAPH_NODE_FLAG_ASYNC))
			                n->ready_in++;
			}
		}
		spa_list_remove(&n->ready_link);
		n->ready_link.next = NULL;
	}
}

static inline void spa_graph_scheduler_push(struct spa_graph_scheduler *sched, struct spa_graph_node *node)
{
	struct spa_graph_port *p;
	struct spa_list ready;

	debug("node %p start push\n", node);

	spa_list_init(&ready);

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link) {
		struct spa_graph_port *pport;
		struct spa_graph_node *pnode;
		if ((pport = p->peer) == NULL)
			continue;
		pnode = pport->node;
		if (pport->io->status == SPA_RESULT_HAVE_BUFFER)
			pnode->ready_in++;

		debug("node %p peer %p io %d %d %d\n", node, pnode, pport->io->status,
				pnode->ready_in, pnode->required_in);

		if (pnode->required_in > 0 && pnode->ready_in == pnode->required_in)
                        spa_list_insert(ready.prev, &pnode->ready_link);
	}

	spa_graph_scheduler_chain(sched, &ready);

	node->state = node->callbacks->process_output(node->callbacks_data);
	debug("node %p processed out %d\n", node, node->state);
	if (node->state == SPA_RESULT_NEED_BUFFER) {
		node->ready_in = 0;
		spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
			if (p->io->status == SPA_RESULT_OK && !(node->flags & SPA_GRAPH_NODE_FLAG_ASYNC))
				node->ready_in++;
		}
	}
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_GRAPH_SCHEDULER_H__ */
