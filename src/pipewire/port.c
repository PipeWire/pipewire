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

#include <spa/pod/parser.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"
#include "pipewire/port.h"

/** \cond */
struct impl {
	struct pw_port this;
};

struct resource_data {
	struct spa_hook resource_listener;
	struct pw_port *port;
};

/** \endcond */


static void port_update_state(struct pw_port *port, enum pw_port_state state)
{
	if (port->state != state) {
		pw_log_debug("port %p: state %d -> %d", port, port->state, state);
		port->state = state;
		spa_hook_list_call(&port->listener_list, struct pw_port_events, state_changed, state);
	}
}

static int schedule_tee_input(struct spa_node *data)
{
	struct pw_port *this = SPA_CONTAINER_OF(data, struct pw_port, mix_node);
	struct spa_graph_node *node = &this->rt.mix_node;
	struct spa_graph_port *p;
	struct spa_io_buffers *io = this->rt.mix_port.io;

	if (!spa_list_is_empty(&node->ports[SPA_DIRECTION_OUTPUT])) {
		pw_log_trace("port %p: tee input %d %d", this, io->status, io->buffer_id);
		spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link) {
			pw_log_trace("port %p: port %d %d %p->%p", this,
					p->port_id, p->flags, io, p->io);
			if (p->flags & SPA_GRAPH_PORT_FLAG_DISABLED)
				continue;
			*p->io = *io;
		}
	}
        return io->status;
}
static int schedule_tee_output(struct spa_node *data)
{
	struct pw_port *this = SPA_CONTAINER_OF(data, struct pw_port, mix_node);
	struct spa_graph_node *node = &this->rt.mix_node;
	struct spa_graph_port *p;
	struct spa_io_buffers *io = this->rt.mix_port.io;

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_OUTPUT], link) {
		pw_log_trace("port %p: port %d %d %p->%p %d %d",
				this, p->port_id, p->flags, p->io, io,
				p->io->status, p->io->buffer_id);
		if (p->flags & SPA_GRAPH_PORT_FLAG_DISABLED)
			continue;
		*io = *p->io;
	}
	pw_log_trace("port %p: tee output %d %d", this, io->status, io->buffer_id);
	return io->status;
}

static int schedule_tee_reuse_buffer(struct spa_node *data, uint32_t port_id, uint32_t buffer_id)
{
	struct pw_port *this = SPA_CONTAINER_OF(data, struct pw_port, mix_node);
	struct spa_graph_port *p = &this->rt.mix_port, *pp;

	if ((pp = p->peer) != NULL) {
		pw_log_trace("port %p: tee reuse buffer %d %d", this, port_id, buffer_id);
		spa_node_port_reuse_buffer(pp->node->implementation, port_id, buffer_id);
	}
	return 0;
}

static const struct spa_node schedule_tee_node = {
	SPA_VERSION_NODE,
	NULL,
	.process_input = schedule_tee_input,
	.process_output = schedule_tee_output,
	.port_reuse_buffer = schedule_tee_reuse_buffer,
};

static int schedule_mix_input(struct spa_node *data)
{
	struct pw_port *this = SPA_CONTAINER_OF(data, struct pw_port, mix_node);
	struct spa_graph_node *node = &this->rt.mix_node;
	struct spa_graph_port *p;
	struct spa_io_buffers *io = this->rt.mix_port.io;

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		if (p->flags & SPA_GRAPH_PORT_FLAG_DISABLED)
			continue;
		pw_log_trace("port %p: mix input %d %p->%p %d %d", this,
				p->port_id, p->io, io, p->io->status, p->io->buffer_id);
		*io = *p->io;
		break;
	}
	return io->status;
}

static int schedule_mix_output(struct spa_node *data)
{
	struct pw_port *this = SPA_CONTAINER_OF(data, struct pw_port, mix_node);
	struct spa_graph_node *node = &this->rt.mix_node;
	struct spa_graph_port *p;
	struct spa_io_buffers *io = this->rt.mix_port.io;

	if (!spa_list_is_empty(&node->ports[SPA_DIRECTION_INPUT])) {
		spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
			pw_log_trace("port %p: port %d %d %p->%p", this,
					p->port_id, p->flags, io, p->io);
			if (p->flags & SPA_GRAPH_PORT_FLAG_DISABLED)
				continue;
			*p->io = *io;
		}
	}
	else {
		io->status = SPA_STATUS_HAVE_BUFFER;
		io->buffer_id = SPA_ID_INVALID;
	}
	pw_log_trace("port %p: output %d %d", this, io->status, io->buffer_id);
	return io->status;
}

