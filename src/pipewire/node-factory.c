/* PipeWire
 * Copyright (C) 2016 Axis Communications AB
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

#include "pipewire/pipewire.h"
#include "pipewire/node-factory.h"
#include "pipewire/private.h"

struct pw_node_factory *pw_node_factory_new(struct pw_core *core,
					    const char *name,
					    size_t user_data_size)
{
	struct pw_node_factory *this;

	this = calloc(1, sizeof(*this) + user_data_size);
	this->core = core;
	this->name = strdup(name);

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(this, sizeof(*this), void);

	return this;
}

void pw_node_factory_export(struct pw_node_factory *factory,
			    struct pw_client *owner,
			    struct pw_global *parent)
{
	struct pw_core *core = factory->core;
	spa_list_insert(core->node_factory_list.prev, &factory->link);
        factory->global = pw_core_add_global(core, owner, parent, core->type.node_factory, 0, NULL, factory);
}

void *pw_node_factory_get_user_data(struct pw_node_factory *factory)
{
	return factory->user_data;
}

void pw_node_factory_set_implementation(struct pw_node_factory *factory,
					const struct pw_node_factory_implementation *implementation,
					void *data)
{
	factory->implementation = implementation;
	factory->implementation_data = data;
}

struct pw_node *pw_node_factory_create_node(struct pw_node_factory *factory,
					    struct pw_resource *resource,
					    const char *name,
					    struct pw_properties *properties)
{
	return factory->implementation->create_node(factory->implementation_data,
						    resource, name, properties);
}
