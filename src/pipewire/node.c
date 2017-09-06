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
#include <stdio.h>
#include <errno.h>

#include <spa/clock.h>

#include "pipewire/pipewire.h"
#include "pipewire/interfaces.h"
#include "pipewire/private.h"

#include "pipewire/node.h"
#include "pipewire/data-loop.h"
#include "pipewire/main-loop.h"
#include "pipewire/work-queue.h"

/** \cond */
struct impl {
	struct pw_node this;

	struct pw_global *parent;

	struct pw_work_queue *work;

	bool registered;
};

struct resource_data {
	struct spa_hook resource_listener;
};

/** \endcond */

static int pause_node(struct pw_node *this)
{
	int res = SPA_RESULT_OK;

	if (this->info.state <= PW_NODE_STATE_IDLE)
		return SPA_RESULT_OK;

	pw_log_debug("node %p: pause node", this);
	res = spa_node_send_command(this->node,
				    &SPA_COMMAND_INIT(this->core->type.command_node.Pause));
	if (res < 0)
		pw_log_debug("node %p: send command error %d", this, res);

	return res;
}

static int start_node(struct pw_node *this)
{
	int res = SPA_RESULT_OK;

	pw_log_debug("node %p: start node", this);
	res = spa_node_send_command(this->node,
				    &SPA_COMMAND_INIT(this->core->type.command_node.Start));
	if (res < 0)
		pw_log_debug("node %p: send command error %d", this, res);

	return res;
}

static int suspend_node(struct pw_node *this)
{
	int res = SPA_RESULT_OK;
	struct pw_port *p;

	pw_log_debug("node %p: suspend node", this);

	spa_list_for_each(p, &this->input_ports, link) {
		if ((res = pw_port_set_format(p, 0, NULL)) < 0)
			pw_log_warn("error unset format input: %d", res);
		/* force CONFIGURE in case of async */
		p->state = PW_PORT_STATE_CONFIGURE;
	}

	spa_list_for_each(p, &this->output_ports, link) {
		if ((res = pw_port_set_format(p, 0, NULL)) < 0)
			pw_log_warn("error unset format output: %d", res);
		/* force CONFIGURE in case of async */
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
						SPA_COMMAND_NODE_CLOCK_UPDATE_LATENCY,   /* change_mask */
						1,       /* rate */
						0,       /* ticks */
						0,       /* monotonic_time */
						0,       /* offset */
						(1 << 16) | 1,   /* scale */
						SPA_CLOCK_STATE_RUNNING, /* state */
						0,       /* flags */
						0);      /* latency */

	if (this->clock && this->live) {
		cu.body.flags.value = SPA_COMMAND_NODE_CLOCK_UPDATE_FLAG_LIVE;
		res = spa_clock_get_time(this->clock,
					 &cu.body.rate.value,
					 &cu.body.ticks.value,
					 &cu.body.monotonic_time.value);
	}
	res = spa_node_send_command(this->node, (struct spa_command *) &cu);
	if (res < 0)
		pw_log_debug("node %p: send clock update error %d", this, res);
}

static void node_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	spa_list_remove(&resource->link);
}

static void
update_info(struct pw_node *this)
{
	this->info.input_formats = NULL;
	if (!spa_list_is_empty(&this->input_ports)) {
		struct pw_port *port = spa_list_first(&this->input_ports, struct pw_port, link);

		for (this->info.n_input_formats = 0;; this->info.n_input_formats++) {
			struct spa_format *fmt;

			if (spa_node_port_enum_formats(port->node->node, port->direction, port->port_id,
						       &fmt, NULL, this->info.n_input_formats) < 0)
				break;

			this->info.input_formats =
			    realloc(this->info.input_formats,
				    sizeof(struct spa_format *) * (this->info.n_input_formats + 1));
			this->info.input_formats[this->info.n_input_formats] = spa_format_copy(fmt);
		}
	}

	this->info.output_formats = NULL;
	if (!spa_list_is_empty(&this->output_ports)) {
		struct pw_port *port = spa_list_first(&this->output_ports, struct pw_port, link);

		for (this->info.n_output_formats = 0;; this->info.n_output_formats++) {
			struct spa_format *fmt;

			if (spa_node_port_enum_formats(port->node->node, port->direction, port->port_id,
						       &fmt, NULL, this->info.n_output_formats) < 0)
				break;

			this->info.output_formats =
			    realloc(this->info.output_formats,
				    sizeof(struct spa_format *) * (this->info.n_output_formats + 1));
			this->info.output_formats[this->info.n_output_formats] = spa_format_copy(fmt);
		}
	}
	this->info.props = this->properties ? &this->properties->dict : NULL;
}

