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
#include "pipewire/client/interfaces.h"

#include "pipewire/server/node.h"
#include "pipewire/server/data-loop.h"
#include "pipewire/server/main-loop.h"
#include "pipewire/server/work-queue.h"

/** \cond */
struct impl {
	struct pw_node this;

	struct pw_work_queue *work;

	bool async_init;
};

/** \endcond */

static void init_complete(struct pw_node *this);

static void update_port_ids(struct pw_node *node)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	uint32_t *input_port_ids, *output_port_ids;
	uint32_t n_input_ports, n_output_ports, max_input_ports, max_output_ports;
	uint32_t i;
	struct spa_list *ports;
	int res;

	if (node->node == NULL)
		return;

	spa_node_get_n_ports(node->node,
			     &n_input_ports, &max_input_ports, &n_output_ports, &max_output_ports);

	node->n_input_ports = n_input_ports;
	node->max_input_ports = max_input_ports;
	node->n_output_ports = n_output_ports;
	node->max_output_ports = max_output_ports;

	node->input_port_map = calloc(max_input_ports, sizeof(struct pw_port *));
	node->output_port_map = calloc(max_output_ports, sizeof(struct pw_port *));

	input_port_ids = alloca(sizeof(uint32_t) * n_input_ports);
	output_port_ids = alloca(sizeof(uint32_t) * n_output_ports);

	spa_node_get_port_ids(node->node,
			      max_input_ports, input_port_ids, max_output_ports, output_port_ids);

	pw_log_debug("node %p: update_port ids %u/%u, %u/%u", node,
		     n_input_ports, max_input_ports, n_output_ports, max_output_ports);

	i = 0;
	ports = &node->input_ports;
	while (true) {
		struct pw_port *p = (ports == &node->input_ports) ? NULL :
		    SPA_CONTAINER_OF(ports, struct pw_port, link);

		if (p && i < n_input_ports && p->port_id == input_port_ids[i]) {
			node->input_port_map[p->port_id] = p;
			pw_log_debug("node %p: exiting input port %d", node, input_port_ids[i]);
			i++;
			ports = ports->next;
		} else if ((p && i < n_input_ports && input_port_ids[i] < p->port_id)
			   || i < n_input_ports) {
			struct pw_port *np;
			pw_log_debug("node %p: input port added %d", node, input_port_ids[i]);

			np = pw_port_new(node, PW_DIRECTION_INPUT, input_port_ids[i]);
			if ((res =
			     spa_node_port_set_io(node->node, SPA_DIRECTION_INPUT, np->port_id,
						  &np->io)) < 0)
				pw_log_warn("node %p: can't set input IO %d", node, res);

			spa_list_insert(ports, &np->link);
			ports = np->link.next;
			node->input_port_map[np->port_id] = np;

			if (!impl->async_init)
				pw_signal_emit(&node->port_added, node, np);
			i++;
		} else if (p) {
			node->input_port_map[p->port_id] = NULL;
			ports = ports->next;
			if (!impl->async_init)
				pw_signal_emit(&node->port_removed, node, p);
			pw_log_debug("node %p: input port removed %d", node, p->port_id);
			pw_port_destroy(p);
		} else {
			pw_log_debug("node %p: no more input ports", node);
			break;
		}
	}

	i = 0;
	ports = &node->output_ports;
	while (true) {
		struct pw_port *p = (ports == &node->output_ports) ? NULL :
		    SPA_CONTAINER_OF(ports, struct pw_port, link);

		if (p && i < n_output_ports && p->port_id == output_port_ids[i]) {
			pw_log_debug("node %p: exiting output port %d", node, output_port_ids[i]);
			i++;
			ports = ports->next;
			node->output_port_map[p->port_id] = p;
		} else if ((p && i < n_output_ports && output_port_ids[i] < p->port_id)
			   || i < n_output_ports) {
			struct pw_port *np;
			pw_log_debug("node %p: output port added %d", node, output_port_ids[i]);

			np = pw_port_new(node, PW_DIRECTION_OUTPUT, output_port_ids[i]);
			if ((res =
			     spa_node_port_set_io(node->node, SPA_DIRECTION_OUTPUT, np->port_id,
						  &np->io)) < 0)
				pw_log_warn("node %p: can't set output IO %d", node, res);

			spa_list_insert(ports, &np->link);
			ports = np->link.next;
			node->output_port_map[np->port_id] = np;

			if (!impl->async_init)
				pw_signal_emit(&node->port_added, node, np);
			i++;
		} else if (p) {
			node->output_port_map[p->port_id] = NULL;
			ports = ports->next;
			if (!impl->async_init)
				pw_signal_emit(&node->port_removed, node, p);
			pw_log_debug("node %p: output port removed %d", node, p->port_id);
			pw_port_destroy(p);
		} else {
			pw_log_debug("node %p: no more output ports", node);
			break;
		}
	}
	pw_signal_emit(&node->initialized, node);
}

