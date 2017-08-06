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
#include "pipewire/module.h"

#include "module-client-node/client-node.h"

struct pw_protocol *pw_protocol_native_ext_client_node_init(struct pw_core *core);

struct factory_data {
	struct pw_node_factory *this;
	struct pw_properties *properties;
};

static struct pw_node *create_node(void *_data,
				   struct pw_resource *resource,
				   const char *name,
				   struct pw_properties *properties)
{
	struct pw_client_node *node;

	node = pw_client_node_new(resource, name, properties);
	if (node == NULL)
		goto no_mem;

	return node->node;

      no_mem:
	pw_log_error("can't create node");
	pw_resource_error(resource, SPA_RESULT_NO_MEMORY, "no memory");
	if (properties)
		pw_properties_free(properties);
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

	factory = pw_node_factory_new(core, "client-node", sizeof(*data));
	if (factory == NULL)
		return false;

	data = pw_node_factory_get_user_data(factory);
	data->this = factory;
	data->properties = properties;

	pw_log_debug("module %p: new", module);

	pw_node_factory_set_implementation(factory,
					   &impl_factory,
					   data);

	pw_protocol_native_ext_client_node_init(core);

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
