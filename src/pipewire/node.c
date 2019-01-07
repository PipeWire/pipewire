/* PipeWire
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
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#include <spa/pod/parser.h>

#include "pipewire/pipewire.h"
#include "pipewire/interfaces.h"
#include "pipewire/private.h"

#include "pipewire/node.h"
#include "pipewire/data-loop.h"
#include "pipewire/main-loop.h"
#include "pipewire/work-queue.h"

#ifndef spa_debug
#define spa_debug pw_log_trace
#endif

#include <spa/graph/graph-scheduler2.h>

#define DEFAULT_QUANTUM		1024u
#define MIN_QUANTUM		64u

/** \cond */
struct impl {
	struct pw_node this;

	struct pw_work_queue *work;
	bool pause_on_idle;

	struct spa_graph driver_graph;
	struct spa_graph_state driver_state;
	struct spa_graph_data driver_data;

	struct spa_graph graph;
	struct spa_graph_state graph_state;
	struct spa_graph_data graph_data;

	struct pw_node_activation root_activation;
	struct pw_node_activation node_activation;

	struct spa_io_position position;
	uint32_t next_position;
};

struct resource_data {
	struct spa_hook resource_listener;
	struct pw_node *node;
};

/** \endcond */

static void node_deactivate(struct pw_node *this)
{
	struct pw_port *port;
	struct pw_link *link;

	pw_log_debug("node %p: deactivate", this);
	spa_list_for_each(port, &this->input_ports, link) {
		spa_list_for_each(link, &port->links, input_link)
			pw_link_deactivate(link);
	}
	spa_list_for_each(port, &this->output_ports, link) {
		spa_list_for_each(link, &port->links, output_link)
			pw_link_deactivate(link);
	}
}

static int
do_node_remove(struct spa_loop *loop,
	       bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct pw_node *this = user_data;
	if (this->rt.root.graph != NULL) {
		spa_graph_node_remove(&this->rt.root);
		this->rt.root.graph = NULL;
	}
	return 0;
}

static int pause_node(struct pw_node *this)
{
	int res = 0;

	if (this->info.state <= PW_NODE_STATE_IDLE)
		return 0;

	pw_log_debug("node %p: pause node", this);
	node_deactivate(this);

	pw_loop_invoke(this->data_loop, do_node_remove, 1, NULL, 0, true, this);

	res = spa_node_send_command(this->node,
				    &SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause));
	if (res < 0)
		pw_log_debug("node %p: pause node error %s", this, spa_strerror(res));

	return res;
}

static int
do_node_add(struct spa_loop *loop,
	    bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct pw_node *this = user_data;
	if (this->rt.root.graph == NULL)
		spa_graph_node_add(this->driver_node->rt.driver, &this->rt.root);
	return 0;
}

static int start_node(struct pw_node *this)
{
	int res = 0;

	if (this->info.state >= PW_NODE_STATE_RUNNING)
		return 0;

	pw_log_debug("node %p: start node %d %d %d %d", this, this->n_ready_output_links,
			this->n_used_output_links, this->n_ready_input_links,
			this->n_used_input_links);

	if (this->n_ready_output_links != this->n_used_output_links ||
	    this->n_ready_input_links != this->n_used_input_links)
		return 0;

	res = spa_node_send_command(this->node,
				    &SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start));
	if (res < 0)
		pw_log_debug("node %p: start node error %s", this, spa_strerror(res));

	return res;
}

static void node_update_state(struct pw_node *node, enum pw_node_state state, char *error)
{
	enum pw_node_state old;
	struct pw_resource *resource;

	old = node->info.state;
	if (old == state)
		return;

	if (state == PW_NODE_STATE_ERROR) {
		pw_log_error("node %p: update state from %s -> error (%s)", node,
		     pw_node_state_as_string(old), error);
	} else {
		pw_log_debug("node %p: update state from %s -> %s", node,
		     pw_node_state_as_string(old), pw_node_state_as_string(state));
	}

	free((char*)node->info.error);
	node->info.error = error;
	node->info.state = state;

	switch (state) {
	case PW_NODE_STATE_RUNNING:
		pw_loop_invoke(node->data_loop, do_node_add, 1, NULL, 0, true, node);
		break;
	default:
		break;
	}

	pw_node_events_state_changed(node, old, state, error);

	node->info.change_mask |= PW_NODE_CHANGE_MASK_STATE;
	pw_node_events_info_changed(node, &node->info);

	if (node->global)
		spa_list_for_each(resource, &node->global->resource_list, link)
			pw_node_resource_info(resource, &node->info);

	node->info.change_mask = 0;
}