static int pause_node(struct pw_node *this)
{
	int res;

	if (this->state <= PW_NODE_STATE_IDLE)
		return SPA_RESULT_OK;

	pw_log_debug("node %p: pause node", this);

	if ((res = spa_node_send_command(this->node,
					 &SPA_COMMAND_INIT(this->core->type.command_node.Pause))) <
	    0)
		pw_log_debug("got error %d", res);

	return res;
}

static int start_node(struct pw_node *this)
{
	int res;

	pw_log_debug("node %p: start node", this);

	if ((res = spa_node_send_command(this->node,
					 &SPA_COMMAND_INIT(this->core->type.command_node.Start))) <
	    0)
		pw_log_debug("got error %d", res);

	return res;
}

static int suspend_node(struct pw_node *this)
{
	int res = SPA_RESULT_OK;
	struct pw_port *p;

	pw_log_debug("node %p: suspend node", this);

	spa_list_for_each(p, &this->input_ports, link) {
		if ((res =
		     spa_node_port_set_format(this->node, SPA_DIRECTION_INPUT, p->port_id, 0,
					      NULL)) < 0)
			pw_log_warn("error unset format input: %d", res);
		p->buffers = NULL;
		p->n_buffers = 0;
		if (p->allocated)
			pw_memblock_free(&p->buffer_mem);
		p->allocated = false;
		p->state = PW_PORT_STATE_CONFIGURE;
	}

	spa_list_for_each(p, &this->output_ports, link) {
		if ((res =
		     spa_node_port_set_format(this->node, SPA_DIRECTION_OUTPUT, p->port_id, 0,
					      NULL)) < 0)
			pw_log_warn("error unset format output: %d", res);
		p->buffers = NULL;
		p->n_buffers = 0;
		if (p->allocated)
			pw_memblock_free(&p->buffer_mem);
		p->allocated = false;
		p->state = PW_PORT_STATE_CONFIGURE;
	}
	return res;
}

static void send_clock_update(struct pw_node *this)
{
	int res;
	struct spa_command_node_clock_update cu =
	    SPA_COMMAND_NODE_CLOCK_UPDATE_INIT(this->core->type.command_node.ClockUpdate,
					       SPA_COMMAND_NODE_CLOCK_UPDATE_TIME |
					       SPA_COMMAND_NODE_CLOCK_UPDATE_SCALE |
					       SPA_COMMAND_NODE_CLOCK_UPDATE_STATE |
					       SPA_COMMAND_NODE_CLOCK_UPDATE_LATENCY,	/* change_mask */
					       1,	/* rate */
					       0,	/* ticks */
					       0,	/* monotonic_time */
					       0,	/* offset */
					       (1 << 16) | 1,	/* scale */
					       SPA_CLOCK_STATE_RUNNING,	/* state */
					       0,	/* flags */
					       0);	/* latency */

	if (this->clock && this->live) {
		cu.body.flags.value = SPA_COMMAND_NODE_CLOCK_UPDATE_FLAG_LIVE;
		res = spa_clock_get_time(this->clock,
					 &cu.body.rate.value,
					 &cu.body.ticks.value,
					 &cu.body.monotonic_time.value);
	}

	if ((res = spa_node_send_command(this->node, (struct spa_command *) &cu)) < 0)
		pw_log_debug("got error %d", res);
}

