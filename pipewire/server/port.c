/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "pipewire/client/pipewire.h"

#include "pipewire/server/port.h"

/** \cond */
struct impl {
	struct pw_port this;

	uint32_t seq;
};
/** \endcond */


static int schedule_tee(struct spa_graph_node *node)
{
        int res;
        struct pw_port *this = node->user_data;
	struct spa_graph_port *p;
	struct spa_port_io *io = this->rt.mix_port.io;

        if (node->action == SPA_GRAPH_ACTION_IN) {
		if (spa_list_is_empty(&node->ports[SPA_DIRECTION_OUTPUT])) {
			io->status = SPA_RESULT_NEED_BUFFER;
			res = SPA_RESULT_NEED_BUFFER;
		}
		else {
			spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link)
				*p->io = *io;
			io->status = SPA_RESULT_OK;
			io->buffer_id = SPA_ID_INVALID;
			res = SPA_RESULT_HAVE_BUFFER;
		}
	}
        else if (node->action == SPA_GRAPH_ACTION_OUT) {
		spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link)
			*io = *p->io;
		io->status = SPA_RESULT_NEED_BUFFER;
                res = SPA_RESULT_NEED_BUFFER;
	}
        else
                res = SPA_RESULT_ERROR;

        return res;
}

static int schedule_mix(struct spa_graph_node *node)
{
        int res;
        struct pw_port *this = node->user_data;
	struct spa_graph_port *p;
	struct spa_port_io *io = this->rt.mix_port.io;

        if (node->action == SPA_GRAPH_ACTION_IN) {
		spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
			*io = *p->io;
			p->io->status = SPA_RESULT_OK;
			p->io->buffer_id = SPA_ID_INVALID;
		}
		res = SPA_RESULT_HAVE_BUFFER;
	}
        else if (node->action == SPA_GRAPH_ACTION_OUT) {
                io->status = SPA_RESULT_NEED_BUFFER;
		spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link)
			*p->io = *io;
                res = SPA_RESULT_NEED_BUFFER;
	}
        else
                res = SPA_RESULT_ERROR;

        return res;
}

static int do_add_port(struct spa_loop *loop,
		       bool async, uint32_t seq, size_t size, void *data, void *user_data)
{
        struct pw_port *this = user_data;

	spa_graph_port_add(this->rt.graph,
			   &this->node->rt.node,
			   &this->rt.port,
			   this->direction,
			   this->port_id,
			   0,
			   &this->io);
	spa_graph_node_add(this->rt.graph,
			   &this->rt.mix_node,
			   this->direction == PW_DIRECTION_INPUT ? schedule_mix : schedule_tee,
			   this);
	spa_graph_port_add(this->rt.graph,
			   &this->rt.mix_node,
			   &this->rt.mix_port,
			   pw_direction_reverse(this->direction),
			   0,
			   0,
			   &this->io);
	spa_graph_port_link(this->rt.graph,
			    &this->rt.port,
			    &this->rt.mix_port);

	return SPA_RESULT_OK;
}

struct pw_port *pw_port_new(struct pw_node *node, enum pw_direction direction, uint32_t port_id)
{
	struct impl *impl;
	struct pw_port *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	this->node = node;
	this->direction = direction;
	this->port_id = port_id;
	this->state = PW_PORT_STATE_CONFIGURE;
	this->io.status = SPA_RESULT_OK;
	this->io.buffer_id = SPA_ID_INVALID;

	spa_list_init(&this->links);

	pw_signal_init(&this->destroy_signal);

	this->rt.graph = node->rt.sched->graph;

	pw_loop_invoke(node->data_loop->loop, do_add_port, SPA_ID_INVALID, 0, NULL, false, this);

	return this;
}

static int do_remove_port(struct spa_loop *loop,
			  bool async, uint32_t seq, size_t size, void *data, void *user_data)
{
        struct pw_port *this = user_data;
	struct spa_graph_port *p;

	spa_graph_port_unlink(this->rt.graph,
			      &this->rt.port);
	spa_graph_port_remove(this->rt.graph,
			      &this->rt.port);

	spa_list_for_each(p, &this->rt.mix_node.ports[this->direction], link)
		spa_graph_port_remove(this->rt.graph, p);

	spa_graph_port_remove(this->rt.graph,
			      &this->rt.mix_port);
	spa_graph_node_remove(this->rt.graph,
			      &this->rt.mix_node);

	return SPA_RESULT_OK;
}