static int schedule_mix_reuse_buffer(struct spa_node *data, uint32_t port_id, uint32_t buffer_id)
{
	struct pw_port *this = SPA_CONTAINER_OF(data, struct pw_port, mix_node);
	struct spa_graph_node *node = &this->rt.mix_node;
	struct spa_graph_port *p, *pp;

	spa_list_for_each(p, &node->ports[SPA_DIRECTION_INPUT], link) {
		if ((pp = p->peer) != NULL) {
			pw_log_trace("port %p: reuse buffer %d %d", this, port_id, buffer_id);
			spa_node_port_reuse_buffer(pp->node->implementation, port_id, buffer_id);
		}
	}
	return 0;
}

static const struct spa_node schedule_mix_node = {
	SPA_VERSION_NODE,
	NULL,
	.process_input = schedule_mix_input,
	.process_output = schedule_mix_output,
	.port_reuse_buffer = schedule_mix_reuse_buffer,
};

int pw_port_init_mix(struct pw_port *port, struct pw_port_mix *mix)
{
	uint32_t id;
	int res = 0;
	const struct pw_port_implementation *pi = port->implementation;

	id = pw_map_insert_new(&port->mix_port_map, mix);

	spa_graph_port_init(&mix->port,
			    port->direction, id,
			    SPA_GRAPH_PORT_FLAG_DISABLED,
			    NULL);

	mix->port.scheduler_data = port;

	if (pi && pi->init_mix)
		res = pi->init_mix(port->implementation_data, mix);

	pw_log_debug("port %p: init mix %d.%d io %p", port,
			port->port_id, mix->port.port_id, mix->port.io);

	return res;
}
int pw_port_release_mix(struct pw_port *port, struct pw_port_mix *mix)
{
	int res = 0;
	const struct pw_port_implementation *pi = port->implementation;

	pw_map_remove(&port->mix_port_map, mix->port.port_id);

	if (pi && pi->release_mix)
		res = pi->release_mix(port->implementation_data, mix);

	pw_log_debug("port %p: release mix %d.%d", port,
			port->port_id, mix->port.port_id);

	return res;
}

struct pw_port *pw_port_new(enum pw_direction direction,
			    uint32_t port_id,
			    struct pw_properties *properties,
			    size_t user_data_size)
{
	struct impl *impl;
	struct pw_port *this;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	pw_log_debug("port %p: new %s %d", this,
			pw_direction_as_string(direction), port_id);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto no_mem;

	this->direction = direction;
	this->port_id = port_id;
	this->properties = properties;
	this->state = PW_PORT_STATE_INIT;
	this->rt.io = SPA_IO_BUFFERS_INIT;

        if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	this->info.props = &this->properties->dict;

	spa_list_init(&this->links);
	spa_list_init(&this->control_list[0]);
	spa_list_init(&this->control_list[1]);

	spa_list_init(&this->resource_list);

	spa_hook_list_init(&this->listener_list);

	spa_graph_port_init(&this->rt.port,
			    this->direction,
			    this->port_id,
			    0,
			    &this->rt.io);
	spa_graph_node_init(&this->rt.mix_node);

	this->mix_node = this->direction == PW_DIRECTION_INPUT ?
				schedule_mix_node :
				schedule_tee_node;
	spa_graph_node_set_implementation(&this->rt.mix_node, &this->mix_node);
	pw_map_init(&this->mix_port_map, 64, 64);

	spa_graph_port_init(&this->rt.mix_port,
			    pw_direction_reverse(this->direction),
			    0,
			    0,
			    &this->rt.io);

	this->rt.mix_port.scheduler_data = this;
	this->rt.port.scheduler_data = this;

	return this;

       no_mem:
	free(impl);
	return NULL;
}

