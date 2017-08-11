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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <spa/graph-scheduler3.h>

#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

#include <spa/node.h>

#include "spa-node.h"
#include "pipewire/private.h"

struct impl {
	struct pw_node *this;

	bool async_init;

	void *hnd;
        struct spa_handle *handle;
        struct spa_node *node;          /**< handle to SPA node */
	char *lib;
	char *factory_name;

	struct spa_hook node_listener;
};

struct port {
	struct pw_port *port;
	enum pw_direction direction;
	uint32_t port_id;
	struct spa_node *node;
};

static int port_impl_set_io(void *data, struct spa_port_io *io)
{
	struct port *p = data;
	return spa_node_port_set_io(p->node, p->direction, p->port_id, io);
}

static int port_impl_enum_formats(void *data,
				  struct spa_format **format,
				  const struct spa_format *filter,
				  int32_t index)
{
	struct port *p = data;
	return spa_node_port_enum_formats(p->node, p->direction, p->port_id, format, filter, index);
}

static int port_impl_set_format(void *data, uint32_t flags, const struct spa_format *format)
{
	struct port *p = data;
	return spa_node_port_set_format(p->node, p->direction, p->port_id, flags, format);
}

static int port_impl_get_format(void *data, const struct spa_format **format)
{
	struct port *p = data;
	return spa_node_port_get_format(p->node, p->direction, p->port_id, format);
}

static int port_impl_get_info(void *data, const struct spa_port_info **info)
{
	struct port *p = data;
	return spa_node_port_get_info(p->node, p->direction, p->port_id, info);
}

static int port_impl_enum_params(void *data, uint32_t index, struct spa_param **param)
{
	struct port *p = data;
	return spa_node_port_enum_params(p->node, p->direction, p->port_id, index, param);
}

static int port_impl_set_param(void *data, struct spa_param *param)
{
	struct port *p = data;
	return spa_node_port_set_param(p->node, p->direction, p->port_id, param);
}

static int port_impl_use_buffers(void *data, struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct port *p = data;
	return spa_node_port_use_buffers(p->node, p->direction, p->port_id, buffers, n_buffers);
}

static int port_impl_alloc_buffers(void *data,
				   struct spa_param **params, uint32_t n_params,
				   struct spa_buffer **buffers, uint32_t *n_buffers)
{
	struct port *p = data;
	return spa_node_port_alloc_buffers(p->node, p->direction, p->port_id,
					   params, n_params, buffers, n_buffers);
}

static int port_impl_reuse_buffer(void *data, uint32_t buffer_id)
{
	struct port *p = data;
	return spa_node_port_reuse_buffer(p->node, p->port_id, buffer_id);
}

static int port_impl_send_command(void *data, struct spa_command *command)
{
	struct port *p = data;
        return spa_node_port_send_command(p->node,
                                          p->direction,
                                          p->port_id,
					  command);
}

const struct pw_port_implementation port_impl = {
	PW_VERSION_PORT_IMPLEMENTATION,
	.set_io = port_impl_set_io,
	.enum_formats = port_impl_enum_formats,
	.set_format = port_impl_set_format,
	.get_format = port_impl_get_format,
	.get_info = port_impl_get_info,
	.enum_params = port_impl_enum_params,
	.set_param = port_impl_set_param,
	.use_buffers = port_impl_use_buffers,
	.alloc_buffers = port_impl_alloc_buffers,
	.reuse_buffer = port_impl_reuse_buffer,
	.send_command = port_impl_send_command,
};

static struct pw_port *
make_port(struct impl *impl, enum pw_direction direction, uint32_t port_id)
{
	struct pw_node *node = impl->this;
	struct pw_port *port;
	struct port *p;

	port = pw_port_new(direction, port_id, NULL, sizeof(struct port));
	if (port == NULL)
		return NULL;

	p = pw_port_get_user_data(port);
	p->port = port;
	p->direction = direction;
	p->port_id = port_id;
	p->node = impl->node;

	pw_port_set_implementation(port, &port_impl, p);
	pw_port_add(port, node);

	return port;
}

