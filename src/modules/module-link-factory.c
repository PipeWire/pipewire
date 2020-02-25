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
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include "config.h"

#include <spa/utils/result.h>

#include <pipewire/impl.h>

#define NAME "link-factory"

#define FACTORY_USAGE	PW_KEY_LINK_OUTPUT_NODE"=<output-node> "	\
			"["PW_KEY_LINK_OUTPUT_PORT"=<output-port>] "	\
			PW_KEY_LINK_INPUT_NODE"=<input-node "		\
			"["PW_KEY_LINK_INPUT_PORT"=<input-port>] "	\
			"["PW_KEY_OBJECT_LINGER"=<bool>] "		\
			"["PW_KEY_LINK_PASSIVE"=<bool>]"

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Allow clients to create links" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct factory_data {
	struct pw_impl_module *module;
	struct pw_impl_factory *this;

	struct spa_list link_list;

	struct spa_hook module_listener;
};

struct link_data {
	struct factory_data *data;
	struct spa_list l;
	struct pw_impl_link *link;
	struct spa_hook link_listener;

	struct pw_resource *resource;
	struct spa_hook resource_listener;

	struct pw_global *global;
	struct spa_hook global_listener;

	struct pw_resource *factory_resource;
	uint32_t new_id;
	bool linger;
};

static void resource_destroy(void *data)
{
	struct link_data *ld = data;
	spa_hook_remove(&ld->resource_listener);
	ld->resource = NULL;
	if (ld->global)
		pw_global_destroy(ld->global);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = resource_destroy
};

static void global_destroy(void *data)
{
	struct link_data *ld = data;
	spa_hook_remove(&ld->global_listener);
	ld->global = NULL;
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy
};

static void link_destroy(void *data)
{
	struct link_data *ld = data;
	spa_list_remove(&ld->l);
	if (ld->global)
		spa_hook_remove(&ld->global_listener);
	if (ld->resource)
		spa_hook_remove(&ld->resource_listener);
}

static void link_initialized(void *data)
{
	struct link_data *ld = data;
	struct pw_impl_client *client = pw_resource_get_client(ld->factory_resource);
	int res;

	ld->global = pw_impl_link_get_global(ld->link);
	pw_global_add_listener(ld->global, &ld->global_listener, &global_events, ld);

	res = pw_global_bind(ld->global, client, PW_PERM_RWX, PW_VERSION_LINK, ld->new_id);
	if (res < 0)
		goto error_bind;

	if (!ld->linger) {
		ld->resource = pw_impl_client_find_resource(client, ld->new_id);
		if (ld->resource == NULL) {
			res = -ENOENT;
			goto error_bind;
		}
		pw_resource_add_listener(ld->resource, &ld->resource_listener, &resource_events, ld);
	}
	return;

error_bind:
	pw_resource_errorf_id(ld->factory_resource, ld->new_id, res,
			"can't bind link: %s", spa_strerror(res));
}

static const struct pw_impl_link_events link_events = {
	PW_VERSION_IMPL_LINK_EVENTS,
	.destroy = link_destroy,
	.initialized = link_initialized
};

static struct pw_impl_port *get_port(struct pw_impl_node *node, enum spa_direction direction)
{
	struct pw_impl_port *p;
	struct pw_context *context = pw_impl_node_get_context(node);
	int res;

	p = pw_impl_node_find_port(node, direction, PW_ID_ANY);

	if (p == NULL || pw_impl_port_is_linked(p)) {
		uint32_t port_id;

		port_id = pw_impl_node_get_free_port_id(node, direction);
		if (port_id == SPA_ID_INVALID)
			return NULL;

		p = pw_context_create_port(context, direction, port_id, NULL, 0);
		if (p == NULL)
			return NULL;

		if ((res = pw_impl_port_add(p, node)) < 0) {
			pw_log_warn("can't add port: %s", spa_strerror(res));
			errno = -res;
			return NULL;
		}
	}
	return p;
}