static int do_pull(struct pw_node *this)
{
	int res = SPA_RESULT_OK;
	struct pw_port *inport;
	bool have_output = false;

	spa_list_for_each(inport, &this->input_ports, link) {
		struct pw_link *link;
		struct pw_port *outport;
		struct spa_port_io *pi;
		struct spa_port_io *po;

		pi = &inport->io;
		pw_log_trace("node %p: need input port %d, %d %d", this,
			     inport->port_id, pi->buffer_id, pi->status);

		if (pi->status != SPA_RESULT_NEED_BUFFER)
			continue;

		spa_list_for_each(link, &inport->rt.links, rt.input_link) {
			if (link->rt.input == NULL || link->rt.output == NULL)
				continue;

			outport = link->rt.output;
			po = &outport->io;

			/* pull */
			*po = *pi;
			pi->buffer_id = SPA_ID_INVALID;

			pw_log_trace("node %p: process output %p %d", outport->node, po,
				     po->buffer_id);

			res = spa_node_process_output(outport->node->node);

			if (res == SPA_RESULT_NEED_BUFFER) {
				res = do_pull(outport->node);
				pw_log_trace("node %p: pull return %d", outport->node, res);
			} else if (res == SPA_RESULT_HAVE_BUFFER) {
				*pi = *po;
				pw_log_trace("node %p: have output %d %d", this, pi->status,
					     pi->buffer_id);
				have_output = true;
			} else if (res < 0) {
				pw_log_warn("node %p: got process output %d", outport->node, res);
			}

		}
	}
	if (have_output) {
		pw_log_trace("node %p: doing process input", this);
		res = spa_node_process_input(this->node);
	}
	return res;
}

static void on_node_event(struct spa_node *node, struct spa_event *event, void *user_data)
{
	struct pw_node *this = user_data;
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);

	if (SPA_EVENT_TYPE(event) == this->core->type.event_node.AsyncComplete) {
		struct spa_event_node_async_complete *ac =
		    (struct spa_event_node_async_complete *) event;

		pw_log_debug("node %p: async complete event %d %d", this, ac->body.seq.value,
			     ac->body.res.value);
		pw_work_queue_complete(impl->work, this, ac->body.seq.value, ac->body.res.value);
		pw_signal_emit(&this->async_complete, this, ac->body.seq.value, ac->body.res.value);
	} else if (SPA_EVENT_TYPE(event) == this->core->type.event_node.RequestClockUpdate) {
		send_clock_update(this);
	}
}

static void on_node_need_input(struct spa_node *node, void *user_data)
{
	struct pw_node *this = user_data;

	do_pull(this);
}

static void on_node_have_output(struct spa_node *node, void *user_data)
{
	struct pw_node *this = user_data;
	int res;
	struct pw_port *outport;

	spa_list_for_each(outport, &this->output_ports, link) {
		struct pw_link *link;
		struct spa_port_io *po;

		po = &outport->io;
		if (po->buffer_id == SPA_ID_INVALID)
			continue;

		pw_log_trace("node %p: have output %d", this, po->buffer_id);

		spa_list_for_each(link, &outport->rt.links, rt.output_link) {
			struct pw_port *inport;

			if (link->rt.input == NULL || link->rt.output == NULL)
				continue;

			inport = link->rt.input;
			inport->io = *po;

			pw_log_trace("node %p: do process input %d", this, po->buffer_id);

			if ((res = spa_node_process_input(inport->node->node)) < 0)
				pw_log_warn("node %p: got process input %d", inport->node, res);

		}
		po->status = SPA_RESULT_NEED_BUFFER;
	}
	res = spa_node_process_output(this->node);
}

static void
on_node_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id, void *user_data)
{
	struct pw_node *this = user_data;
	struct pw_port *inport;

	pw_log_trace("node %p: reuse buffer %u", this, buffer_id);

	spa_list_for_each(inport, &this->input_ports, link) {
		struct pw_link *link;
		struct pw_port *outport;

		spa_list_for_each(link, &inport->rt.links, rt.input_link) {
			if (link->rt.input == NULL || link->rt.output == NULL)
				continue;

			outport = link->rt.output;
			outport->io.buffer_id = buffer_id;
		}
	}
}

static void node_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	spa_list_remove(&resource->link);
}