static int suspend_node(struct pw_node *this)
{
	int res = 0;
	struct pw_port *p;

	pw_log_debug("node %p: suspend node", this);

	spa_list_for_each(p, &this->input_ports, link) {
		if ((res = pw_port_set_param(p, SPA_ID_INVALID, SPA_PARAM_Format, 0, NULL)) < 0)
			pw_log_warn("error unset format input: %s", spa_strerror(res));
		/* force CONFIGURE in case of async */
		p->state = PW_PORT_STATE_CONFIGURE;
	}

	spa_list_for_each(p, &this->output_ports, link) {
		if ((res = pw_port_set_param(p, SPA_ID_INVALID, SPA_PARAM_Format, 0, NULL)) < 0)
			pw_log_warn("error unset format output: %s", spa_strerror(res));
		/* force CONFIGURE in case of async */
		p->state = PW_PORT_STATE_CONFIGURE;
	}
	node_update_state(this, PW_NODE_STATE_SUSPENDED, NULL);
	return res;
}
static void node_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	spa_list_remove(&resource->link);
}

static void update_port_map(struct pw_node *node, enum pw_direction direction,
			    struct pw_map *portmap, uint32_t *ids, uint32_t n_ids)
{
	uint32_t o, n;
	size_t os, ns;
	struct pw_port *port;
	int res;

	o = n = 0;
	os = pw_map_get_size(portmap);
	ns = n_ids;

	while (o < os || n < ns) {
		port = pw_map_lookup(portmap, o);

		if (n >= ns || o < ids[n]) {
			pw_log_debug("node %p: %s port %d removed", node,
					pw_direction_as_string(direction), o);

			if (port != NULL)
				pw_port_destroy(port);

			o++;
		}
		else if (o >= os || o > ids[n]) {
			pw_log_debug("node %p: %s port %d added", node,
					pw_direction_as_string(direction), ids[n]);

			if (port == NULL) {
				if ((port = pw_port_new(direction, ids[n], NULL, node->port_user_data_size))) {
					if ((res = pw_port_add(port, node)) < 0) {
						pw_log_error("node %p: can't add port %p: %d, %s",
								node, port, res, spa_strerror(res));
						pw_port_destroy(port);
					}
				}
				o = ids[n] + 1;
				os++;
			}

			n++;
		}
		else {
			pw_log_debug("node %p: %s port %d unchanged", node,
					pw_direction_as_string(direction), o);
			n++;
			o++;
		}
	}
}

int pw_node_update_ports(struct pw_node *node)
{
	uint32_t *input_port_ids, *output_port_ids;
	uint32_t n_input_ports, n_output_ports, max_input_ports, max_output_ports;
	int res;

	res = spa_node_get_n_ports(node->node,
				   &n_input_ports,
				   &max_input_ports,
				   &n_output_ports,
				   &max_output_ports);
	if (res < 0)
		return res;

	if (node->info.max_input_ports != max_input_ports) {
		node->info.max_input_ports = max_input_ports;
		node->info.change_mask |= PW_NODE_CHANGE_MASK_INPUT_PORTS;
	}
	if (node->info.max_output_ports != max_output_ports) {
		node->info.max_output_ports = max_output_ports;
		node->info.change_mask |= PW_NODE_CHANGE_MASK_OUTPUT_PORTS;
	}

	input_port_ids = alloca(sizeof(uint32_t) * n_input_ports);
	output_port_ids = alloca(sizeof(uint32_t) * n_output_ports);

	res = spa_node_get_port_ids(node->node,
				    input_port_ids,
				    max_input_ports,
				    output_port_ids,
				    max_output_ports);
	if (res < 0)
		return res;

	pw_log_debug("node %p: update_port ids input %u/%u, outputs %u/%u", node,
		     n_input_ports, max_input_ports, n_output_ports, max_output_ports);

	update_port_map(node, PW_DIRECTION_INPUT, &node->input_port_map, input_port_ids, n_input_ports);
	update_port_map(node, PW_DIRECTION_OUTPUT, &node->output_port_map, output_port_ids, n_output_ports);

	return 0;
}

