/* Spa
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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>

#include <spa/support/log-impl.h>
#include <spa/support/loop.h>
#include <spa/node/node.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/param/audio/format-utils.h>
#include <spa/graph/graph.h>
#include <spa/graph/graph-scheduler2.h>

static SPA_LOG_IMPL(default_log);

struct version {
	uint16_t current;
	uint16_t pending;
};

struct data {
	struct spa_log *log;
	struct spa_loop data_loop;

	int writers;
	struct version version;
	struct spa_graph graph[2];
	struct spa_graph_state graph_state[2];

	struct spa_graph_node source_node[2];
	struct spa_graph_port source_out[2];
	struct spa_graph_port volume_in[2];
	struct spa_graph_node volume_node[2];
	struct spa_graph_port volume_out[2];
	struct spa_graph_port sink_in[2];
	struct spa_graph_node sink_node[2];
};

static int copy_graph(struct data *data, int current)
{
	int c = (current)&1;
	int v = (current+1)&1;
	struct spa_graph *ng, *og;
	struct spa_graph_node *nn, *on;
	struct spa_graph_port *np, *op;
	int d;

	d = (v - c);

	og = &data->graph[c];
	ng = &data->graph[v];
	spa_list_init(&ng->nodes);

	printf("copy graph %d -> %d\n", c, v);
	spa_list_for_each(on, &og->nodes, link) {
		nn = &on[d];
		*nn = *on;
	        spa_list_append(&ng->nodes, &nn->link);

		spa_list_init(&nn->ports[SPA_DIRECTION_INPUT]);
		spa_list_for_each(op, &on->ports[SPA_DIRECTION_INPUT], link) {
			np = &op[d];
			*np = *op;
			np->node = nn;
			np->peer = &op->peer[d];
		        spa_list_append(&nn->ports[SPA_DIRECTION_INPUT], &np->link);
		}

		spa_list_init(&nn->ports[SPA_DIRECTION_OUTPUT]);
		spa_list_for_each(op, &on->ports[SPA_DIRECTION_OUTPUT], link) {
			np = &op[d];
			*np = *op;
			np->node = nn;
			np->peer = &op->peer[d];
		        spa_list_append(&nn->ports[SPA_DIRECTION_OUTPUT], &np->link);
		}
	}
	return 0;
}

static int start_write(struct data *data)
{
	if (data->writers++ == 0) {
		printf("writer start %d %d\n", data->version.current, data->version.pending);
		if (data->version.current == data->version.pending)
			copy_graph(data, data->version.current);
		data->version.pending = data->version.current;
	}
	return (data->version.current + 1) & 1;
}

static int end_write(struct data *data)
{
	if (--data->writers == 0) {
		data->version.pending++;
		printf("writer end %d %d\n", data->version.current, data->version.pending);
	}
	return 0;
}

static bool switch_graph(struct data *data)
{
	bool res = data->version.current != data->version.pending;
	if (res) {
		printf("switch graph %d -> %d\n", data->version.current, data->version.pending);
		data->version.current = data->version.pending;
	}
	return res;
}

static int print_graph(struct data *data, int v)
{
	struct spa_graph *g;
	struct spa_graph_node *n;
	struct spa_graph_port *p;

	g = &data->graph[v];
	printf("graph %p (version %d):\n", g, v);

	spa_list_for_each(n, &g->nodes, link) {
		printf("  node %p\n", n);
		spa_list_for_each(p, &n->ports[SPA_DIRECTION_INPUT], link) {
			printf("    in:  %p -> %p\n", p, p->peer);
		}
		spa_list_for_each(p, &n->ports[SPA_DIRECTION_OUTPUT], link) {
			printf("    out: %p -> %p\n", p, p->peer);
		}
	}
	return 0;
}

static int make_graph1(struct data *data)
{
	int v = start_write(data);

	spa_graph_node_init(&data->source_node[v], NULL);
	spa_graph_node_add(&data->graph[v], &data->source_node[v]);
	spa_graph_port_add(&data->source_node[v], &data->source_out[v]);

	spa_graph_node_init(&data->volume_node[v], NULL);
	spa_graph_node_add(&data->graph[v], &data->volume_node[v]);
	spa_graph_port_add(&data->volume_node[v], &data->volume_in[v]);

	spa_graph_port_link(&data->source_out[v], &data->volume_in[v]);

	spa_graph_port_add(&data->volume_node[v], &data->volume_out[v]);

	spa_graph_node_init(&data->sink_node[v], NULL);
	spa_graph_node_add(&data->graph[v], &data->sink_node[v]);
	spa_graph_port_add(&data->sink_node[v], &data->sink_in[v]);

	spa_graph_port_link(&data->volume_out[v], &data->sink_in[v]);

	end_write(data);

	return 0;
}

static int make_graph2(struct data *data)
{
	int v = start_write(data);

        spa_graph_port_unlink(&data->volume_in[v]);
        spa_graph_port_unlink(&data->volume_out[v]);
	spa_graph_node_remove(&data->volume_node[v]);

        spa_graph_port_link(&data->source_out[v], &data->sink_in[v]);

	end_write(data);

	return 0;
}

static int make_graph3(struct data *data)
{
	int v = start_write(data);

        spa_graph_port_unlink(&data->source_out[v]);

	spa_graph_node_add(&data->graph[v], &data->volume_node[v]);

        spa_graph_port_link(&data->source_out[v], &data->volume_in[v]);
        spa_graph_port_link(&data->volume_out[v], &data->sink_in[v]);

	end_write(data);

	return 0;
}

int main(int argc, char *argv[])
{
	struct data data = { NULL };
	const char *str;

	spa_graph_init(&data.graph[0], &data.graph_state[0]);
	spa_graph_init(&data.graph[1], &data.graph_state[1]);

	data.log = &default_log.log;

	if ((str = getenv("SPA_DEBUG")))
		data.log->level = atoi(str);

	print_graph(&data, 0);
	print_graph(&data, 1);
	make_graph1(&data);
	print_graph(&data, 0);
	print_graph(&data, 1);
	switch_graph(&data);
	print_graph(&data, 0);
	print_graph(&data, 1);
	make_graph2(&data);
	print_graph(&data, 0);
	print_graph(&data, 1);
	switch_graph(&data);
	make_graph3(&data);
	print_graph(&data, 0);
	print_graph(&data, 1);

}