static void
clear_info(struct pw_node *this)
{
	int i;

	free((char*)this->info.name);
	if (this->info.input_formats) {
		for (i = 0; i < this->info.n_input_formats; i++)
			free(this->info.input_formats[i]);
		free(this->info.input_formats);
	}

	if (this->info.output_formats) {
		for (i = 0; i < this->info.n_output_formats; i++)
			free(this->info.output_formats[i]);
		free(this->info.output_formats);
	}
	free((char*)this->info.error);

}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = node_unbind_func,
};

static int
node_bind_func(struct pw_global *global,
	       struct pw_client *client, uint32_t permissions,
	       uint32_t version, uint32_t id)
{
	struct pw_node *this = global->object;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	pw_resource_add_listener(resource, &data->resource_listener, &resource_events, resource);

	pw_log_debug("node %p: bound to %d", this, resource->id);

	spa_list_insert(this->resource_list.prev, &resource->link);

	this->info.change_mask = ~0;
	pw_node_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return SPA_RESULT_OK;

      no_mem:
	pw_log_error("can't create node resource");
	pw_core_resource_error(client->core_resource,
			       client->core_resource->id, SPA_RESULT_NO_MEMORY, "no memory");
	return SPA_RESULT_NO_MEMORY;
}

static int
do_node_add(struct spa_loop *loop,
	    bool async, uint32_t seq, size_t size, const void *data, void *user_data)
{
	struct pw_node *this = user_data;

	spa_graph_node_add(this->rt.graph, &this->rt.node);

	return SPA_RESULT_OK;
}


void pw_node_register(struct pw_node *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct pw_core *core = this->core;

	pw_log_debug("node %p: register", this);

	update_info(this);

	pw_loop_invoke(this->data_loop, do_node_add, 1, 0, NULL, false, this);

	spa_list_insert(core->node_list.prev, &this->link);
	this->global = pw_core_add_global(core, this->owner ? this->owner->client : NULL,
					  impl->parent,
					  core->type.node, PW_VERSION_NODE,
					  node_bind_func, this);

	impl->registered = true;
	spa_hook_list_call(&this->listener_list, struct pw_node_events, initialized);

	pw_node_update_state(this, PW_NODE_STATE_SUSPENDED, NULL);
}

struct pw_node *pw_node_new(struct pw_core *core,
			    struct pw_resource *owner,
			    struct pw_global *parent,
			    const char *name,
			    struct pw_properties *properties,
			    size_t user_data_size)
{
	struct impl *impl;
	struct pw_node *this;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	this->core = core;
	this->owner = owner;
	impl->parent = parent;
	pw_log_debug("node %p: new, owner %p", this, owner);

	if (user_data_size > 0)
                this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto no_mem;

	this->properties = properties;

	impl->work = pw_work_queue_new(this->core->main_loop);
	this->info.name = strdup(name);

	this->data_loop = core->data_loop;

	this->rt.graph = &core->rt.graph;

	spa_list_init(&this->resource_list);

	spa_hook_list_init(&this->listener_list);

	this->info.state = PW_NODE_STATE_CREATING;
	this->info.props = &this->properties->dict;

	spa_list_init(&this->input_ports);
	pw_map_init(&this->input_port_map, 64, 64);
	spa_list_init(&this->output_ports);
	pw_map_init(&this->output_port_map, 64, 64);

	spa_graph_node_init(&this->rt.node);

	return this;

      no_mem:
	free(impl);
	return NULL;
}

const struct pw_node_info *pw_node_get_info(struct pw_node *node)
{
	return &node->info;
}

void * pw_node_get_user_data(struct pw_node *node)
{
	return node->user_data;
}

struct pw_core * pw_node_get_core(struct pw_node *node)
{
	return node->core;
}

struct pw_resource *pw_node_get_owner(struct pw_node *node)
{
	return node->owner;
}

struct pw_global *pw_node_get_global(struct pw_node *node)
{
	return node->global;
}

const struct pw_properties *pw_node_get_properties(struct pw_node *node)
{
	return node->properties;
}

void pw_node_update_properties(struct pw_node *node, const struct spa_dict *dict)
{
	struct pw_resource *resource;
	uint32_t i;

	for (i = 0; i < dict->n_items; i++)
		pw_properties_set(node->properties, dict->items[i].key, dict->items[i].value);

	node->info.props = &node->properties->dict;

	node->info.change_mask = PW_NODE_CHANGE_MASK_PROPS;
	spa_hook_list_call(&node->listener_list, struct pw_node_events, info_changed, &node->info);

	spa_list_for_each(resource, &node->resource_list, link)
		pw_node_resource_info(resource, &node->info);

	node->info.change_mask = 0;
}

