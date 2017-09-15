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
	struct pw_node_factory *this;
	struct pw_properties *properties;
};

static struct pw_node *create_node(void *_data,
				   struct pw_resource *resource,
				   const char *name,
				   struct pw_properties *properties)
{
	struct factory_data *data = _data;
	struct pw_node *node;
	const char *lib, *factory_name;

	if (properties == NULL)
		goto no_properties;

	lib = pw_properties_get(properties, "spa.library.name");
	factory_name = pw_properties_get(properties, "spa.factory.name");

	if (lib == NULL || factory_name == NULL)
		goto no_properties;

	node = pw_spa_node_load(data->core,
				NULL,
				NULL,
				lib,
				factory_name,
				name,
				properties, 0);
	if (node == NULL)
		goto no_mem;

	return node;

      no_properties:
	pw_log_error("needed properties: spa.library.name=<library-name> spa.factory.name=<factory-name>");
	if (resource) {
		pw_resource_error(resource, SPA_RESULT_INVALID_ARGUMENTS,
					"needed properties: "
						"spa.library.name=<library-name> "
						"spa.factory.name=<factory-name>");
	}
	return NULL;
      no_mem:
	pw_log_error("can't create node");
	if (resource) {
		pw_resource_error(resource, SPA_RESULT_NO_MEMORY, "no memory");
	}
	return NULL;
}

static const struct pw_node_factory_implementation impl_factory = {
	PW_VERSION_NODE_FACRORY_IMPLEMENTATION,
	.create_node = create_node,
};

static bool module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct pw_node_factory *factory;
	struct factory_data *data;

	factory = pw_node_factory_new(core, "spa-node-factory", sizeof(*data));
	if (factory == NULL)
		return false;

	data = pw_node_factory_get_user_data(factory);
	data->this = factory;
	data->core = core;
	data->properties = properties;

	pw_log_debug("module %p: new", module);

	pw_node_factory_set_implementation(factory,
					   &impl_factory,
					   data);

	pw_node_factory_export(factory, NULL, pw_module_get_global(module));

	return true;
}

#if 0
static void module_destroy(struct impl *impl)
{
	pw_log_debug("module %p: destroy", impl);

	free(impl);
}
#endif

bool pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
