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
	spa_list_init(&this->rt.links);
	pw_signal_init(&this->destroy_signal);

	return this;
}

void pw_port_destroy(struct pw_port *port)
{
	pw_log_debug("port %p: destroy", port);

	pw_signal_emit(&port->destroy_signal, port);

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

static int
do_add_link(struct spa_loop *loop,
	    bool async, uint32_t seq, size_t size, void *data, void *user_data)
{
	struct pw_port *this = user_data;
	struct pw_link *link = ((struct pw_link **) data)[0];

	if (this->direction == PW_DIRECTION_INPUT) {
		spa_list_insert(this->rt.links.prev, &link->rt.input_link);
		link->rt.input = this;
	} else {
		spa_list_insert(this->rt.links.prev, &link->rt.output_link);
		link->rt.output = this;
	}

	return SPA_RESULT_OK;
}

static struct pw_link *find_link(struct pw_port *output_port, struct pw_port *input_port)
{
	struct pw_link *pl;

	spa_list_for_each(pl, &output_port->links, output_link) {
		if (pl->input == input_port)
			return pl;
	}
	return NULL;
}

struct pw_link *pw_port_link(struct pw_port *output_port,
			     struct pw_port *input_port,
			     struct spa_format *format_filter,
			     struct pw_properties *properties,
			     char **error)
{
	struct pw_node *input_node, *output_node;
	struct pw_link *link;

	output_node = output_port->node;
	input_node = input_port->node;

	pw_log_debug("port link %p:%u -> %p:%u", output_node, output_port->port_id, input_node,
		     input_port->port_id);

	if (output_node == input_node)
		goto same_node;

	if (!spa_list_is_empty(&input_port->links))
		goto was_linked;

	link = find_link(output_port, input_port);

	if (link == NULL) {
		input_node->live = output_node->live;
		if (output_node->clock)
			input_node->clock = output_node->clock;
		pw_log_debug("node %p: clock %p, live %d", output_node, output_node->clock,
			     output_node->live);

		link = pw_link_new(output_node->core,
				   output_port, input_port, format_filter, properties);
		if (link == NULL)
			goto no_mem;

		spa_list_insert(output_port->links.prev, &link->output_link);
		spa_list_insert(input_port->links.prev, &link->input_link);

		output_node->n_used_output_links++;
		input_node->n_used_input_links++;

		pw_loop_invoke(output_node->data_loop->loop,
			       do_add_link,
			       SPA_ID_INVALID, sizeof(struct pw_link *), &link, false, output_port);
		pw_loop_invoke(input_node->data_loop->loop,
			       do_add_link,
			       SPA_ID_INVALID, sizeof(struct pw_link *), &link, false, input_port);
	}
	return link;

      same_node:
	asprintf(error, "can't link a node to itself");
	return NULL;
      was_linked:
	asprintf(error, "input port was already linked");
	return NULL;
      no_mem:
	return NULL;
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
do_remove_link(struct spa_loop *loop,
	       bool async, uint32_t seq, size_t size, void *data, void *user_data)
{
	struct pw_port *port = user_data;
	struct pw_link *link = ((struct pw_link **) data)[0];

	if (port->direction == PW_DIRECTION_INPUT) {
		pw_port_pause_rt(link->rt.input);
		spa_list_remove(&link->rt.input_link);
		link->rt.input = NULL;
	} else {
		pw_port_pause_rt(link->rt.output);
		spa_list_remove(&link->rt.output_link);
		link->rt.output = NULL;
	}
	return SPA_RESULT_OK;
}

int pw_port_unlink(struct pw_port *port, struct pw_link *link)
{
	int res;
	struct impl *impl = SPA_CONTAINER_OF(port, struct impl, this);
	struct pw_node *node = port->node;

	pw_log_debug("port %p: start unlink %p", port, link);

	res = pw_loop_invoke(node->data_loop->loop,
			     do_remove_link, impl->seq++, sizeof(struct pw_link *), &link, true, port);

	if (port->state > PW_PORT_STATE_PAUSED)
		port_update_state (port, PW_PORT_STATE_PAUSED);

	pw_log_debug("port %p: finish unlink", port);
	if (port->direction == PW_DIRECTION_OUTPUT) {
		if (link->output) {
			spa_list_remove(&link->output_link);
			node->n_used_output_links--;
			link->output = NULL;
		}
	} else {
		if (link->input) {
			spa_list_remove(&link->input_link);
			node->n_used_input_links--;
			link->input = NULL;
		}
	}

	if (!port->allocated)
		pw_port_use_buffers(port, NULL, 0);

	if (node->n_used_output_links == 0 && node->n_used_input_links == 0)
		pw_node_update_state(node, PW_NODE_STATE_IDLE, NULL);

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