static void node_done(void *data, int seq, int res)
{
	struct pw_node *node = data;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);

	pw_log_debug("node %p: async complete event %d %d", node, seq, res);
	pw_work_queue_complete(impl->work, node, seq, res);
	spa_hook_list_call(&node->listener_list, struct pw_node_events, async_complete, seq, res);
}

static void node_event(void *data, struct spa_event *event)
{
	struct pw_node *node = data;

	pw_log_trace("node %p: event %d", node, SPA_EVENT_TYPE(event));
        if (SPA_EVENT_TYPE(event) == node->core->type.event_node.RequestClockUpdate) {
                send_clock_update(node);
        }
	spa_hook_list_call(&node->listener_list, struct pw_node_events, event, event);
}

static void node_need_input(void *data)
{
	struct pw_node *node = data;
	spa_hook_list_call(&node->listener_list, struct pw_node_events, need_input);
	spa_graph_need_input(node->rt.graph, &node->rt.node);
}

static void node_have_output(void *data)
{
	struct pw_node *node = data;
	spa_hook_list_call(&node->listener_list, struct pw_node_events, have_output);
	spa_graph_have_output(node->rt.graph, &node->rt.node);
}

static void node_reuse_buffer(void *data, uint32_t port_id, uint32_t buffer_id)
{
	struct pw_node *node = data;
        struct spa_graph_port *p, *pp;

	spa_list_for_each(p, &node->rt.node.ports[SPA_DIRECTION_INPUT], link) {
		if (p->port_id != port_id)
			continue;

		if ((pp = p->peer) != NULL)
			spa_node_port_reuse_buffer(pp->node->implementation, pp->port_id, buffer_id);
		break;
	}
}


static const struct spa_node_callbacks node_callbacks = {
	SPA_VERSION_NODE_CALLBACKS,
	.done = node_done,
	.event = node_event,
	.need_input = node_need_input,
	.have_output = node_have_output,
	.reuse_buffer = node_reuse_buffer,
};

void pw_node_set_implementation(struct pw_node *node,
				struct spa_node *spa_node)
{
	node->node = spa_node;
	spa_node_set_callbacks(node->node, &node_callbacks, node);
	spa_graph_node_set_implementation(&node->rt.node, spa_node);
}

struct spa_node *pw_node_get_implementation(struct pw_node *node)
{
	return node->node;
}

void pw_node_add_listener(struct pw_node *node,
			   struct spa_hook *listener,
			   const struct pw_node_events *events,
			   void *data)
{
	spa_hook_list_append(&node->listener_list, listener, events, data);
}

static int
do_node_remove(struct spa_loop *loop,
	       bool async, uint32_t seq, size_t size, const void *data, void *user_data)
{
	struct pw_node *this = user_data;

	pause_node(this);

	spa_graph_node_remove(&this->rt.node);

	return SPA_RESULT_OK;
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
	struct pw_port *port, *tmpp;

	pw_log_debug("node %p: destroy", impl);
	spa_hook_list_call(&node->listener_list, struct pw_node_events, destroy);

	pw_loop_invoke(node->data_loop, do_node_remove, 1, 0, NULL, true, node);

	if (impl->registered) {
		spa_list_remove(&node->link);
		pw_global_destroy(node->global);
		node->global = NULL;
	}

	spa_list_for_each_safe(resource, tmp, &node->resource_list, link)
		pw_resource_destroy(resource);

	pw_log_debug("node %p: destroy ports", node);
	spa_list_for_each_safe(port, tmpp, &node->input_ports, link) {
		spa_hook_list_call(&node->listener_list, struct pw_node_events, port_removed, port);
		pw_port_destroy(port);
	}

	spa_list_for_each_safe(port, tmpp, &node->output_ports, link) {
		spa_hook_list_call(&node->listener_list, struct pw_node_events, port_removed, port);
		pw_port_destroy(port);
	}

	pw_log_debug("node %p: free", node);
	spa_hook_list_call(&node->listener_list, struct pw_node_events, free);

	pw_work_queue_destroy(impl->work);

	pw_map_clear(&node->input_port_map);
	pw_map_clear(&node->output_port_map);

	if (node->properties)
		pw_properties_free(node->properties);

	clear_info(node);

	free(impl);
}

void pw_node_set_max_ports(struct pw_node *node,
			   uint32_t max_input_ports,
			   uint32_t max_output_ports)
{
	if (node->info.max_input_ports != max_input_ports) {
		node->info.max_input_ports = max_input_ports;
		node->info.change_mask |= PW_NODE_CHANGE_MASK_INPUT_PORTS;
	}
	if (node->info.max_output_ports != max_output_ports) {
		node->info.max_output_ports = max_output_ports;
		node->info.change_mask |= PW_NODE_CHANGE_MASK_OUTPUT_PORTS;
	}
}