static void
clear_info(struct pw_node *this)
{
	free((char*)this->info.name);
	free((char*)this->info.error);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = node_unbind_func,
};

static int reply_param(void *data, uint32_t id, uint32_t index, uint32_t next, struct spa_pod *param)
{
	struct pw_resource *resource = data;
	pw_node_resource_param(resource, id, index, next, param);
	return 0;
}

static void node_enum_params(void *object, uint32_t id, uint32_t index, uint32_t num,
		const struct spa_pod *filter)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct pw_node *node = data->node;
	int res;

	if ((res = pw_node_for_each_param(node, id, index, num,
				filter, reply_param, resource)) < 0) {
		pw_log_error("resource %p: %d error %d (%s)", resource,
				resource->id, res, spa_strerror(res));
		pw_core_resource_error(resource->client->core_resource,
				resource->id, res, spa_strerror(res));
	}
}

static void node_set_param(void *object, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct pw_node *node = data->node;
	int res;

	if ((res = spa_node_set_param(node->node, id, flags, param)) < 0) {
		pw_log_error("resource %p: %d error %d (%s)", resource,
				resource->id, res, spa_strerror(res));
		pw_core_resource_error(resource->client->core_resource,
				resource->id, res, spa_strerror(res));
	}
}

static void node_send_command(void *object, const struct spa_command *command)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct pw_node *node = data->node;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Suspend:
		suspend_node(node);
		break;
	default:
		spa_node_send_command(node->node, command);
		break;
	}
}

static const struct pw_node_proxy_methods node_methods = {
	PW_VERSION_NODE_PROXY_METHODS,
	.enum_params = node_enum_params,
	.set_param = node_set_param,
	.send_command = node_send_command
};

static void
global_bind(void *_data, struct pw_client *client, uint32_t permissions,
	    uint32_t version, uint32_t id)
{
	struct pw_node *this = _data;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	data->node = this;
	pw_resource_add_listener(resource, &data->resource_listener, &resource_events, resource);

	pw_resource_set_implementation(resource, &node_methods, resource);

	pw_log_debug("node %p: bound to %d", this, resource->id);

	spa_list_append(&global->resource_list, &resource->link);

	this->info.change_mask = ~0;
	pw_node_resource_info(resource, &this->info);
	this->info.change_mask = 0;
	return;

      no_mem:
	pw_log_error("can't create node resource");
	pw_core_resource_error(client->core_resource, id, -ENOMEM, "no memory");
	return;
}

static void global_destroy(void *data)
{
	struct pw_node *this = data;
	spa_hook_remove(&this->global_listener);
	this->global = NULL;
	pw_node_destroy(this);
}

static void global_registering(void *data)
{
	struct pw_node *this = data;
	struct pw_port *port;

	spa_list_for_each(port, &this->input_ports, link)
		pw_port_register(port, this->global->owner, this->global,
				 pw_properties_copy(port->properties));
	spa_list_for_each(port, &this->output_ports, link)
		pw_port_register(port, this->global->owner, this->global,
				 pw_properties_copy(port->properties));
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.registering = global_registering,
	.destroy = global_destroy,
	.bind = global_bind,
};