static void update_port_ids(struct impl *impl)
{
	struct pw_node *this = impl->this;
        uint32_t *input_port_ids, *output_port_ids;
        uint32_t n_input_ports, n_output_ports, max_input_ports, max_output_ports;
        uint32_t i;
        struct spa_list *ports;

        spa_node_get_n_ports(impl->node,
                             &n_input_ports, &max_input_ports, &n_output_ports, &max_output_ports);

        this->info.max_input_ports = max_input_ports;
        this->info.max_output_ports = max_output_ports;

        input_port_ids = alloca(sizeof(uint32_t) * n_input_ports);
        output_port_ids = alloca(sizeof(uint32_t) * n_output_ports);

        spa_node_get_port_ids(impl->node,
                              max_input_ports, input_port_ids, max_output_ports, output_port_ids);

        pw_log_debug("node %p: update_port ids %u/%u, %u/%u", this,
                     n_input_ports, max_input_ports, n_output_ports, max_output_ports);

	i = 0;
        ports = &this->input_ports;
        while (true) {
                struct pw_port *p = (ports == &this->input_ports) ? NULL :
                    SPA_CONTAINER_OF(ports, struct pw_port, link);

                if (p && i < n_input_ports && p->port_id == input_port_ids[i]) {
                        pw_log_debug("node %p: exiting input port %d", this, input_port_ids[i]);
                        i++;
                        ports = ports->next;
                } else if ((p && i < n_input_ports && input_port_ids[i] < p->port_id)
                           || i < n_input_ports) {
                        struct pw_port *np;
                        pw_log_debug("node %p: input port added %d", this, input_port_ids[i]);
                        np = make_port(impl, PW_DIRECTION_INPUT, input_port_ids[i]);

                        ports = np->link.next;
                        i++;
                } else if (p) {
                        ports = ports->next;
                        pw_log_debug("node %p: input port removed %d", this, p->port_id);
                        pw_port_destroy(p);
                } else {
                        pw_log_debug("node %p: no more input ports", this);
                        break;
                }
        }

        i = 0;
        ports = &this->output_ports;
        while (true) {
                struct pw_port *p = (ports == &this->output_ports) ? NULL :
                    SPA_CONTAINER_OF(ports, struct pw_port, link);

                if (p && i < n_output_ports && p->port_id == output_port_ids[i]) {
                        pw_log_debug("node %p: exiting output port %d", this, output_port_ids[i]);
                        i++;
                        ports = ports->next;
                } else if ((p && i < n_output_ports && output_port_ids[i] < p->port_id)
                           || i < n_output_ports) {
                        struct pw_port *np;
                        pw_log_debug("node %p: output port added %d", this, output_port_ids[i]);
                        np = make_port(impl, PW_DIRECTION_OUTPUT, output_port_ids[i]);
                        ports = np->link.next;
                        i++;
                } else if (p) {
                        ports = ports->next;
                        pw_log_debug("node %p: output port removed %d", this, p->port_id);
                        pw_port_destroy(p);
                } else {
                        pw_log_debug("node %p: no more output ports", this);
                        break;
                }
        }
}


static int node_impl_get_props(void *data, struct spa_props **props)
{
	struct impl *impl = data;
	return spa_node_get_props(impl->node, props);
}

static int node_impl_set_props(void *data, const struct spa_props *props)
{
	struct impl *impl = data;
	return spa_node_set_props(impl->node, props);
}

static int node_impl_send_command(void *data, const struct spa_command *command)
{
	struct impl *impl = data;
	return spa_node_send_command(impl->node, command);
}

static struct pw_port*
node_impl_add_port(void *data,
		   enum pw_direction direction,
		   uint32_t port_id)
{
	struct impl *impl = data;
	int res;

	if ((res = spa_node_add_port(impl->node, direction, port_id)) < 0) {
		pw_log_error("node %p: could not add port %d %d", impl->this, port_id, res);
		return NULL;
	}

	return make_port(impl, direction, port_id);
}

static int node_impl_process_input(void *data)
{
	struct impl *impl = data;
	return spa_node_process_input(impl->node);
}

static int node_impl_process_output(void *data)
{
	struct impl *impl = data;
	return spa_node_process_output(impl->node);
}

static const struct pw_node_implementation node_impl = {
	PW_VERSION_NODE_IMPLEMENTATION,
	.get_props = node_impl_get_props,
	.set_props = node_impl_set_props,
	.send_command = node_impl_send_command,
	.add_port = node_impl_add_port,
	.process_input = node_impl_process_input,
	.process_output = node_impl_process_output,
};