static int
node_bind_func(struct pw_global *global, struct pw_client *client, uint32_t version, uint32_t id)
{
	struct pw_node *this = global->object;
	struct pw_resource *resource;
	struct pw_node_info info;
	int i;

	resource = pw_resource_new(client, id, global->type, global->object, node_unbind_func);
	if (resource == NULL)
		goto no_mem;

	pw_log_debug("node %p: bound to %d", this, resource->id);

	spa_list_insert(this->resource_list.prev, &resource->link);

	info.id = global->id;
	info.change_mask = ~0;
	info.name = this->name;
	info.max_inputs = this->max_input_ports;
	info.n_inputs = this->n_input_ports;
	info.input_formats = NULL;
	for (info.n_input_formats = 0;; info.n_input_formats++) {
		struct spa_format *fmt;

		if (spa_node_port_enum_formats(this->node,
					       SPA_DIRECTION_INPUT,
					       0, &fmt, NULL, info.n_input_formats) < 0)
			break;

		info.input_formats =
		    realloc(info.input_formats,
			    sizeof(struct spa_format *) * (info.n_input_formats + 1));
		info.input_formats[info.n_input_formats] = spa_format_copy(fmt);
	}
	info.max_outputs = this->max_output_ports;
	info.n_outputs = this->n_output_ports;
	info.output_formats = NULL;
	for (info.n_output_formats = 0;; info.n_output_formats++) {
		struct spa_format *fmt;

		if (spa_node_port_enum_formats(this->node,
					       SPA_DIRECTION_OUTPUT,
					       0, &fmt, NULL, info.n_output_formats) < 0)
			break;

		info.output_formats =
		    realloc(info.output_formats,
			    sizeof(struct spa_format *) * (info.n_output_formats + 1));
		info.output_formats[info.n_output_formats] = spa_format_copy(fmt);
	}
	info.state = this->state;
	info.error = this->error;
	info.props = this->properties ? &this->properties->dict : NULL;

	pw_node_notify_info(resource, &info);

	if (info.input_formats) {
		for (i = 0; i < info.n_input_formats; i++)
			free(info.input_formats[i]);
		free(info.input_formats);
	}

	if (info.output_formats) {
		for (i = 0; i < info.n_output_formats; i++)
			free(info.output_formats[i]);
		free(info.output_formats);
	}

	return SPA_RESULT_OK;

      no_mem:
	pw_log_error("can't create node resource");
	pw_core_notify_error(client->core_resource,
			     client->core_resource->id, SPA_RESULT_NO_MEMORY, "no memory");
	return SPA_RESULT_NO_MEMORY;
}

static void init_complete(struct pw_node *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);

	update_port_ids(this);
	pw_log_debug("node %p: init completed", this);
	impl->async_init = false;

	spa_list_insert(this->core->node_list.prev, &this->link);
	pw_core_add_global(this->core,
			   this->owner,
			   this->core->type.node, 0, this, node_bind_func, &this->global);

	pw_node_update_state(this, PW_NODE_STATE_SUSPENDED, NULL);
}

void pw_node_set_data_loop(struct pw_node *node, struct pw_data_loop *loop)
{
	node->data_loop = loop;
	pw_signal_emit(&node->loop_changed, node);
}

static const struct spa_node_callbacks node_callbacks = {
	&on_node_event,
	&on_node_need_input,
	&on_node_have_output,
	&on_node_reuse_buffer,
};

struct pw_node *pw_node_new(struct pw_core *core,
			    struct pw_client *owner,
			    const char *name,
			    bool async,
			    struct spa_node *node,
			    struct spa_clock *clock, struct pw_properties *properties)
{
	struct impl *impl;
	struct pw_node *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	this->core = core;
	this->owner = owner;
	pw_log_debug("node %p: new, owner %p", this, owner);

	impl->work = pw_work_queue_new(this->core->main_loop->loop);

	this->name = strdup(name);
	this->properties = properties;

	this->node = node;
	this->clock = clock;
	this->data_loop = core->data_loop;

	spa_list_init(&this->resource_list);

	if (spa_node_set_callbacks(this->node, &node_callbacks, sizeof(node_callbacks), this) < 0)
		pw_log_warn("node %p: error setting callback", this);

	pw_signal_init(&this->destroy_signal);
	pw_signal_init(&this->port_added);
	pw_signal_init(&this->port_removed);
	pw_signal_init(&this->state_request);
	pw_signal_init(&this->state_changed);
	pw_signal_init(&this->free_signal);
	pw_signal_init(&this->async_complete);
	pw_signal_init(&this->initialized);
	pw_signal_init(&this->loop_changed);

	this->state = PW_NODE_STATE_CREATING;

	spa_list_init(&this->input_ports);
	spa_list_init(&this->output_ports);