int pw_node_register(struct pw_node *this,
		     struct pw_client *owner,
		     struct pw_global *parent,
		     struct pw_properties *properties)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct pw_core *core = this->core;
	const char *str;

	pw_log_debug("node %p: register", this);

	if (this->registered)
		return -EEXIST;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		return -ENOMEM;

	pw_node_update_ports(this);

	if ((str = pw_properties_get(this->properties, "media.class")) != NULL)
		pw_properties_set(properties, "media.class", str);
	pw_properties_set(properties, "node.name", this->info.name);
	if ((str = pw_properties_get(this->properties, "node.session")) != NULL)
		pw_properties_set(properties, "node.session", str);

	spa_list_append(&core->node_list, &this->link);
	this->registered = true;

	this->global = pw_global_new(core,
				     PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
				     properties,
				     this);
	if (this->global == NULL)
		return -ENOMEM;

	this->info.id = this->global->id;
	impl->position.clock.id = this->info.id;
	pw_properties_setf(this->properties, "node.id", "%d", this->info.id);

	pw_node_initialized(this);

	pw_global_add_listener(this->global, &this->global_listener, &global_events, this);
	pw_global_register(this->global, owner, parent);

	return 0;
}

int pw_node_initialized(struct pw_node *this)
{
	pw_log_debug("node %p initialized", this);
	pw_node_events_initialized(this);
	node_update_state(this, PW_NODE_STATE_SUSPENDED, NULL);
	return 0;
}

static int
do_move_nodes(struct spa_loop *loop,
		bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *src = user_data;
	struct pw_node *this = &src->this;
	struct impl *dst = *(struct impl **)data;
	struct spa_graph_node *n, *t;

	pw_log_trace("node %p: root %p driver:%p->%p", this,
			&this->rt.root, &src->driver_graph, &dst->driver_graph);

	if (this->rt.root.graph != NULL) {
		spa_graph_node_remove(&this->rt.root);
		spa_graph_node_add(&dst->driver_graph, &this->rt.root);
	}

	if (this->node && spa_node_set_io(this->node,
			    SPA_IO_Position,
			    &dst->position, sizeof(struct spa_io_position)) >= 0) {
		pw_log_debug("node %p: set position %p", this, &dst->position);
		this->rt.position = &dst->position;
	}

	if (&src->driver_graph == &dst->driver_graph)
		return 0;

	spa_list_for_each_safe(n, t, &src->driver_graph.nodes, link) {
		spa_graph_node_remove(n);
		spa_graph_node_add(&dst->driver_graph, n);
	}
	return 0;
}

static int recalc_quantum(struct pw_node *driver)
{
	uint32_t quantum = DEFAULT_QUANTUM;
	struct pw_node *n;

	spa_list_for_each(n, &driver->driver_list, driver_link) {
		if (n->quantum_size > 0 && n->quantum_size < quantum)
			quantum = n->quantum_size;
	}
	if (driver->rt.position) {
		driver->rt.position->size = SPA_MAX(quantum, MIN_QUANTUM);
		pw_log_info("node %p: driver quantum %d", driver, driver->rt.position->size);
	}
	return 0;
}

int pw_node_set_driver(struct pw_node *node, struct pw_node *driver)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	struct pw_node *n, *t, *old;

	old = node->driver_node;

	pw_log_debug("node %p: driver:%p current:%p", node, driver, old);

	if (driver == NULL)
		driver = node;

	spa_list_remove(&node->driver_link);
	spa_list_append(&driver->driver_list, &node->driver_link);

	spa_list_for_each_safe(n, t, &node->driver_list, driver_link) {
		pw_log_debug("driver %p: add %p old %p", driver, n, n->driver_node);

		if (n->driver_node == driver)
			continue;

		spa_list_remove(&n->driver_link);
		spa_list_append(&driver->driver_list, &n->driver_link);
		n->driver_node = driver;
		pw_node_events_driver_changed(n, driver);
	}

	recalc_quantum(driver);

	pw_loop_invoke(node->data_loop,
		       do_move_nodes, SPA_ID_INVALID, &driver, sizeof(struct pw_node *),
		       true, impl);

	if (old != driver) {
		node->driver_node = driver;
		pw_node_events_driver_changed(node, driver);
	}

	return 0;
}