static void pw_spa_node_destroy(void *data)
{
	struct impl *impl = data;
	struct pw_node *node = impl->this;

	pw_log_debug("spa-node %p: destroy", node);

	if (impl->handle) {
		spa_handle_clear(impl->handle);
		free(impl->handle);
	}
	free(impl->lib);
	free(impl->factory_name);
	if (impl->hnd)
		dlclose(impl->hnd);
}

static void complete_init(struct impl *impl)
{
        struct pw_node *this = impl->this;
	update_port_ids(impl);
	pw_node_register(this);
}

static void on_node_done(void *data, int seq, int res)
{
        struct impl *impl = data;
        struct pw_node *this = impl->this;

	if (impl->async_init) {
		complete_init(impl);
		impl->async_init = false;
	}

        pw_log_debug("spa-node %p: async complete event %d %d", this, seq, res);
	spa_hook_list_call(&this->listener_list, struct pw_node_events, async_complete, seq, res);
}

static void on_node_event(void *data, struct spa_event *event)
{
        struct impl *impl = data;
        struct pw_node *this = impl->this;

	spa_hook_list_call(&this->listener_list, struct pw_node_events, event, event);
}

static void on_node_need_input(void *data)
{
        struct impl *impl = data;
        struct pw_node *this = impl->this;
	spa_hook_list_call(&this->listener_list, struct pw_node_events, need_input);
}

static void on_node_have_output(void *data)
{
        struct impl *impl = data;
        struct pw_node *this = impl->this;
	spa_hook_list_call(&this->listener_list, struct pw_node_events, have_output);
}

static void
on_node_reuse_buffer(void *data, uint32_t port_id, uint32_t buffer_id)
{
        struct impl *impl = data;
        struct pw_node *this = impl->this;
        struct spa_graph_port *p, *pp;

        spa_list_for_each(p, &this->rt.node.ports[SPA_DIRECTION_INPUT], link) {
		if (p->port_id != port_id)
			continue;

		pp = p->peer;
		if (pp && pp->callbacks->reuse_buffer)
			pp->callbacks->reuse_buffer(pp->callbacks_data, buffer_id);

		break;
	}
}

static const struct spa_node_callbacks spa_node_callbacks = {
	SPA_VERSION_NODE_CALLBACKS,
	.done = on_node_done,
	.event = on_node_event,
	.need_input = on_node_need_input,
	.have_output = on_node_have_output,
	.reuse_buffer = on_node_reuse_buffer,
};

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = pw_spa_node_destroy,
};

struct pw_node *
pw_spa_node_new(struct pw_core *core,
		struct pw_resource *owner,
		struct pw_global *parent,
		const char *name,
		bool async,
		struct spa_node *node,
		struct spa_clock *clock,
		struct pw_properties *properties)
{
	struct pw_node *this;
	struct impl *impl;

	if (node->info) {
		uint32_t i;

		if (properties == NULL)
			properties = pw_properties_new(NULL, NULL);

		if (properties)
			return NULL;

		for (i = 0; i < node->info->n_items; i++)
			pw_properties_set(properties,
					  node->info->items[i].key,
					  node->info->items[i].value);
	}

	this = pw_node_new(core, owner, parent, name, properties, sizeof(struct impl));
	if (this == NULL)
		return NULL;

	this->clock = clock;

	impl = this->user_data;
	impl->this = this;
	impl->node = node;
	impl->async_init = async;

	pw_node_add_listener(this, &impl->node_listener, &node_events, impl);

	pw_node_set_implementation(this, &node_impl, impl);

	if (spa_node_set_callbacks(impl->node, &spa_node_callbacks, impl) < 0)
		pw_log_warn("spa-node %p: error setting callback", this);

	if (!async) {
		complete_init(impl);
	}

	return this;
}