void pw_port_destroy(struct pw_port *port)
{
	pw_log_debug("port %p: destroy", port);

	pw_signal_emit(&port->destroy_signal, port);

	pw_loop_invoke(port->node->data_loop->loop, do_remove_port, SPA_ID_INVALID, 0, NULL, true, port);

	spa_list_remove(&port->link);

	free(port);
}

static void port_update_state(struct pw_port *port, enum pw_port_state state)
{
	if (port->state != state) {
		pw_log_debug("port %p: state %d -> %d", port, port->state, state);
		port->state = state;
	}
}

int pw_port_pause_rt(struct pw_port *port)
{
	int res;

	if (port->state <= PW_PORT_STATE_PAUSED)
		return SPA_RESULT_OK;

	res = spa_node_port_send_command(port->node->node,
					 port->direction,
					 port->port_id,
					 &SPA_COMMAND_INIT(port->node->core->type.command_node.
							   Pause));
	port_update_state (port, PW_PORT_STATE_PAUSED);
	return res;
}

static int
do_port_pause(struct spa_loop *loop,
	      bool async, uint32_t seq, size_t size, void *data, void *user_data)
{
	struct pw_port *port = user_data;
	return pw_port_pause_rt(port);
}

int pw_port_set_format(struct pw_port *port, uint32_t flags, struct spa_format *format)
{
	int res;

	res = spa_node_port_set_format(port->node->node, port->direction, port->port_id, flags, format);

	pw_log_debug("port %p: set format %d", port, res);

	if (!SPA_RESULT_IS_ASYNC(res)) {
		if (format == NULL) {
			port->buffers = NULL;
			port->n_buffers = 0;
			if (port->allocated)
				pw_memblock_free(&port->buffer_mem);
			port->allocated = false;
			port_update_state (port, PW_PORT_STATE_CONFIGURE);
		}
		else {
			port_update_state (port, PW_PORT_STATE_READY);
		}
	}
	return res;
}

int pw_port_use_buffers(struct pw_port *port, struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct impl *impl = SPA_CONTAINER_OF(port, struct impl, this);
	int res;

	if (n_buffers == 0 && port->state <= PW_PORT_STATE_READY)
		return SPA_RESULT_OK;

	if (n_buffers > 0 && port->state < PW_PORT_STATE_READY)
		return SPA_RESULT_NO_FORMAT;

	if (port->state > PW_PORT_STATE_PAUSED) {
		res = pw_loop_invoke(port->node->data_loop->loop,
				     do_port_pause, impl->seq++, 0, NULL, true, port);
		port_update_state (port, PW_PORT_STATE_PAUSED);
	}

	pw_log_debug("port %p: use %d buffers", port, n_buffers);
	res = spa_node_port_use_buffers(port->node->node, port->direction, port->port_id, buffers, n_buffers);
	port->buffers = buffers;
	port->n_buffers = n_buffers;
	if (port->allocated)
		pw_memblock_free(&port->buffer_mem);
	port->allocated = false;

	if (port->n_buffers == 0)
		port_update_state (port, PW_PORT_STATE_READY);
	else if (!SPA_RESULT_IS_ASYNC(res))
		port_update_state (port, PW_PORT_STATE_PAUSED);

	return res;
}

int pw_port_alloc_buffers(struct pw_port *port,
			  struct spa_param **params, uint32_t n_params,
			  struct spa_buffer **buffers, uint32_t *n_buffers)
{
	struct impl *impl = SPA_CONTAINER_OF(port, struct impl, this);
	int res;

	if (port->state < PW_PORT_STATE_READY)
		return SPA_RESULT_NO_FORMAT;

	if (port->state > PW_PORT_STATE_PAUSED) {
		res = pw_loop_invoke(port->node->data_loop->loop,
				     do_port_pause, impl->seq++, 0, NULL, true, port);
		port_update_state (port, PW_PORT_STATE_PAUSED);
	}

	pw_log_debug("port %p: alloc %d buffers", port, *n_buffers);
	res = spa_node_port_alloc_buffers(port->node->node, port->direction, port->port_id,
					  params, n_params, buffers, n_buffers);
	port->buffers = buffers;
	port->n_buffers = *n_buffers;
	port->allocated = true;

	if (!SPA_RESULT_IS_ASYNC(res))
		port_update_state (port, PW_PORT_STATE_PAUSED);

	return res;
}