	if (this->node->info) {
		uint32_t i;

		if (this->properties == NULL)
			this->properties = pw_properties_new(NULL, NULL);

		if (this->properties)
			goto no_mem;

		for (i = 0; i < this->node->info->n_items; i++)
			pw_properties_set(this->properties,
					  this->node->info->items[i].key,
					  this->node->info->items[i].value);
	}

	impl->async_init = async;
	if (async) {
		pw_work_queue_add(impl->work,
				  this,
				  SPA_RESULT_RETURN_ASYNC(0), (pw_work_func_t) init_complete, NULL);
	} else {
		init_complete(this);
	}

	return this;

      no_mem:
	free(this->name);
	free(impl);
	return NULL;
}

static int
do_node_remove_done(struct spa_loop *loop,
		    bool async, uint32_t seq, size_t size, void *data, void *user_data)
{
	struct pw_node *this = user_data;
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct pw_port *port, *tmp;

	pw_log_debug("node %p: remove done, destroy ports", this);
	spa_list_for_each_safe(port, tmp, &this->input_ports, link)
		pw_port_destroy(port);

	spa_list_for_each_safe(port, tmp, &this->output_ports, link)
		pw_port_destroy(port);

	pw_log_debug("node %p: free", this);
	pw_signal_emit(&this->free_signal, this);

	pw_work_queue_destroy(impl->work);

	if (this->input_port_map)
		free(this->input_port_map);
	if (this->output_port_map)
		free(this->output_port_map);

	free(this->name);
	free(this->error);
	if (this->properties)
		pw_properties_free(this->properties);
	free(impl);

	return SPA_RESULT_OK;
}

static int
do_node_remove(struct spa_loop *loop,
	       bool async, uint32_t seq, size_t size, void *data, void *user_data)
{
	struct pw_node *this = user_data;
	struct pw_port *port, *tmp;
	int res;

	pause_node(this);

	spa_list_for_each_safe(port, tmp, &this->input_ports, link) {
		struct pw_link *link, *tlink;
		spa_list_for_each_safe(link, tlink, &port->rt.links, rt.input_link) {
			pw_port_pause_rt(link->rt.input);
			spa_list_remove(&link->rt.input_link);
			link->rt.input = NULL;
		}
	}
	spa_list_for_each_safe(port, tmp, &this->output_ports, link) {
		struct pw_link *link, *tlink;
		spa_list_for_each_safe(link, tlink, &port->rt.links, rt.output_link) {
			pw_port_pause_rt(link->rt.output);
			spa_list_remove(&link->rt.output_link);
			link->rt.output = NULL;
		}
	}

	res = pw_loop_invoke(this->core->main_loop->loop, do_node_remove_done, seq, 0, NULL, this);

	return res;
}

/** Destroy a node
 * \param node a node to destroy
 *
 * Remove \a node. This will stop the transfer on the node and
 * free the resources allocated by \a node.
 *
 * \memberof pw_node
 */
void pw_node_destroy(struct pw_node *node)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	struct pw_resource *resource, *tmp;

	pw_log_debug("node %p: destroy", impl);
	pw_signal_emit(&node->destroy_signal, node);

	if (!impl->async_init) {
		spa_list_remove(&node->link);
		pw_global_destroy(node->global);
	}

	spa_list_for_each_safe(resource, tmp, &node->resource_list, link)
		pw_resource_destroy(resource);

	pw_loop_invoke(node->data_loop->loop, do_node_remove, 1, 0, NULL, node);
}

/**
 * pw_node_get_free_port:
 * \param node a \ref pw_node
 * \param direction a \ref pw_direction
 * \return the new port or NULL on error
 *
 * Find a new unused port in \a node with \a direction
 *
 * \memberof pw_node
 */
struct pw_port *pw_node_get_free_port(struct pw_node *node, enum pw_direction direction)
{
	uint32_t *n_ports, max_ports;
	struct spa_list *ports;
	struct pw_port *port = NULL, *p, **portmap;
	int res;
	int i;

	if (direction == PW_DIRECTION_INPUT) {
		max_ports = node->max_input_ports;
		n_ports = &node->n_input_ports;
		ports = &node->input_ports;
		portmap = node->input_port_map;
	} else {
		max_ports = node->max_output_ports;
		n_ports = &node->n_output_ports;
		ports = &node->output_ports;
		portmap = node->output_port_map;
	}

	pw_log_debug("node %p: direction %d max %u, n %u", node, direction, max_ports, *n_ports);