static int
setup_props(struct pw_core *core, struct spa_node *spa_node, struct pw_properties *pw_props)
{
	int res;
	struct spa_props *props;
	void *state = NULL;
	const char *key;

	if ((res = spa_node_get_props(spa_node, &props)) != SPA_RESULT_OK) {
		pw_log_debug("spa_node_get_props failed: %d", res);
		return SPA_RESULT_ERROR;
	}

	while ((key = pw_properties_iterate(pw_props, &state))) {
		struct spa_pod_prop *prop;
		uint32_t id;

		if (!spa_type_is_a(key, SPA_TYPE_PROPS_BASE))
			continue;

		id = spa_type_map_get_id(core->type.map, key);
		if (id == SPA_ID_INVALID)
			continue;

		if ((prop = spa_pod_object_find_prop(&props->object, id))) {
			const char *value = pw_properties_get(pw_props, key);

			pw_log_info("configure prop %s", key);

			switch(prop->body.value.type) {
			case SPA_POD_TYPE_ID:
				SPA_POD_VALUE(struct spa_pod_id, &prop->body.value) =
					spa_type_map_get_id(core->type.map, value);
				break;
			case SPA_POD_TYPE_INT:
				SPA_POD_VALUE(struct spa_pod_int, &prop->body.value) = atoi(value);
				break;
			case SPA_POD_TYPE_LONG:
				SPA_POD_VALUE(struct spa_pod_long, &prop->body.value) = atoi(value);
				break;
			case SPA_POD_TYPE_FLOAT:
				SPA_POD_VALUE(struct spa_pod_float, &prop->body.value) = atof(value);
				break;
			case SPA_POD_TYPE_DOUBLE:
				SPA_POD_VALUE(struct spa_pod_double, &prop->body.value) = atof(value);
				break;
			case SPA_POD_TYPE_STRING:
				break;
			default:
				break;
			}
		}
	}

	if ((res = spa_node_set_props(spa_node, props)) != SPA_RESULT_OK) {
		pw_log_debug("spa_node_set_props failed: %d", res);
		return SPA_RESULT_ERROR;
	}
	return SPA_RESULT_OK;
}


struct pw_node *pw_spa_node_load(struct pw_core *core,
				 struct pw_resource *owner,
				 struct pw_global *parent,
				 const char *lib,
				 const char *factory_name,
				 const char *name,
				 struct pw_properties *properties)
{
	struct pw_node *this;
	struct impl *impl;
	struct spa_node *spa_node;
	struct spa_clock *spa_clock;
	int res;
	struct spa_handle *handle;
	void *hnd;
	uint32_t index;
	spa_handle_factory_enum_func_t enum_func;
	const struct spa_handle_factory *factory;
	void *iface;
	char *filename;
	const char *dir;
	bool async;

	if ((dir = getenv("SPA_PLUGIN_DIR")) == NULL)
		dir = PLUGINDIR;

	asprintf(&filename, "%s/%s.so", dir, lib);

	if ((hnd = dlopen(filename, RTLD_NOW)) == NULL) {
		pw_log_error("can't load %s: %s", filename, dlerror());
		goto open_failed;
	}
	if ((enum_func = dlsym(hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		pw_log_error("can't find enum function");
		goto no_symbol;
	}

	for (index = 0;; index++) {
		if ((res = enum_func(&factory, index)) < 0) {
			if (res != SPA_RESULT_ENUM_END)
				pw_log_error("can't enumerate factories: %d", res);
			goto enum_failed;
		}
		if (strcmp(factory->name, factory_name) == 0)
			break;
	}

	handle = calloc(1, factory->size);
	if ((res = spa_handle_factory_init(factory,
					   handle, NULL, core->support, core->n_support)) < 0) {
		pw_log_error("can't make factory instance: %d", res);
		goto init_failed;
	}
	async = SPA_RESULT_IS_ASYNC(res);

	if ((res = spa_handle_get_interface(handle, core->type.spa_node, &iface)) < 0) {
		pw_log_error("can't get node interface %d", res);
		goto interface_failed;
	}
	spa_node = iface;

	if ((res = spa_handle_get_interface(handle, core->type.spa_clock, &iface)) < 0) {
		iface = NULL;
	}
	spa_clock = iface;

	if (properties != NULL) {
		if (setup_props(core, spa_node, properties) != SPA_RESULT_OK) {
			pw_log_debug("Unrecognized properties");
		}
	}

	this = pw_spa_node_new(core, owner, parent, name, async, spa_node, spa_clock, properties);
	impl->hnd = hnd;
	impl->handle = handle;
	impl->lib = filename;
	impl->factory_name = strdup(factory_name);

	return this;

      interface_failed:
	spa_handle_clear(handle);
      init_failed:
	free(handle);
      enum_failed:
      no_symbol:
	dlclose(hnd);
      open_failed:
	free(filename);
	return NULL;
}
