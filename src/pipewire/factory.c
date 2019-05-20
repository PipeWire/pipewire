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

#include <errno.h>

#include <spa/debug/types.h>

#include "pipewire/factory.h"
#include "pipewire/private.h"
#include "pipewire/type.h"
#include "pipewire/interfaces.h"

#define pw_factory_resource_info(r,...) pw_resource_notify(r,struct pw_factory_proxy_events,info,0,__VA_ARGS__)

struct resource_data {
	struct spa_hook resource_listener;
};

SPA_EXPORT
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

SPA_EXPORT
void pw_factory_destroy(struct pw_factory *factory)
{
	pw_log_debug("factory %p: destroy", factory);
	pw_factory_emit_destroy(factory);

	if (factory->registered)
		spa_list_remove(&factory->link);

	if (factory->global) {
		spa_hook_remove(&factory->global_listener);
		pw_global_destroy(factory->global);
	}
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
global_bind(void *_data, struct pw_client *client, uint32_t permissions,
		  uint32_t version, uint32_t id)
{
	struct pw_factory *this = _data;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	pw_resource_add_listener(resource, &data->resource_listener, &resource_events, resource);

	pw_log_debug("factory %p: bound to %d", this, resource->id);

	spa_list_append(&global->resource_list, &resource->link);

	this->info.change_mask = ~0;
	pw_factory_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

      no_mem:
	pw_log_error("can't create factory resource");
	return -ENOMEM;
}

static void global_destroy(void *object)
{
	struct pw_factory *factory = object;
	spa_hook_remove(&factory->global_listener);
	factory->global = NULL;
	pw_factory_destroy(factory);
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy,
};

SPA_EXPORT
int pw_factory_register(struct pw_factory *factory,
			 struct pw_client *owner,
			 struct pw_global *parent,
			 struct pw_properties *properties)
{
	struct pw_core *core = factory->core;

	if (factory->registered)
		return -EEXIST;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		return -ENOMEM;

	pw_properties_set(properties, "factory.name", factory->info.name);
	pw_properties_setf(properties, "factory.type.name", "%s",
			spa_debug_type_find_name(pw_type_info(), factory->info.type));
	pw_properties_setf(properties, "factory.type.version", "%d", factory->info.version);

	spa_list_append(&core->factory_list, &factory->link);
	factory->registered = true;

        factory->global = pw_global_new(core,
					PW_TYPE_INTERFACE_Factory,
					PW_VERSION_FACTORY_PROXY,
					properties,
					global_bind,
					factory);
	if (factory->global == NULL)
		return -ENOMEM;


	pw_global_add_listener(factory->global, &factory->global_listener, &global_events, factory);
	pw_global_register(factory->global, owner, parent);
	factory->info.id = factory->global->id;

	return 0;
}

SPA_EXPORT
void *pw_factory_get_user_data(struct pw_factory *factory)
{
	return factory->user_data;
}

SPA_EXPORT
struct pw_global *pw_factory_get_global(struct pw_factory *factory)
{
	return factory->global;
}

SPA_EXPORT
void pw_factory_add_listener(struct pw_factory *factory,
			     struct spa_hook *listener,
			     const struct pw_factory_events *events,
			     void *data)
{
	spa_hook_list_append(&factory->listener_list, listener, events, data);
}

SPA_EXPORT
void pw_factory_set_implementation(struct pw_factory *factory,
				   const struct pw_factory_implementation *implementation,
				   void *data)
{
	factory->impl = SPA_CALLBACKS_INIT(implementation, data);
}

SPA_EXPORT
void *pw_factory_create_object(struct pw_factory *factory,
			       struct pw_resource *resource,
			       uint32_t type,
			       uint32_t version,
			       struct pw_properties *properties,
			       uint32_t new_id)
{
	void *res = NULL;
	spa_callbacks_call_res(&factory->impl,
			struct pw_factory_implementation,
			res, create_object, 0,
			resource, type, version, properties, new_id);
	return res;
}