enum pw_direction pw_port_get_direction(struct pw_port *port)
{
	return port->direction;
}

uint32_t pw_port_get_id(struct pw_port *port)
{
	return port->port_id;
}

const struct pw_properties *pw_port_get_properties(struct pw_port *port)
{
	return port->properties;
}

int pw_port_update_properties(struct pw_port *port, const struct spa_dict *dict)
{
	struct pw_resource *resource;
	uint32_t i;

	for (i = 0; i < dict->n_items; i++)
		pw_properties_set(port->properties, dict->items[i].key, dict->items[i].value);

	port->info.props = &port->properties->dict;

	port->info.change_mask |= PW_PORT_CHANGE_MASK_PROPS;
	spa_hook_list_call(&port->listener_list, struct pw_port_events,
			info_changed, &port->info);

	spa_list_for_each(resource, &port->resource_list, link)
		pw_port_resource_info(resource, &port->info);

	port->info.change_mask = 0;

	return 0;
}

struct pw_node *pw_port_get_node(struct pw_port *port)
{
	return port->node;
}

void pw_port_add_listener(struct pw_port *port,
			  struct spa_hook *listener,
			  const struct pw_port_events *events,
			  void *data)
{
	spa_hook_list_append(&port->listener_list, listener, events, data);
}

void * pw_port_get_user_data(struct pw_port *port)
{
	return port->user_data;
}

static int do_add_port(struct spa_loop *loop,
		       bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
        struct pw_port *this = user_data;

	this->rt.port.flags = this->spa_info->flags;
	spa_graph_port_add(&this->node->rt.node, &this->rt.port);
	spa_graph_port_add(&this->rt.mix_node, &this->rt.mix_port);
	spa_graph_port_link(&this->rt.port, &this->rt.mix_port);

	return 0;
}

static int make_control(void *data, uint32_t id, uint32_t index, uint32_t next, struct spa_pod *param)
{
	struct pw_port *port = data;
	struct pw_node *node = port->node;
	pw_control_new(node->core, port, param, 0);
	return 0;
}

static void port_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	spa_list_remove(&resource->link);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = port_unbind_func,
};

static int reply_param(void *data, uint32_t id, uint32_t index, uint32_t next, struct spa_pod *param)
{
	struct pw_resource *resource = data;
	pw_port_resource_param(resource, id, index, next, param);
	return 0;
}

static void port_enum_params(void *object, uint32_t id, uint32_t index, uint32_t num,
		const struct spa_pod *filter)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct pw_port *port = data->port;

	pw_port_for_each_param(port, id, index, num, filter,
			reply_param, resource);
}

static const struct pw_port_proxy_methods port_methods = {
	PW_VERSION_NODE_PROXY_METHODS,
	.enum_params = port_enum_params
};

static void
global_bind(void *_data, struct pw_client *client, uint32_t permissions,
	       uint32_t version, uint32_t id)
{
	struct pw_port *this = _data;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	data->port = this;
	pw_resource_add_listener(resource, &data->resource_listener, &resource_events, resource);

	pw_resource_set_implementation(resource, &port_methods, resource);

	pw_log_debug("port %p: bound to %d", this, resource->id);

	spa_list_append(&this->resource_list, &resource->link);

	this->info.change_mask = ~0;
	pw_port_resource_info(resource, &this->info);
	this->info.change_mask = 0;
	return;

      no_mem:
	pw_log_error("can't create port resource");
	pw_core_resource_error(client->core_resource,
			       client->core_resource->id, -ENOMEM, "no memory");
	return;
}

static void global_destroy(void *object)
{
	struct pw_port *port = object;
	spa_hook_remove(&port->global_listener);
	port->global = NULL;
	pw_port_destroy(port);
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy,
	.bind = global_bind,
};

int pw_port_register(struct pw_port *port,
		     struct pw_client *owner,
		     struct pw_global *parent,
		     struct pw_properties *properties)
{
	struct pw_node *node = port->node;
	struct pw_core *core = node->core;

	port->global = pw_global_new(core,
				core->type.port, PW_VERSION_PORT,
				properties,
				port);
	if (port->global == NULL)
		return -ENOMEM;

