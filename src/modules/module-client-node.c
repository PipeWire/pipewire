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

struct impl {
	struct pw_node_factory this;
	struct pw_properties *properties;
};

static struct pw_node *create_node(struct pw_node_factory *factory,
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
	pw_core_resource_error(resource->client->core_resource,
			       resource->client->core_resource->id, SPA_RESULT_NO_MEMORY, "no memory");
	if (properties)
		pw_properties_free(properties);
	return NULL;
}

static bool module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = module->core;
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return false;

	pw_log_debug("module %p: new", impl);

	impl->properties = properties;

	impl->this.core = core;
	impl->this.name = "client-node";

        pw_signal_init(&impl->this.destroy_signal);
	impl->this.create_node = create_node;

	pw_protocol_native_ext_client_node_init(core);

	spa_list_insert(core->node_factory_list.prev, &impl->this.link);

        impl->this.global = pw_core_add_global(core, NULL, module->global,
					       core->type.node_factory, 0,
					       NULL, impl);

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