bool pw_node_for_each_port(struct pw_node *node,
			   enum pw_direction direction,
			   bool (*callback) (void *data, struct pw_port *port),
			   void *data)
{
	struct spa_list *ports;
	struct pw_port *p, *t;

	if (direction == PW_DIRECTION_INPUT)
		ports = &node->input_ports;
	else
		ports = &node->output_ports;

	spa_list_for_each_safe(p, t, ports, link)
		if (!callback(data, p))
			return false;
	return true;
}

struct pw_port *
pw_node_find_port(struct pw_node *node, enum pw_direction direction, uint32_t port_id)
{
	struct pw_map *portmap;

	if (direction == PW_DIRECTION_INPUT)
		portmap = &node->input_port_map;
	else
		portmap = &node->output_port_map;

	return pw_map_lookup(portmap, port_id);
}

uint32_t pw_node_get_free_port_id(struct pw_node *node, enum pw_direction direction)
{
	if (direction == PW_DIRECTION_INPUT)
		return pw_map_insert_new(&node->input_port_map, NULL);
	else
		return pw_map_insert_new(&node->output_port_map, NULL);
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
	uint32_t n_ports, max_ports;
	struct spa_list *ports;
	struct pw_port *port = NULL, *p, *mixport = NULL;
	struct pw_map *portmap;

	if (direction == PW_DIRECTION_INPUT) {
		max_ports = node->info.max_input_ports;
		n_ports = node->info.n_input_ports;
		ports = &node->input_ports;
		portmap = &node->input_port_map;
	} else {
		max_ports = node->info.max_output_ports;
		n_ports = node->info.n_output_ports;
		ports = &node->output_ports;
		portmap = &node->output_port_map;
	}

	pw_log_debug("node %p: direction %d max %u, n %u", node, direction, max_ports, n_ports);

	/* first try to find an unlinked port */
	spa_list_for_each(p, ports, link) {
		if (spa_list_is_empty(&p->links))
			return p;
		/* for output we can reuse an existing port, for input only
		 * when there is a multiplex */
		if (direction == PW_DIRECTION_OUTPUT || p->mix != NULL)
			mixport = p;
	}

	/* no port, can we create one ? */
	if (n_ports < max_ports) {
		uint32_t port_id = pw_map_insert_new(portmap, NULL);
		int res;

		pw_log_debug("node %p: creating port direction %d %u", node, direction, port_id);

		if ((res = spa_node_add_port(node->node, direction, port_id)) < 0) {
			pw_log_error("node %p: could not add port %d %d", node, port_id, res);
			goto no_mem;
		}
		port = pw_port_new(direction, port_id, NULL, 0);
		if (port == NULL)
			goto no_mem;

		pw_port_add(port, node);
	} else {
		port = mixport;
	}
	return port;

      no_mem:
	pw_log_error("node %p: can't create new port", node);
	return NULL;
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

static void node_deactivate(struct pw_node *this)
{
	struct pw_port *port;

	pw_log_debug("node %p: deactivate", this);
	spa_list_for_each(port, &this->input_ports, link) {
		struct pw_link *link;
		spa_list_for_each(link, &port->links, input_link)
			pw_link_deactivate(link);
	}
	spa_list_for_each(port, &this->output_ports, link) {
		struct pw_link *link;
		spa_list_for_each(link, &port->links, output_link)
			pw_link_deactivate(link);
	}
}

static void node_activate(struct pw_node *this)
{
	struct pw_port *port;

	pw_log_debug("node %p: activate", this);
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

	spa_hook_list_call(&node->listener_list, struct pw_node_events, state_request, state);

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

	old = node->info.state;
	if (old != state) {
		struct pw_resource *resource;

		pw_log_debug("node %p: update state from %s -> %s", node,
			     pw_node_state_as_string(old), pw_node_state_as_string(state));

		if (node->info.error)
			free((char*)node->info.error);
		node->info.error = error;
		node->info.state = state;

		if (state == PW_NODE_STATE_IDLE)
			node_deactivate(node);

		spa_hook_list_call(&node->listener_list, struct pw_node_events, state_changed,
				 old, state, error);

		node->info.change_mask |= PW_NODE_CHANGE_MASK_STATE;
		spa_hook_list_call(&node->listener_list, struct pw_node_events, info_changed, &node->info);

		spa_list_for_each(resource, &node->resource_list, link)
			pw_node_resource_info(resource, &node->info);

		node->info.change_mask = 0;
	}
}