	pw_global_add_listener(port->global, &port->global_listener, &global_events, port);

	return pw_global_register(port->global, owner, parent);
}

int pw_port_add(struct pw_port *port, struct pw_node *node)
{
	uint32_t port_id = port->port_id;
	struct pw_core *core = node->core;
	struct pw_type *t = &core->type;
	const char *str, *dir;

	if (port->node != NULL)
		return -EEXIST;

	port->node = node;

	spa_node_port_get_info(node->node,
			       port->direction, port_id,
			       &port->spa_info);

	if (port->spa_info->props)
		pw_port_update_properties(port, port->spa_info->props);

	dir = port->direction == PW_DIRECTION_INPUT ?  "in" : "out";

	if ((str = pw_properties_get(port->properties, "port.name")) == NULL) {
		pw_properties_setf(port->properties, "port.name", "%s_%d", dir, port_id);
	}
	pw_properties_set(port->properties, "port.direction", dir);

	if (SPA_FLAG_CHECK(port->spa_info->flags, SPA_PORT_INFO_FLAG_PHYSICAL))
		pw_properties_set(port->properties, "port.physical", "1");
	if (SPA_FLAG_CHECK(port->spa_info->flags, SPA_PORT_INFO_FLAG_TERMINAL))
		pw_properties_set(port->properties, "port.terminal", "1");

	pw_log_debug("port %p: add to node %p %08x", port, node, port->spa_info->flags);
	if (port->direction == PW_DIRECTION_INPUT) {
		spa_list_append(&node->input_ports, &port->link);
		pw_map_insert_at(&node->input_port_map, port_id, port);
		node->info.n_input_ports++;
		node->info.change_mask |= PW_NODE_CHANGE_MASK_INPUT_PORTS;
	}
	else {
		spa_list_append(&node->output_ports, &port->link);
		pw_map_insert_at(&node->output_port_map, port_id, port);
		node->info.n_output_ports++;
		node->info.change_mask |= PW_NODE_CHANGE_MASK_OUTPUT_PORTS;
	}

	pw_port_for_each_param(port, t->param_io.idPropsOut, 0, 0, NULL, make_control, port);
	pw_port_for_each_param(port, t->param_io.idPropsIn, 0, 0, NULL, make_control, port);

	pw_log_debug("port %p: setting node io", port);
	spa_node_port_set_io(node->node,
			     port->direction, port_id,
			     t->io.Buffers,
			     port->rt.port.io, sizeof(*port->rt.port.io));

	if (node->global)
		pw_port_register(port, node->global->owner, node->global,
				pw_properties_copy(port->properties));

	pw_loop_invoke(node->data_loop, do_add_port, SPA_ID_INVALID, NULL, 0, false, port);

	if (port->state <= PW_PORT_STATE_INIT)
		port_update_state(port, PW_PORT_STATE_CONFIGURE);

	spa_hook_list_call(&node->listener_list, struct pw_node_events, port_added, port);

	return 0;
}

void pw_port_unlink(struct pw_port *port)
{
	struct pw_link *l, *t;

	if (port->direction == PW_DIRECTION_OUTPUT) {
		spa_list_for_each_safe(l, t, &port->links, output_link)
			pw_link_destroy(l);
	}
	else {
		spa_list_for_each_safe(l, t, &port->links, input_link)
			pw_link_destroy(l);
	}
}

static int do_remove_port(struct spa_loop *loop,
			  bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
        struct pw_port *this = user_data;
	struct spa_graph_port *p;

	spa_graph_port_unlink(&this->rt.port);
	spa_graph_port_remove(&this->rt.port);

	spa_list_for_each(p, &this->rt.mix_node.ports[this->direction], link)
		spa_graph_port_remove(p);

	spa_graph_port_remove(&this->rt.mix_port);

	return 0;
}

