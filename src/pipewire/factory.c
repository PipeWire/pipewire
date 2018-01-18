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

#include <errno.h>

#include "pipewire/pipewire.h"
#include "pipewire/factory.h"
#include "pipewire/private.h"

struct resource_data {
	struct spa_hook resource_listener;
};

struct pw_factory *pw_factory_new(struct pw_core *core,
				  const char *name,
				  uint32_t type,
				  uint32_t version,
				  struct pw_properties *properties,
				  size_t user_data_size)
{
	struct pw_factory *this;

	this = calloc(1, sizeof(*this) + user_data_size);
	this->core = core;
	this->properties = properties;
	spa_list_init(&this->resource_list);

	this->info.name = strdup(name);
	this->info.type = type;
	this->info.version = version;
	this->info.props = properties ? &properties->dict : NULL;
	spa_hook_list_init(&this->listener_list);

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(this, sizeof(*this), void);

	pw_log_debug("factory %p: new %s", this, name);

	return this;
}

void pw_factory_destroy(struct pw_factory *factory)
{
	pw_log_debug("factory %p: destroy", factory);
	spa_hook_list_call(&factory->listener_list, struct pw_factory_events, destroy);

	if (factory->global) {
		spa_list_remove(&factory->link);
		pw_global_destroy(factory->global);
	}
	if (factory->info.name)
		free((char *)factory->info.name);
	if (factory->properties)
		pw_properties_free(factory->properties);

	free(factory);
}

static void factory_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	spa_list_remove(&resource->link);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = factory_unbind_func,
};

static int
factory_bind_func(struct pw_global *global,
		  struct pw_client *client, uint32_t permissions,
		  uint32_t version, uint32_t id)
{
	struct pw_factory *this = global->object;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	pw_resource_add_listener(resource, &data->resource_listener, &resource_events, resource);

	pw_log_debug("factory %p: bound to %d", this, resource->id);

	spa_list_append(&this->resource_list, &resource->link);

	this->info.change_mask = ~0;
	pw_factory_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

      no_mem:
	pw_log_error("can't create factory resource");
	pw_core_resource_error(client->core_resource,
			       client->core_resource->id, -ENOMEM, "no memory");
	return -ENOMEM;
}

void pw_factory_register(struct pw_factory *factory,
			 struct pw_client *owner,
			 struct pw_global *parent)
{
	struct pw_core *core = factory->core;
	spa_list_append(&core->factory_list, &factory->link);
        factory->global = pw_global_new(core,
				     core->type.factory, 0, factory_bind_func, factory);
	if (factory->global != NULL) {
		pw_global_register(factory->global, owner, parent);
		factory->info.id = factory->global->id;
	}
}

void *pw_factory_get_user_data(struct pw_factory *factory)
{
	return factory->user_data;
}

struct pw_global *pw_factory_get_global(struct pw_factory *factory)
{
	return factory->global;
}

void pw_factory_add_listener(struct pw_factory *factory,
			     struct spa_hook *listener,
			     const struct pw_factory_events *events,
			     void *data)
{
	spa_hook_list_append(&factory->listener_list, listener, events, data);
}

void pw_factory_set_implementation(struct pw_factory *factory,
				   const struct pw_factory_implementation *implementation,
				   void *data)
{
	factory->implementation = implementation;
	factory->implementation_data = data;
}

void *pw_factory_create_object(struct pw_factory *factory,
			       struct pw_resource *resource,
			       uint32_t type,
			       uint32_t version,
			       struct pw_properties *properties,
			       uint32_t new_id)
{
	return factory->implementation->create_object(factory->implementation_data,
						      resource, type, version, properties, new_id);
}
