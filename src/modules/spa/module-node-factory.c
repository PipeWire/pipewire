/* PipeWire
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include "config.h"

#include "pipewire/interfaces.h"
#include "pipewire/core.h"
#include "pipewire/log.h"
#include "pipewire/module.h"

#include "spa-node.h"

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
		pw_resource_error(resource, -EINVAL,
					"needed properties: "
						"spa.library.name=<library-name> "
						"spa.factory.name=<factory-name>");
	}
	return NULL;
      no_mem:
	pw_log_error("can't create node");
	if (resource) {
		pw_resource_error(resource, -ENOMEM, "no memory");
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
	struct node_data *nd;

	spa_hook_remove(&data->module_listener);

	spa_list_consume(nd, &data->node_list, link)
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
	struct pw_type *t = pw_core_get_type(core);
	struct pw_factory *factory;
	struct factory_data *data;

	factory = pw_factory_new(core,
				 "spa-node-factory",
				 t->node,
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

	pw_factory_register(factory, NULL, pw_module_get_global(module), NULL);

	return 0;
}

SPA_EXPORT
int pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