static void pw_port_remove(struct pw_port *port)
{
	struct pw_node *node = port->node;

	if (node == NULL)
		return;

	pw_log_debug("port %p: remove", port);

	pw_loop_invoke(port->node->data_loop, do_remove_port,
		       SPA_ID_INVALID, NULL, 0, true, port);

	if (port->direction == PW_DIRECTION_INPUT) {
		pw_map_remove(&node->input_port_map, port->port_id);
		node->info.n_input_ports--;
	}
	else {
		pw_map_remove(&node->output_port_map, port->port_id);
		node->info.n_output_ports--;
	}
	spa_list_remove(&port->link);
	spa_hook_list_call(&node->listener_list, struct pw_node_events, port_removed, port);
}

void pw_port_destroy(struct pw_port *port)
{
	struct pw_control *control, *ctemp;
	struct pw_resource *resource, *tmp;

	pw_log_debug("port %p: destroy", port);

	spa_hook_list_call(&port->listener_list, struct pw_port_events, destroy);

	pw_port_remove(port);

	spa_list_for_each_safe(control, ctemp, &port->control_list[0], port_link)
		pw_control_destroy(control);
	spa_list_for_each_safe(control, ctemp, &port->control_list[1], port_link)
		pw_control_destroy(control);

	if (port->global) {
		spa_hook_remove(&port->global_listener);
		pw_global_destroy(port->global);
	}
	spa_list_for_each_safe(resource, tmp, &port->resource_list, link)
		pw_resource_destroy(resource);

	pw_log_debug("port %p: free", port);
	spa_hook_list_call(&port->listener_list, struct pw_port_events, free);

	free_allocation(&port->allocation);

	pw_map_clear(&port->mix_port_map);

	if (port->properties)
		pw_properties_free(port->properties);

	free(port);
}

static int
do_port_command(struct spa_loop *loop,
              bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
        struct pw_port *port = user_data;
	struct pw_node *node = port->node;
	return spa_node_port_send_command(node->node, port->direction, port->port_id, data);
}

int pw_port_send_command(struct pw_port *port, bool block, const struct spa_command *command)
{
	return pw_loop_invoke(port->node->data_loop, do_port_command, 0,
			command, SPA_POD_SIZE(command), block, port);
}

int pw_port_for_each_param(struct pw_port *port,
			   uint32_t param_id,
			   uint32_t index, uint32_t max,
			   const struct spa_pod *filter,
			   int (*callback) (void *data,
					    uint32_t id, uint32_t index, uint32_t next,
					    struct spa_pod *param),
			   void *data)
{
	int res = 0;
	uint8_t buf[4096];
	struct spa_pod_builder b = { 0 };
	uint32_t idx, count;
	struct pw_node *node = port->node;
	struct spa_pod *param;

	if (max == 0)
		max = UINT32_MAX;

	for (count = 0; count < max; count++) {
		spa_pod_builder_init(&b, buf, sizeof(buf));
		idx = index;
		if ((res = spa_node_port_enum_params(node->node,
						     port->direction, port->port_id,
						     param_id, &index,
						     filter, &param, &b)) <= 0)
			break;

		if ((res = callback(data, param_id, idx, index, param)) != 0)
			break;
	}
	return res;
}

struct param_filter {
	struct pw_port *in_port;
	struct pw_port *out_port;
	uint32_t in_param_id;
	uint32_t out_param_id;
	int (*callback) (void *data, uint32_t id, uint32_t index, uint32_t next, struct spa_pod *param);
	void *data;
	uint32_t n_params;
};

static int do_filter(void *data, uint32_t id, uint32_t index, uint32_t next, struct spa_pod *param)
{
	struct param_filter *f = data;
	f->n_params++;
	return pw_port_for_each_param(f->out_port, f->out_param_id, 0, 0, param, f->callback, f->data);
}

int pw_port_for_each_filtered_param(struct pw_port *in_port,
				    struct pw_port *out_port,
				    uint32_t in_param_id,
				    uint32_t out_param_id,
				    int (*callback) (void *data,
						     uint32_t id, uint32_t index, uint32_t next,
						     struct spa_pod *param),
				    void *data)
{
	int res;
	struct param_filter filter = { in_port, out_port, in_param_id, out_param_id, callback, data, 0 };

	if ((res = pw_port_for_each_param(in_port, in_param_id, 0, 0, NULL, do_filter, &filter)) < 0)
		return res;

	if (filter.n_params == 0)
		res = do_filter(&filter, 0, 0, 0, NULL);

	return res;
}