	spa_list_for_each(p, ports, link) {
		if (spa_list_is_empty(&p->links)) {
			port = p;
			break;
		}
	}

	if (port == NULL) {
		/* no port, can we create one ? */
		if (*n_ports < max_ports) {
			for (i = 0; i < max_ports && port == NULL; i++) {
				if (portmap[i] == NULL) {
					pw_log_debug("node %p: creating port direction %d %u", node,
						     direction, i);
					port = portmap[i] = pw_port_new(node, direction, i);
					spa_list_insert(ports, &port->link);
					(*n_ports)++;
					if ((res = spa_node_add_port(node->node, direction, i)) < 0) {
						pw_log_error("node %p: could not add port %d", node,
							     i);
					} else {
						spa_node_port_set_io(node->node, direction, i,
								     &port->io);
					}
				}
			}
		} else {
			/* for output we can reuse an existing port */
			if (direction == PW_DIRECTION_OUTPUT && !spa_list_is_empty(ports)) {
				port = spa_list_first(ports, struct pw_port, link);
			}
		}
	}
	return port;

}

static void on_state_complete(struct pw_node *node, void *data, int res)
{
	enum pw_node_state state = SPA_PTR_TO_INT(data);
	char *error = NULL;

	pw_log_debug("node %p: state complete %d", node, res);
	if (SPA_RESULT_IS_ERROR(res)) {
		asprintf(&error, "error changing node state: %d", res);
		state = PW_NODE_STATE_ERROR;
	}
	pw_node_update_state(node, state, error);
}

static void node_activate(struct pw_node *this)
{
	struct pw_port *port;

	spa_list_for_each(port, &this->input_ports, link) {
		struct pw_link *link;
		spa_list_for_each(link, &port->links, input_link)
			pw_link_activate(link);
	}
	spa_list_for_each(port, &this->output_ports, link) {
		struct pw_link *link;
		spa_list_for_each(link, &port->links, output_link)
			pw_link_activate(link);
	}
}

/** Set th node state
 * \param node a \ref pw_node
 * \param state a \ref pw_node_state
 * \return 0 on success < 0 on error
 *
 * Set the state of \a node to \a state.
 *
 * \memberof pw_node
 */
int pw_node_set_state(struct pw_node *node, enum pw_node_state state)
{
	int res = SPA_RESULT_OK;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);

	pw_signal_emit(&node->state_request, node, state);

	pw_log_debug("node %p: set state %s", node, pw_node_state_as_string(state));

	switch (state) {
	case PW_NODE_STATE_CREATING:
		return SPA_RESULT_ERROR;

	case PW_NODE_STATE_SUSPENDED:
		res = suspend_node(node);
		break;

	case PW_NODE_STATE_IDLE:
		res = pause_node(node);
		break;

	case PW_NODE_STATE_RUNNING:
		node_activate(node);
		send_clock_update(node);
		res = start_node(node);
		break;

	case PW_NODE_STATE_ERROR:
		break;
	}
	if (SPA_RESULT_IS_ERROR(res))
		return res;

	pw_work_queue_add(impl->work,
			  node, res, (pw_work_func_t) on_state_complete, SPA_INT_TO_PTR(state));

	return res;
}

/** Update the node state
 * \param node a \ref pw_node
 * \param state a \ref pw_node_state
 * \param error error when \a state is \ref PW_NODE_STATE_ERROR
 *
 * Update the state of a node. This method is used from inside \a node
 * itself.
 *
 * \memberof pw_node
 */
void pw_node_update_state(struct pw_node *node, enum pw_node_state state, char *error)
{
	enum pw_node_state old;

	old = node->state;
	if (old != state) {
		struct pw_node_info info;
		struct pw_resource *resource;

		pw_log_debug("node %p: update state from %s -> %s", node,
			     pw_node_state_as_string(old), pw_node_state_as_string(state));

		if (node->error)
			free(node->error);
		node->error = error;
		node->state = state;

		pw_signal_emit(&node->state_changed, node, old, state);

		spa_zero(info);
		info.change_mask = 1 << 5;
		info.state = node->state;
		info.error = node->error;

		spa_list_for_each(resource, &node->resource_list, link) {
			/* global is only set when there are resources */
			info.id = node->global->id;
			pw_node_notify_info(resource, &info);
		}
	}
}
