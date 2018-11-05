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

#include "pipewire/interfaces.h"
#include "pipewire/core.h"
#include "pipewire/log.h"
#include "pipewire/module.h"

#include "spa-node.h"

static const struct spa_dict_item module_props[] = {
	{ PW_MODULE_PROP_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_MODULE_PROP_DESCRIPTION, "Provide a factory to make SPA nodes" },
	{ PW_MODULE_PROP_VERSION, PACKAGE_VERSION },
};

struct factory_data {
	struct pw_core *core;
	struct pw_factory *this;
	struct pw_properties *properties;

	struct spa_hook factory_listener;
	struct spa_hook module_listener;

	struct spa_list node_list;
};

struct node_data {
	struct spa_list link;
	struct pw_node *node;
	struct spa_hook node_listener;
};

static void node_destroy(void *data)
{
	struct node_data *nd = data;
	spa_list_remove(&nd->link);
	spa_hook_remove(&nd->node_listener);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = node_destroy,
};

static void *create_object(void *_data,
			   struct pw_resource *resource,
			   uint32_t type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	struct factory_data *data = _data;
	struct pw_node *node;
	const char *lib, *factory_name, *name;
	struct node_data *nd;

	if (properties == NULL)
		goto no_properties;

	lib = pw_properties_get(properties, "spa.library.name");
	factory_name = pw_properties_get(properties, "spa.factory.name");
	name = pw_properties_get(properties, "name");

	if (lib == NULL || factory_name == NULL)
		goto no_properties;

	if (name == NULL)
		name = "spa-node";

	node = pw_spa_node_load(data->core,
				NULL,
				pw_factory_get_global(data->this),
				lib,
				factory_name,
				name,
				PW_SPA_NODE_FLAG_ACTIVATE,
				properties,
				sizeof(struct node_data));
	if (node == NULL)
		goto no_mem;

	nd = pw_spa_node_get_user_data(node);
	nd->node = node;
	spa_list_append(&data->node_list, &nd->link);

	pw_node_add_listener(node, &nd->node_listener, &node_events, nd);

	if (resource)
		pw_global_bind(pw_node_get_global(node),
			       pw_resource_get_client(resource),
			       PW_PERM_RWX,
			       version, new_id);

	return node;

      no_properties:
	pw_log_error("needed properties: spa.library.name=<library-name> spa.factory.name=<factory-name>");
	if (resource) {
		pw_resource_error(resource, new_id, -EINVAL,
					"needed properties: "
						"spa.library.name=<library-name> "
						"spa.factory.name=<factory-name>");
	}
	return NULL;
      no_mem:
	pw_log_error("can't create node");
	if (resource) {
		pw_resource_error(resource, new_id, -ENOMEM, "no memory");
	}
	return NULL;
}

static const struct pw_factory_implementation factory_impl = {
	PW_VERSION_FACTORY_IMPLEMENTATION,
	.create_object = create_object,
};

static void factory_destroy(void *_data)
{
	struct factory_data *data = _data;
	struct node_data *nd, *t;

	spa_hook_remove(&data->module_listener);

	spa_list_for_each_safe(nd, t, &data->node_list, link)
		pw_node_destroy(nd->node);

	if (data->properties)
		pw_properties_free(data->properties);
}

static const struct pw_factory_events factory_events = {
	PW_VERSION_FACTORY_IMPLEMENTATION,
	.destroy = factory_destroy,
};

static void module_destroy(void *_data)
{
	struct factory_data *data = _data;
	pw_factory_destroy(data->this);
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
				 "spa-node-factory",
				 PW_TYPE_INTERFACE_Node,
				 PW_VERSION_NODE,
				 NULL,
				 sizeof(*data));
	if (factory == NULL)
		return -ENOMEM;

	data = pw_factory_get_user_data(factory);
	data->this = factory;
	data->core = core;
	data->properties = properties;
	spa_list_init(&data->node_list);

	pw_factory_add_listener(factory, &data->factory_listener, &factory_events, data);
	pw_factory_set_implementation(factory, &factory_impl, data);

	pw_log_debug("module %p: new", module);
	pw_module_add_listener(module, &data->module_listener, &module_events, data);

	pw_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	pw_factory_register(factory, NULL, pw_module_get_global(module), NULL);

	return 0;
}

int pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