int pw_port_set_param(struct pw_port *port, uint32_t id, uint32_t flags,
		      const struct spa_pod *param)
{
	int res;
	struct pw_node *node = port->node;
	struct pw_core *core = node->core;
	struct pw_type *t = &core->type;

	res = spa_node_port_set_param(node->node, port->direction, port->port_id, id, flags, param);
	pw_log_debug("port %p: set param %s: %d (%s)", port,
			spa_type_map_get_type(t->map, id), res, spa_strerror(res));

	if (id == t->param.idFormat) {
		if (param == NULL || res < 0) {
			free_allocation(&port->allocation);
			port->allocated = false;
			port_update_state (port, PW_PORT_STATE_CONFIGURE);
		}
		else if (!SPA_RESULT_IS_ASYNC(res)) {
			port_update_state (port, PW_PORT_STATE_READY);
		}
	}
	return res;
}

int pw_port_use_buffers(struct pw_port *port, uint32_t mix_id,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	int res;
	struct pw_node *node = port->node;
	struct pw_port_mix *mix;
	struct spa_graph_port *p;

	pw_log_debug("port %p: %d.%d: %d buffers ", port, port->port_id, mix_id, n_buffers);

	if (n_buffers == 0 && port->state <= PW_PORT_STATE_READY)
		return 0;

	if (n_buffers > 0 && port->state < PW_PORT_STATE_READY)
		return -EIO;

	if ((mix = pw_map_lookup(&port->mix_port_map, mix_id)) == NULL)
		return -EIO;

	p = &mix->port;

	if (port->mix_node.port_use_buffers)
		res = spa_node_port_use_buffers(&port->mix_node, p->direction, p->port_id, buffers, n_buffers);
	else
		res = spa_node_port_use_buffers(node->node, port->direction, port->port_id, buffers, n_buffers);

	pw_log_debug("port %p: %d.%d: use %d buffers: %d (%s)", port,
			port->port_id, mix_id, n_buffers, res, spa_strerror(res));

	port->allocated = false;

	free_allocation(&port->allocation);

	if (res < 0) {
		n_buffers = 0;
		buffers = NULL;
	}
	mix->n_buffers = n_buffers;
	mix->buffers = buffers;

	if (n_buffers == 0)
		port_update_state (port, PW_PORT_STATE_READY);
	else if (!SPA_RESULT_IS_ASYNC(res))
		port_update_state (port, PW_PORT_STATE_PAUSED);

	return res;
}

int pw_port_alloc_buffers(struct pw_port *port, uint32_t mix_id,
			  struct spa_pod **params, uint32_t n_params,
			  struct spa_buffer **buffers, uint32_t *n_buffers)
{
	int res;
	struct pw_node *node = port->node;
	struct pw_port_mix *mix;
	struct spa_graph_port *p;

	if (port->state < PW_PORT_STATE_READY)
		return -EIO;

	if ((mix = pw_map_lookup(&port->mix_port_map, mix_id)) == NULL)
		return -EIO;

	p = &mix->port;

	if (port->mix_node.port_use_buffers)
		res = spa_node_port_alloc_buffers(&port->mix_node, p->direction, p->port_id,
						  params, n_params,
						  buffers, n_buffers);
	else
		res = spa_node_port_alloc_buffers(node->node, port->direction, port->port_id,
						  params, n_params,
						  buffers, n_buffers);

	pw_log_debug("port %p: %d.%d alloc %d buffers: %d (%s)", port,
			port->port_id, mix_id, *n_buffers, res, spa_strerror(res));

	free_allocation(&port->allocation);

	if (res < 0) {
		*n_buffers = 0;
		buffers = NULL;
		port->allocated = false;
	}
	else {
		port->allocated = true;
	}
	mix->n_buffers = *n_buffers;
	mix->buffers = buffers;

	if (*n_buffers == 0)
		port_update_state (port, PW_PORT_STATE_READY);
	else if (!SPA_RESULT_IS_ASYNC(res))
		port_update_state (port, PW_PORT_STATE_PAUSED);

	return res;
}