static uint32_t flp2(uint32_t x)
{
	x = x | (x >> 1);
	x = x | (x >> 2);
	x = x | (x >> 4);
	x = x | (x >> 8);
	x = x | (x >> 16);
	return x - (x >> 1);
}

static void check_properties(struct pw_node *node)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	const char *str;

	if ((str = pw_properties_get(node->properties, "node.pause-on-idle")))
		impl->pause_on_idle = pw_properties_parse_bool(str);
	else
		impl->pause_on_idle = true;

	if ((str = pw_properties_get(node->properties, "node.driver")))
		node->driver = pw_properties_parse_bool(str);
	else
		node->driver = false;

	if ((str = pw_properties_get(node->properties, "node.latency"))) {
		uint32_t num, denom;
		pw_log_info("node %p: latency '%s'", node, str);
                if (sscanf(str, "%u/%u", &num, &denom) == 2 && denom != 0) {
			node->quantum_size = flp2((num * 48000 / denom));
			pw_log_info("node %p: quantum %d", node, node->quantum_size);
		}
	} else
		node->quantum_size = DEFAULT_QUANTUM;

	pw_log_debug("node %p: graph %p driver:%d", node, &impl->driver_graph, node->driver);

}

struct pw_node *pw_node_new(struct pw_core *core,
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
	pw_log_debug("node %p: new \"%s\"", this, name);

	if (user_data_size > 0)
                this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto no_mem;

	this->enabled = true;
	this->properties = properties;

	impl->work = pw_work_queue_new(this->core->main_loop);
	this->info.name = strdup(name);

	this->data_loop = core->data_loop;

	spa_list_init(&this->driver_list);

	spa_hook_list_init(&this->listener_list);

	this->info.state = PW_NODE_STATE_CREATING;
	this->info.props = &this->properties->dict;

	spa_list_init(&this->input_ports);
	pw_map_init(&this->input_port_map, 64, 64);
	spa_list_init(&this->output_ports);
	pw_map_init(&this->output_port_map, 64, 64);


	spa_graph_init(&impl->driver_graph, &impl->driver_state);
	spa_graph_data_init(&impl->driver_data, &impl->driver_graph);
	spa_graph_set_callbacks(&impl->driver_graph,
			&spa_graph_impl_default, &impl->driver_data);

	this->rt.driver = &impl->driver_graph;
	this->rt.activation = &impl->root_activation;
	spa_graph_node_init(&this->rt.root, &this->rt.activation->state);

	spa_graph_init(&impl->graph, &impl->graph_state);
	spa_graph_data_init(&impl->graph_data, &impl->graph);
	spa_graph_set_callbacks(&impl->graph,
			&spa_graph_impl_default, &impl->graph_data);

	spa_graph_node_set_subgraph(&this->rt.root, &impl->graph);
	spa_graph_node_set_callbacks(&this->rt.root,
			&spa_graph_node_sub_impl_default, this);

	impl->node_activation.state.status = SPA_STATUS_NEED_BUFFER;
	spa_graph_node_init(&this->rt.node, &impl->node_activation.state);
	spa_graph_node_add(&impl->graph, &this->rt.node);

	impl->position.clock.rate = SPA_FRACTION(1, 48000);
	impl->position.size = DEFAULT_QUANTUM;

	check_properties(this);

	this->driver_node = this;
	this->driver_root = this;
	spa_list_append(&this->driver_list, &this->driver_link);

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

struct pw_global *pw_node_get_global(struct pw_node *node)
{
	return node->global;
}

const struct pw_properties *pw_node_get_properties(struct pw_node *node)
{
	return node->properties;
}

int pw_node_update_properties(struct pw_node *node, const struct spa_dict *dict)
{
	struct pw_resource *resource;
	int changed;

	changed = pw_properties_update(node->properties, dict);

	pw_log_debug("node %p: updated %d properties", node, changed);

	if (!changed)
		return 0;

	check_properties(node);

	node->info.props = &node->properties->dict;
	node->info.change_mask |= PW_NODE_CHANGE_MASK_PROPS;
	pw_node_events_info_changed(node, &node->info);

	if (node->global)
		spa_list_for_each(resource, &node->global->resource_list, link)
			pw_node_resource_info(resource, &node->info);

	node->info.change_mask = 0;

	return changed;
}

static void node_info(void *data, const struct spa_dict *info)
{
	struct pw_node *node = data;
	pw_node_update_properties(node, info);
}

static void node_done(void *data, int seq, int res)
{
	struct pw_node *node = data;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);

	pw_log_debug("node %p: async complete event %d %d %s", node, seq, res, spa_strerror(res));
	pw_work_queue_complete(impl->work, node, seq, res);
	pw_node_events_async_complete(node, seq, res);
}

