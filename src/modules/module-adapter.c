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

#include <spa/node/node.h>
#include <spa/utils/hook.h>

#include <pipewire/pipewire.h>
#include "pipewire/private.h"

#include "modules/spa/spa-node.h"
#include "module-adapter/adapter.h"

#define FACTORY_USAGE	SPA_KEY_FACTORY_NAME"=<factory-name> " \
			"["SPA_KEY_LIBRARY_NAME"=<library-name>] " \
			ADAPTER_USAGE

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Manage adapter nodes" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct factory_data {
	struct pw_factory *this;

	struct spa_list node_list;

	struct pw_core *core;
	struct pw_module *module;
	struct spa_hook module_listener;
};

struct node_data {
	struct factory_data *data;
	struct spa_list link;
	struct pw_node *adapter;
	struct spa_hook adapter_listener;
	struct spa_hook resource_listener;
};

static void resource_destroy(void *data)
{
	struct node_data *nd = data;
	spa_hook_remove(&nd->resource_listener);
	if (nd->adapter)
		pw_node_destroy(nd->adapter);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = resource_destroy
};

static void node_destroy(void *data)
{
	struct node_data *nd = data;
	spa_list_remove(&nd->link);
	nd->adapter = NULL;
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = node_destroy
};

static void *create_object(void *_data,
			   struct pw_resource *resource,
			   uint32_t type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	struct factory_data *d = _data;
	struct pw_client *client;
	struct pw_node *adapter, *slave;
	const char *factory_name;
	int res;
	struct node_data *nd;

	if (properties == NULL)
		goto error_properties;

	factory_name = pw_properties_get(properties, SPA_KEY_FACTORY_NAME);
	if (factory_name == NULL)
		goto error_properties;

	slave = pw_spa_node_load(d->core,
				NULL,
				pw_factory_get_global(d->this),
				factory_name,
				"slave-node",
				PW_SPA_NODE_FLAG_ACTIVATE |
				PW_SPA_NODE_FLAG_NO_REGISTER,
				pw_properties_copy(properties), 0);
	if (slave == NULL)
		goto error_no_mem;

	adapter = pw_adapter_new(pw_module_get_core(d->module),
			slave,
			properties,
			sizeof(struct node_data));
	properties = NULL;

	if (adapter == NULL) {
		if (errno == ENOMEM)
			goto error_no_mem;
		else
			goto error_usage;
	}

	nd = pw_adapter_get_user_data(adapter);
	nd->data = d;
	nd->adapter = adapter;
	spa_list_append(&d->node_list, &nd->link);

	pw_node_add_listener(adapter, &nd->adapter_listener, &node_events, nd);

	client = resource ? pw_resource_get_client(resource): NULL;

	pw_node_register(adapter, client, pw_module_get_global(d->module), NULL);

	if (client) {
		struct pw_resource *bound_resource;

		res = pw_global_bind(pw_node_get_global(adapter), client,
				PW_PERM_RWX, PW_VERSION_NODE_PROXY, new_id);
		if (res < 0)
			goto error_bind;

		if ((bound_resource = pw_client_find_resource(client, new_id)) == NULL)
			goto error_bind;

		pw_resource_add_listener(bound_resource, &nd->resource_listener, &resource_events, nd);
	}

	pw_node_set_active(adapter, true);

	return adapter;

error_properties:
	res = -EINVAL;
	pw_log_error("factory %p: usage: " FACTORY_USAGE, d->this);
	if (resource)
		pw_resource_error(resource, res, "usage: " FACTORY_USAGE);
	goto error_cleanup;
error_no_mem:
	res = -errno;
	pw_log_error("can't create node: %m");
	if (resource)
		pw_resource_error(resource, res, "can't create node: %s", spa_strerror(res));
	goto error_cleanup;
error_usage:
	res = -EINVAL;
	pw_log_error("usage: "ADAPTER_USAGE);
	if (resource)
		pw_resource_error(resource, res, "usage: "ADAPTER_USAGE);
	goto error_cleanup;
error_bind:
	if (resource)
		pw_resource_error(resource, res, "can't bind adapter node");
	goto error_cleanup;
error_cleanup:
	if (properties)
		pw_properties_free(properties);
	errno = -res;
	return NULL;
}

static const struct pw_factory_implementation impl_factory = {
	PW_VERSION_FACTORY_IMPLEMENTATION,
	.create_object = create_object,
};

static void module_destroy(void *data)
{
	struct factory_data *d = data;
	struct node_data *nd, *t;

	spa_hook_remove(&d->module_listener);

	spa_list_for_each_safe(nd, t, &d->node_list, link)
		pw_node_destroy(nd->adapter);

	pw_factory_destroy(d->this);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
};

static int module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct pw_factory *factory;
	struct factory_data *data;

	factory = pw_factory_new(core,
				 "adapter",
				 PW_TYPE_INTERFACE_Node,
				 PW_VERSION_NODE_PROXY,
				 pw_properties_new(
					 PW_KEY_FACTORY_USAGE, FACTORY_USAGE,
					 NULL),
				 sizeof(*data));
	if (factory == NULL)
		return -errno;

	data = pw_factory_get_user_data(factory);
	data->this = factory;
	data->core = core;
	data->module = module;
	spa_list_init(&data->node_list);

	pw_log_debug("module %p: new", module);

	pw_factory_set_implementation(factory,
				      &impl_factory,
				      data);

	pw_factory_register(factory, NULL, pw_module_get_global(module), NULL);

	pw_module_add_listener(module, &data->module_listener, &module_events, data);

	pw_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
}

SPA_EXPORT
int pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