static void *create_object(void *_data,
			   struct pw_resource *resource,
			   const char *type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	struct factory_data *d = _data;
	struct pw_impl_client *client = NULL;
	struct pw_impl_node *output_node, *input_node;
	struct pw_impl_port *outport, *inport;
	struct pw_context *context;
	struct pw_global *global;
	struct pw_impl_link *link;
	uint32_t output_node_id, input_node_id;
	uint32_t output_port_id, input_port_id;
	struct link_data *ld;
	const char *str;
	int res;
	bool linger;

	client = pw_resource_get_client(resource);
	context = pw_impl_client_get_context(client);

	if (properties == NULL)
		goto error_properties;

	if ((str = pw_properties_get(properties, PW_KEY_LINK_OUTPUT_NODE)) == NULL)
		goto error_properties;

	output_node_id = pw_properties_parse_int(str);

	if ((str = pw_properties_get(properties, PW_KEY_LINK_INPUT_NODE)) == NULL)
		goto error_properties;

	input_node_id = pw_properties_parse_int(str);

	str = pw_properties_get(properties, PW_KEY_LINK_OUTPUT_PORT);
	output_port_id = str ? pw_properties_parse_int(str) : -1;

	str = pw_properties_get(properties, PW_KEY_LINK_INPUT_PORT);
	input_port_id = str ? pw_properties_parse_int(str) : -1;

	global = pw_context_find_global(context, output_node_id);
	if (global == NULL || !pw_global_is_type(global, PW_TYPE_INTERFACE_Node))
		goto error_output;

	output_node = pw_global_get_object(global);

	global = pw_context_find_global(context, input_node_id);
	if (global == NULL || !pw_global_is_type(global, PW_TYPE_INTERFACE_Node))
		goto error_input;

	input_node = pw_global_get_object(global);

	if (output_port_id == PW_ID_ANY) {
		outport = get_port(output_node, SPA_DIRECTION_OUTPUT);
	}
	else {
		global = pw_context_find_global(context, output_port_id);
		if (global == NULL || !pw_global_is_type(global, PW_TYPE_INTERFACE_Port))
			goto error_output_port;

		outport = pw_global_get_object(global);
	}
	if (outport == NULL)
		goto error_output_port;

	if (input_port_id == PW_ID_ANY)
		inport = get_port(input_node, SPA_DIRECTION_INPUT);
	else {
		global = pw_context_find_global(context, input_port_id);
		if (global == NULL || !pw_global_is_type(global, PW_TYPE_INTERFACE_Port))
			goto error_input_port;

		inport = pw_global_get_object(global);
	}
	if (inport == NULL)
		goto error_input_port;

	str = pw_properties_get(properties, PW_KEY_OBJECT_LINGER);
	linger = str ? pw_properties_parse_bool(str) : false;

	pw_properties_setf(properties, PW_KEY_FACTORY_ID, "%d",
			pw_impl_factory_get_info(d->this)->id);
	if (!linger)
		pw_properties_setf(properties, PW_KEY_CLIENT_ID, "%d",
				pw_impl_client_get_info(client)->id);


	link = pw_context_create_link(context, outport, inport, NULL, properties, sizeof(struct link_data));
	properties = NULL;
	if (link == NULL) {
		res = -errno;
		goto error_create_link;
	}

	ld = pw_impl_link_get_user_data(link);
	ld->data = d;
	ld->factory_resource = resource;
	ld->link = link;
	ld->new_id = new_id;
	ld->linger = linger;
	spa_list_append(&d->link_list, &ld->l);

	pw_impl_link_add_listener(link, &ld->link_listener, &link_events, ld);
	if ((res = pw_impl_link_register(link, NULL)) < 0)
		goto error_link_register;

	return link;

error_properties:
	res = -EINVAL;
	pw_resource_errorf_id(resource, new_id, res, NAME": no properties. usage:"FACTORY_USAGE);
	goto error_exit;
error_output:
	res = -EINVAL;
	pw_resource_errorf_id(resource, new_id, res, NAME": unknown output node %u", output_node_id);
	goto error_exit;
error_input:
	res = -EINVAL;
	pw_resource_errorf_id(resource, new_id, res, NAME": unknown input node %u", input_node_id);
	goto error_exit;
error_output_port:
	res = -EINVAL;
	pw_resource_errorf_id(resource, new_id, res, NAME": unknown output port %u", output_port_id);
	goto error_exit;
error_input_port:
	res = -EINVAL;
	pw_resource_errorf_id(resource, new_id, res, NAME": unknown input port %u", input_port_id);
	goto error_exit;
error_create_link:
	pw_resource_errorf_id(resource, new_id, res, NAME": can't link ports %d and %d: %s",
			pw_impl_port_get_info(outport)->id, pw_impl_port_get_info(inport)->id,
			spa_strerror(res));
	goto error_exit;
error_link_register:
	pw_resource_errorf_id(resource, new_id, res, NAME": can't register link: %s", spa_strerror(res));
	goto error_exit;
error_exit:
	if (properties)
		pw_properties_free(properties);
	errno = -res;
	return NULL;
}

static const struct pw_impl_factory_implementation impl_factory = {
	PW_VERSION_IMPL_FACTORY_IMPLEMENTATION,
	.create_object = create_object,
};

static void module_destroy(void *data)
{
	struct factory_data *d = data;
	struct link_data *ld, *t;

	spa_hook_remove(&d->module_listener);

	spa_list_for_each_safe(ld, t, &d->link_list, l)
		pw_impl_link_destroy(ld->link);

	pw_impl_factory_destroy(d->this);
}

static void module_registered(void *data)
{
	struct factory_data *d = data;
	struct pw_impl_module *module = d->module;
	struct pw_impl_factory *factory = d->this;
	struct spa_dict_item items[1];
	char id[16];
	int res;

	snprintf(id, sizeof(id), "%d", pw_global_get_id(pw_impl_module_get_global(module)));
	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_MODULE_ID, id);
	pw_impl_factory_update_properties(factory, &SPA_DICT_INIT(items, 1));

	if ((res = pw_impl_factory_register(factory, NULL)) < 0) {
		pw_log_error(NAME" %p: can't register factory: %s", factory, spa_strerror(res));
	}
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
	.registered = module_registered,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_impl_factory *factory;
	struct factory_data *data;

	factory = pw_context_create_factory(context,
				 "link-factory",
				 PW_TYPE_INTERFACE_Link,
				 PW_VERSION_LINK,
				 pw_properties_new(
					 PW_KEY_FACTORY_USAGE, FACTORY_USAGE,
					 NULL),
				 sizeof(*data));
	if (factory == NULL)
		return -errno;

	data = pw_impl_factory_get_user_data(factory);
	data->this = factory;
	data->module = module;
	spa_list_init(&data->link_list);

	pw_log_debug("module %p: new", module);

	pw_impl_factory_set_implementation(factory,
				      &impl_factory,
				      data);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	pw_impl_module_add_listener(module, &data->module_listener, &module_events, data);

	return 0;
}