static void node_event(void *data, struct spa_event *event)
{
	struct pw_node *node = data;

	pw_log_trace("node %p: event %d", node, SPA_EVENT_TYPE(event));
	pw_node_events_event(node, event);

	switch (SPA_NODE_EVENT_ID(event)) {
	case SPA_NODE_EVENT_PortsChanged:
		pw_node_update_ports(node);
		break;
	default:
		break;
	}

}

static void node_process(void *data, int status)
{
	struct pw_node *node = data, *driver, *root;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	bool is_driving = false;

	driver = node->driver_node;
	root = node->driver_root ? node->driver_root : driver;

	pw_log_trace("node %p: process driver:%d exported:%d %p %p", node,
			node->driver, node->exported, driver, driver->rt.driver);

	if (node->driver) {
		if (driver == node)
			is_driving = true;
		else if (node->rt.position && driver->rt.position)
			*node->rt.position = *driver->rt.position;
	}

	if (is_driving && (root->rt.driver->state->pending == 0 || !node->remote)) {
		struct timespec ts;
		struct spa_io_position *q = node->rt.position;

		if (root->rt.driver->state->pending != 0) {
			pw_log_warn("node %p: graph not finished", node);
		}

		if (!node->rt.clock) {
			clock_gettime(CLOCK_MONOTONIC, &ts);
			q->clock.nsec = SPA_TIMESPEC_TO_NSEC(&ts);
			q->clock.position = impl->next_position;
			q->clock.delay = 0;
		}

		pw_log_trace("node %p: run %"PRIu64" %d/%d %"PRIu64" %"PRIi64" %d", node,
				q->clock.nsec, q->clock.rate.num, q->clock.rate.denom,
				q->clock.position, q->clock.delay, q->size);

		spa_graph_run(root->rt.driver);

		impl->next_position += q->size;
	}
	else
		spa_graph_node_trigger(&node->rt.node);
}

static void node_reuse_buffer(void *data, uint32_t port_id, uint32_t buffer_id)
{
	struct pw_node *node = data;
        struct spa_graph_port *p, *pp;

	spa_list_for_each(p, &node->rt.node.ports[SPA_DIRECTION_INPUT], link) {
		if (p->port_id != port_id)
			continue;

		if ((pp = p->peer) != NULL)
			spa_graph_node_reuse_buffer(pp->node, pp->port_id, buffer_id);
		break;
	}
}

static const struct spa_node_callbacks node_callbacks = {
	SPA_VERSION_NODE_CALLBACKS,
	.info = node_info,
	.done = node_done,
	.event = node_event,
	.process = node_process,
	.reuse_buffer = node_reuse_buffer,
};

