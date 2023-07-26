/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <spa/debug/types.h>
#include <spa/utils/string.h>

#include "pipewire/impl.h"
#include "pipewire/private.h"

PW_LOG_TOPIC_EXTERN(log_factory);
#define PW_LOG_TOPIC_DEFAULT log_factory

#define pw_factory_resource_info(r,...) pw_resource_call(r,struct pw_factory_events,info,0,__VA_ARGS__)

SPA_EXPORT
struct pw_impl_factory *pw_context_create_factory(struct pw_context *context,
				  const char *name,
				  const char *type,
				  uint32_t version,
				  struct pw_properties *properties,
				  size_t user_data_size)
{
	struct pw_impl_factory *this;
	int res;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		return NULL;

	this = calloc(1, sizeof(*this) + user_data_size);
	if (this == NULL) {
		res = -errno;
		goto error_exit;
	};

	this->context = context;
	this->properties = properties;

	this->info.name = strdup(name);
	this->info.type = type;
	this->info.version = version;
	this->info.props = &properties->dict;
	spa_hook_list_init(&this->listener_list);

	if (user_data_size > 0)
		this->user_data = SPA_PTROFF(this, sizeof(*this), void);

	pw_log_debug("%p: new %s", this, name);

	return this;

error_exit:
	pw_properties_free(properties);
	errno = -res;
	return NULL;
}

SPA_EXPORT
void pw_impl_factory_destroy(struct pw_impl_factory *factory)
{
	pw_log_debug("%p: destroy", factory);
	pw_impl_factory_emit_destroy(factory);

	if (factory->registered)
		spa_list_remove(&factory->link);

	if (factory->global) {
		spa_hook_remove(&factory->global_listener);
		pw_global_destroy(factory->global);
	}

	pw_impl_factory_emit_free(factory);
	pw_log_debug("%p: free", factory);

	spa_hook_list_clean(&factory->listener_list);

	free((char *)factory->info.name);

	pw_properties_free(factory->properties);

	free(factory);
}

static int
global_bind(void *object, struct pw_impl_client *client, uint32_t permissions,
		  uint32_t version, uint32_t id)
{
	struct pw_impl_factory *this = object;
	struct pw_global *global = this->global;
	struct pw_resource *resource;

	resource = pw_resource_new(client, id, permissions, global->type, version, 0);
	if (resource == NULL)
		goto error_resource;

	pw_log_debug("%p: bound to %d", this, resource->id);
	pw_global_add_resource(global, resource);

	this->info.change_mask = PW_FACTORY_CHANGE_MASK_ALL;
	pw_factory_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

error_resource:
	pw_log_error("%p: can't create factory resource: %m", this);
	return -errno;
}

static void global_destroy(void *data)
{
	struct pw_impl_factory *factory = data;
	spa_hook_remove(&factory->global_listener);
	factory->global = NULL;
	pw_impl_factory_destroy(factory);
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy,
};

SPA_EXPORT
const struct pw_properties *pw_impl_factory_get_properties(struct pw_impl_factory *factory)
{
	return factory->properties;
}

SPA_EXPORT
int pw_impl_factory_update_properties(struct pw_impl_factory *factory, const struct spa_dict *dict)
{
	struct pw_resource *resource;
	int changed;

	changed = pw_properties_update(factory->properties, dict);
	factory->info.props = &factory->properties->dict;

	pw_log_debug("%p: updated %d properties", factory, changed);

	if (!changed)
		return 0;

	factory->info.change_mask |= PW_FACTORY_CHANGE_MASK_PROPS;
	if (factory->global)
		spa_list_for_each(resource, &factory->global->resource_list, link)
			pw_factory_resource_info(resource, &factory->info);
	factory->info.change_mask = 0;

	return changed;
}

SPA_EXPORT
int pw_impl_factory_register(struct pw_impl_factory *factory,
			 struct pw_properties *properties)
{
	static const char * const keys[] = {
		PW_KEY_OBJECT_SERIAL,
		PW_KEY_MODULE_ID,
		PW_KEY_FACTORY_NAME,
		PW_KEY_FACTORY_TYPE_NAME,
		PW_KEY_FACTORY_TYPE_VERSION,
		NULL
	};

	struct pw_context *context = factory->context;

	if (factory->registered)
		goto error_existed;

        factory->global = pw_global_new(context,
					PW_TYPE_INTERFACE_Factory,
					PW_VERSION_FACTORY,
					PW_FACTORY_PERM_MASK,
					properties,
					global_bind,
					factory);
	if (factory->global == NULL)
		return -errno;

	spa_list_append(&context->factory_list, &factory->link);
	factory->registered = true;

	factory->info.id = factory->global->id;
	pw_properties_setf(factory->properties, PW_KEY_OBJECT_ID, "%d", factory->info.id);
	pw_properties_setf(factory->properties, PW_KEY_OBJECT_SERIAL, "%"PRIu64,
			pw_global_get_serial(factory->global));
	pw_properties_set(factory->properties, PW_KEY_FACTORY_NAME, factory->info.name);
	pw_properties_setf(factory->properties, PW_KEY_FACTORY_TYPE_NAME, "%s", factory->info.type);
	pw_properties_setf(factory->properties, PW_KEY_FACTORY_TYPE_VERSION, "%d", factory->info.version);
	factory->info.props = &factory->properties->dict;

	pw_global_update_keys(factory->global, factory->info.props, keys);

	pw_impl_factory_emit_initialized(factory);

	pw_global_add_listener(factory->global, &factory->global_listener, &global_events, factory);
	pw_global_register(factory->global);

	return 0;

error_existed:
	pw_properties_free(properties);
	return -EEXIST;
}

SPA_EXPORT
void *pw_impl_factory_get_user_data(struct pw_impl_factory *factory)
{
	return factory->user_data;
}

SPA_EXPORT
const struct pw_factory_info *pw_impl_factory_get_info(struct pw_impl_factory *factory)
{
	return &factory->info;
}

SPA_EXPORT
struct pw_global *pw_impl_factory_get_global(struct pw_impl_factory *factory)
{
	return factory->global;
}

SPA_EXPORT
void pw_impl_factory_add_listener(struct pw_impl_factory *factory,
			     struct spa_hook *listener,
			     const struct pw_impl_factory_events *events,
			     void *data)
{
	spa_hook_list_append(&factory->listener_list, listener, events, data);
}

SPA_EXPORT
void pw_impl_factory_set_implementation(struct pw_impl_factory *factory,
				   const struct pw_impl_factory_implementation *implementation,
				   void *data)
{
	factory->impl = SPA_CALLBACKS_INIT(implementation, data);
}

SPA_EXPORT
void *pw_impl_factory_create_object(struct pw_impl_factory *factory,
			       struct pw_resource *resource,
			       const char *type,
			       uint32_t version,
			       struct pw_properties *properties,
			       uint32_t new_id)
{
	void *res = NULL;
	spa_callbacks_call_res(&factory->impl,
			struct pw_impl_factory_implementation,
			res, create_object, 0,
			resource, type, version, properties, new_id);
	return res;
}

/** Find a factory by name
 *
 * \param context the context object
 * \param name the name of the factory to find
 *
 * Find in the list of factories registered in \a context for one with
 * the given \a name.
 *
 * \ingroup pw_context
 */
SPA_EXPORT
struct pw_impl_factory *pw_context_find_factory(struct pw_context *context,
					const char *name)
{
	struct pw_impl_factory *factory;

	spa_list_for_each(factory, &context->factory_list, link) {
		if (spa_streq(factory->info.name, name))
			return factory;
	}
	return NULL;
}