void pw_node_set_implementation(struct pw_node *node,
				struct spa_node *spa_node)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);

	node->node = spa_node;
	spa_node_set_callbacks(node->node, &node_callbacks, node);
	spa_graph_node_set_callbacks(&node->rt.node, &spa_graph_node_impl_default, spa_node);

	if (spa_node_set_io(node->node,
			    SPA_IO_Position,
			    &impl->position, sizeof(struct spa_io_position)) >= 0) {
		pw_log_debug("node %p: set position %p", node, &impl->position);
		node->rt.position = &impl->position;
	}
	if (spa_node_set_io(node->node,
			    SPA_IO_Clock,
			    &impl->position.clock, sizeof(struct spa_io_position)) >= 0) {
		pw_log_debug("node %p: set clock %p", node, &impl->position.clock);
		node->rt.clock = &impl->position.clock;
	}
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
	struct pw_node *n, *t;
	struct pw_port *port;

	pw_log_debug("node %p: destroy", impl);
	pw_node_events_destroy(node);

	pause_node(node);
	suspend_node(node);

	pw_log_debug("node %p: driver node %p", impl, node->driver_node);

	/* move all nodes driven by us to their own driver */
	spa_list_for_each_safe(n, t, &node->driver_list, driver_link)
		if (n != node)
			pw_node_set_driver(n, NULL);

	if (node->driver_node != node) {
		/* remove ourself from the (other) driver node */
		spa_list_remove(&node->driver_link);
		recalc_quantum(node->driver_node);
	}

	if (node->registered)
		spa_list_remove(&node->link);

	pw_log_debug("node %p: unlink ports", node);
	spa_list_for_each(port, &node->input_ports, link)
		pw_port_unlink(port);
	spa_list_for_each(port, &node->output_ports, link)
		pw_port_unlink(port);

	pw_log_debug("node %p: destroy ports", node);
	spa_list_consume(port, &node->input_ports, link)
		pw_port_destroy(port);
	spa_list_consume(port, &node->output_ports, link)
		pw_port_destroy(port);

	if (node->global) {
		spa_hook_remove(&node->global_listener);
		pw_global_destroy(node->global);
	}

	pw_log_debug("node %p: free", node);
	pw_node_events_free(node);

	pw_work_queue_destroy(impl->work);

	pw_map_clear(&node->input_port_map);
	pw_map_clear(&node->output_port_map);

	pw_properties_free(node->properties);

	clear_info(node);

	free(impl);
}

int pw_node_for_each_port(struct pw_node *node,
			  enum pw_direction direction,
			  int (*callback) (void *data, struct pw_port *port),
			  void *data)
{
	struct spa_list *ports;
	struct pw_port *p, *t;
	int res;

	if (direction == PW_DIRECTION_INPUT)
		ports = &node->input_ports;
	else
		ports = &node->output_ports;

	spa_list_for_each_safe(p, t, ports, link)
		if ((res = callback(data, p)) != 0)
			return res;
	return 0;
}

int pw_node_for_each_param(struct pw_node *node,
			   uint32_t param_id,
			   uint32_t index, uint32_t max,
			   const struct spa_pod *filter,
			   int (*callback) (void *data,
					    uint32_t id, uint32_t index, uint32_t next,
					    struct spa_pod *param),
			   void *data)
{
	int res = 0;
	uint32_t idx, count;
	uint8_t buf[4096];
	struct spa_pod_builder b = { 0 };
	struct spa_pod *param;

	if (max == 0)
		max = UINT32_MAX;

	for (count = 0; count < max; count++) {
		spa_pod_builder_init(&b, buf, sizeof(buf));

		idx = index;
		if ((res = spa_node_enum_params(node->node,
						param_id, &index,
						filter, &param, &b)) <= 0)
			break;

		if ((res = callback(data, param_id, idx, index, param)) != 0)
			break;
	}
	return res;
}

struct pw_port *
pw_node_find_port(struct pw_node *node, enum pw_direction direction, uint32_t port_id)
{
	struct pw_port *port, *p;
	struct pw_map *portmap;
	struct spa_list *ports;

	if (direction == PW_DIRECTION_INPUT) {
		portmap = &node->input_port_map;
		ports = &node->input_ports;
	} else {
		portmap = &node->output_port_map;
		ports = &node->output_ports;
	}

	if (port_id != SPA_ID_INVALID)
		port = pw_map_lookup(portmap, port_id);
	else {
		port = NULL;
		/* try to find an unlinked port */
		spa_list_for_each(p, ports, link) {
			if (spa_list_is_empty(&p->links)) {
				port = p;
				break;
			}
			/* We can use this port if it can multiplex */
			if (SPA_FLAG_CHECK(p->mix_flags, PW_PORT_MIX_FLAG_MULTI))
				port = p;
		}
	}
	pw_log_debug("node %p: return port %p", node, port);
	return port;
}

uint32_t pw_node_get_free_port_id(struct pw_node *node, enum pw_direction direction)
{
	uint32_t n_ports, max_ports;
	struct pw_map *portmap;
	uint32_t port_id;

	if (direction == PW_DIRECTION_INPUT) {
		max_ports = node->info.max_input_ports;
		n_ports = node->info.n_input_ports;
		portmap = &node->input_port_map;
	} else {
		max_ports = node->info.max_output_ports;
		n_ports = node->info.n_output_ports;
		portmap = &node->output_port_map;
	}
	pw_log_debug("node %p: direction %s n_ports:%u max_ports:%u",
			node, pw_direction_as_string(direction), n_ports, max_ports);

	if (n_ports >= max_ports)
		goto no_mem;

	port_id = pw_map_insert_new(portmap, NULL);
	if (port_id == SPA_ID_INVALID)
		goto no_mem;

	pw_log_debug("node %p: free port %d", node, port_id);

	return port_id;

      no_mem:
	pw_log_warn("no more port available");
	return SPA_ID_INVALID;
}

static void on_state_complete(void *obj, void *data, int res, uint32_t seq)
{
	struct pw_node *node = obj;
	enum pw_node_state state = SPA_PTR_TO_INT(data);
	char *error = NULL;

	pw_log_debug("node %p: state complete %d", node, res);
	if (SPA_RESULT_IS_ERROR(res)) {
		asprintf(&error, "error changing node state: %d", res);
		state = PW_NODE_STATE_ERROR;
	}
	node_update_state(node, state, error);
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
	int res = 0;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	enum pw_node_state old = node->info.state;

	pw_log_debug("node %p: set state %s -> %s, active %d", node,
			pw_node_state_as_string(old),
			pw_node_state_as_string(state),
			node->active);

	if (old == state)
		return 0;

	pw_node_events_state_request(node, state);

	switch (state) {
	case PW_NODE_STATE_CREATING:
		return -EIO;

	case PW_NODE_STATE_SUSPENDED:
		res = suspend_node(node);
		break;

	case PW_NODE_STATE_IDLE:
		if (node->active && impl->pause_on_idle)
			res = pause_node(node);
		break;

	case PW_NODE_STATE_RUNNING:
		if (node->active) {
			node_activate(node);
			res = start_node(node);
		}
		break;

	case PW_NODE_STATE_ERROR:
		break;
	}
	if (SPA_RESULT_IS_ERROR(res))
		return res;

	pw_work_queue_add(impl->work,
			  node, res, on_state_complete, SPA_INT_TO_PTR(state));

	return res;
}

int pw_node_set_active(struct pw_node *node, bool active)
{
	bool old = node->active;

	if (old != active) {
		pw_log_debug("node %p: %s", node, active ? "activate" : "deactivate");
		node->active = active;
		pw_node_events_active_changed(node, active);
		if (active) {
			if (node->enabled)
				node_activate(node);
		}
		else {
			node->active = true;
			pw_node_set_state(node, PW_NODE_STATE_IDLE);
			node->active = false;
		}

	}
	return 0;
}

bool pw_node_is_active(struct pw_node *node)
{
	return node->active;
}

int pw_node_set_enabled(struct pw_node *node, bool enabled)
{
	bool old = node->enabled;

	if (old != enabled) {
		pw_log_debug("node %p: %s", node, enabled ? "enable" : "disable");
		node->enabled = enabled;
		pw_node_events_enabled_changed(node, enabled);

		if (enabled) {
			if (node->active)
				node_activate(node);
		}
		else {
			pw_node_set_state(node, PW_NODE_STATE_SUSPENDED);
		}
	}
	return 0;
}

bool pw_node_is_enabled(struct pw_node *node)
{
	return node->enabled;
}
