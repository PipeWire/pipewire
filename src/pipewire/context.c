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
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <regex.h>

#include <pipewire/log.h>

#include <spa/support/cpu.h>
#include <spa/support/dbus.h>
#include <spa/node/utils.h>
#include <spa/utils/names.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>

#include <pipewire/pipewire.h>
#include <pipewire/private.h>
#include <pipewire/interfaces.h>
#include <pipewire/protocol.h>
#include <pipewire/context.h>
#include <pipewire/data-loop.h>
#include <pipewire/device.h>
#include <pipewire/map.h>
#include <pipewire/type.h>
#include <pipewire/module.h>
#include <pipewire/version.h>

#define NAME "context"

/** \cond */
struct impl {
	struct pw_context this;
	struct spa_handle *dbus_handle;
};


struct resource_data {
	struct spa_hook resource_listener;
	struct spa_hook object_listener;
};

struct factory_entry {
	regex_t regex;
	char *lib;
};

/** \endcond */
static void * registry_bind(void *object, uint32_t id,
		uint32_t type, uint32_t version, size_t user_data_size)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_context *context = resource->context;
	struct pw_global *global;
	uint32_t permissions, new_id = user_data_size;

	if ((global = pw_context_find_global(context, id)) == NULL)
		goto error_no_id;

	permissions = pw_global_get_permissions(global, client);

	if (!PW_PERM_IS_R(permissions))
		goto error_no_id;

	if (global->type != type)
		goto error_wrong_interface;

	pw_log_debug("global %p: bind global id %d, iface %s/%d to %d", global, id,
		     spa_debug_type_find_name(pw_type_info(), type), version, new_id);

	if (pw_global_bind(global, client, permissions, version, new_id) < 0)
		goto error_exit_clean;

	return NULL;

error_no_id:
	pw_log_debug("registry %p: no global with id %u to bind to %u", resource, id, new_id);
	pw_resource_errorf(resource, -ENOENT, "no such global %u", id);
	goto error_exit_clean;
error_wrong_interface:
	pw_log_debug("registry %p: global with id %u has no interface %u", resource, id, type);
	pw_resource_errorf(resource, -ENOENT, "no such interface %u", type);
	goto error_exit_clean;
error_exit_clean:
	/* unmark the new_id the map, the client does not yet know about the failed
	 * bind and will choose the next id, which we would refuse when we don't mark
	 * new_id as 'used and freed' */
	pw_map_insert_at(&client->objects, new_id, NULL);
	pw_core_resource_remove_id(client->core_resource, new_id);
	return NULL;
}

static int registry_destroy(void *object, uint32_t id)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_context *context = resource->context;
	struct pw_global *global;
	uint32_t permissions;
	int res;

	if ((global = pw_context_find_global(context, id)) == NULL)
		goto error_no_id;

	permissions = pw_global_get_permissions(global, client);

	if (!PW_PERM_IS_R(permissions))
		goto error_no_id;

	if (!PW_PERM_IS_X(permissions))
		goto error_not_allowed;

	pw_log_debug("global %p: destroy global id %d", global, id);

	pw_global_destroy(global);
	return 0;

error_no_id:
	pw_log_debug("registry %p: no global with id %u to destroy", resource, id);
	res = -ENOENT;
	goto error_exit;
error_not_allowed:
	pw_log_debug("registry %p: destroy of id %u not allowed", resource, id);
	res = -EPERM;
	goto error_exit;
error_exit:
	return res;
}

static const struct pw_registry_methods registry_methods = {
	PW_VERSION_REGISTRY_METHODS,
	.bind = registry_bind,
	.destroy = registry_destroy
};

static void destroy_registry_resource(void *object)
{
	struct pw_resource *resource = object;
	spa_list_remove(&resource->link);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = destroy_registry_resource
};

static int destroy_resource(void *object, void *data)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;

	if (resource &&
	    resource != client->core_resource) {
		pw_resource_remove(resource);
	}
	return 0;
}

static int core_hello(void *object, uint32_t version)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_context *this = resource->context;
	int res;

	pw_log_debug(NAME" %p: hello %d from resource %p", this, version, resource);
	pw_map_for_each(&client->objects, destroy_resource, client);

	this->info.change_mask = PW_CORE_CHANGE_MASK_ALL;
	pw_core_resource_info(resource, &this->info);

	if (version >= 3) {
		if ((res = pw_global_bind(client->global, client,
				PW_PERM_RWX, PW_VERSION_CLIENT_PROXY, 1)) < 0)
			return res;
	}
	return 0;
}

static int core_sync(void *object, uint32_t id, int seq)
{
	struct pw_resource *resource = object;
	pw_log_trace(NAME" %p: sync %d for resource %d", resource->context, seq, id);
	pw_core_resource_done(resource, id, seq);
	return 0;
}

static int core_pong(void *object, uint32_t id, int seq)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_resource *r;

	pw_log_debug(NAME" %p: pong %d for resource %d", resource->context, seq, id);

	if ((r = pw_client_find_resource(client, id)) == NULL)
		return -EINVAL;

	pw_resource_emit_pong(r, seq);
	return 0;
}

static int core_error(void *object, uint32_t id, int seq, int res, const char *message)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_resource *r;

	pw_log_debug(NAME" %p: error %d for resource %d: %s", resource->context, res, id, message);

	if ((r = pw_client_find_resource(client, id)) == NULL)
		return -EINVAL;

	pw_resource_emit_error(r, seq, res, message);
	return 0;
}

static struct pw_registry * core_get_registry(void *object, uint32_t version, size_t user_data_size)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_context *this = resource->context;
	struct pw_global *global;
	struct pw_resource *registry_resource;
	struct resource_data *data;
	uint32_t new_id = user_data_size;
	int res;

	registry_resource = pw_resource_new(client,
					    new_id,
					    PW_PERM_RWX,
					    PW_TYPE_INTERFACE_Registry,
					    version,
					    sizeof(*data));
	if (registry_resource == NULL) {
		res = -errno;
		goto error_resource;
	}

	data = pw_resource_get_user_data(registry_resource);
	pw_resource_add_listener(registry_resource,
				&data->resource_listener,
				&resource_events,
				registry_resource);
	pw_resource_add_object_listener(registry_resource,
				&data->object_listener,
				&registry_methods,
				registry_resource);

	spa_list_append(&this->registry_resource_list, &registry_resource->link);

	spa_list_for_each(global, &this->global_list, link) {
		uint32_t permissions = pw_global_get_permissions(global, client);
		if (PW_PERM_IS_R(permissions)) {
			pw_registry_resource_global(registry_resource,
						    global->id,
						    permissions,
						    global->type,
						    global->version,
						    &global->properties->dict);
		}
	}

	return (struct pw_registry *)registry_resource;

error_resource:
	pw_log_error(NAME" %p: can't create registry resource: %m", this);
	pw_core_resource_errorf(client->core_resource, new_id,
			client->recv_seq, res,
			"can't create registry resource: %s", spa_strerror(res));
	pw_map_insert_at(&client->objects, new_id, NULL);
	pw_core_resource_remove_id(client->core_resource, new_id);
	errno = -res;
	return NULL;
}

static void *
core_create_object(void *object,
		   const char *factory_name,
		   uint32_t type,
		   uint32_t version,
		   const struct spa_dict *props,
		   size_t user_data_size)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_factory *factory;
	void *obj;
	struct pw_properties *properties;
	struct pw_context *this = client->context;
	uint32_t new_id = user_data_size;
	int res;

	factory = pw_context_find_factory(this, factory_name);
	if (factory == NULL || factory->global == NULL)
		goto error_no_factory;

	if (!PW_PERM_IS_R(pw_global_get_permissions(factory->global, client)))
		goto error_no_factory;

	if (factory->info.type != type)
		goto error_type;

	if (factory->info.version < version)
		goto error_version;

	if (props) {
		properties = pw_properties_new_dict(props);
		if (properties == NULL)
			goto error_properties;
	} else
		properties = NULL;

	/* error will be posted */
	obj = pw_factory_create_object(factory, resource, type, version, properties, new_id);
	if (obj == NULL)
		goto error_create_failed;

	return 0;

error_no_factory:
	res = -ENOENT;
	pw_log_error(NAME" %p: can't find factory '%s'", this, factory_name);
	pw_resource_errorf(resource, res, "unknown factory name %s", factory_name);
	goto error_exit;
error_version:
error_type:
	res = -EPROTO;
	pw_log_error(NAME" %p: invalid resource type/version", this);
	pw_resource_errorf(resource, res, "wrong resource type/version");
	goto error_exit;
error_properties:
	res = -errno;
	pw_log_error(NAME" %p: can't create properties: %m", this);
	pw_resource_errorf(resource, res, "can't create properties: %s", spa_strerror(res));
	goto error_exit;
error_create_failed:
	res = -errno;
	goto error_exit;
error_exit:
	pw_map_insert_at(&client->objects, new_id, NULL);
	pw_core_resource_remove_id(client->core_resource, new_id);
	errno = -res;
	return NULL;
}

static int core_destroy(void *object, void *proxy)
{
	struct pw_resource *resource = object;
	struct pw_client *client = resource->client;
	struct pw_resource *r = proxy;
	pw_log_debug(NAME" %p: destroy resource %p from client %p", resource->context, r, client);
	pw_resource_destroy(r);
	return 0;
}

static const struct pw_core_methods core_methods = {
	PW_VERSION_CORE_METHODS,
	.hello = core_hello,
	.sync = core_sync,
	.pong = core_pong,
	.error = core_error,
	.get_registry = core_get_registry,
	.create_object = core_create_object,
	.destroy = core_destroy,
};

static void core_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	if (resource->id == 0)
		resource->client->core_resource = NULL;
	spa_list_remove(&resource->link);
}

static const struct pw_resource_events core_resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = core_unbind_func,
};

static int
global_bind(void *_data,
	    struct pw_client *client,
	    uint32_t permissions,
	    uint32_t version,
	    uint32_t id)
{
	struct pw_context *this = _data;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;
	int res;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL) {
		res = -errno;
		goto error;
	}

	data = pw_resource_get_user_data(resource);

	pw_resource_add_listener(resource,
			&data->resource_listener,
			&core_resource_events,
			resource);
	pw_resource_add_object_listener(resource,
			&data->object_listener,
			&core_methods, resource);

	spa_list_append(&global->resource_list, &resource->link);
	pw_resource_set_bound_id(resource, global->id);

	if (resource->id == 0)
		client->core_resource = resource;
	else
		pw_core_resource_info(resource, &this->info);

	pw_log_debug(NAME" %p: bound to %d", this, resource->id);

	return 0;

error:
	pw_log_error(NAME" %p: can't create resource: %m", this);
	return res;
}

static void global_destroy(void *object)
{
	struct pw_context *context = object;
	spa_hook_remove(&context->global_listener);
	context->global = NULL;
	pw_context_destroy(context);
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy,
};

static int load_module_profile(struct pw_context *this, const char *profile)
{
	pw_log_debug(NAME" %p: module profile %s", this, profile);
	if (strcmp(profile, "default") == 0) {
		pw_module_load(this, "libpipewire-module-rtkit", NULL, NULL);
		pw_module_load(this, "libpipewire-module-protocol-native", NULL, NULL);
		pw_module_load(this, "libpipewire-module-client-node", NULL, NULL);
		pw_module_load(this, "libpipewire-module-client-device", NULL, NULL);
		pw_module_load(this, "libpipewire-module-adapter", NULL, NULL);
		pw_module_load(this, "libpipewire-module-metadata", NULL, NULL);
		pw_module_load(this, "libpipewire-module-session-manager", NULL, NULL);
	}
	return 0;
}

/** Create a new context object
 *
 * \param main_loop the main loop to use
 * \param properties extra properties for the context, ownership it taken
 * \return a newly allocated context object
 *
 * \memberof pw_context
 */
SPA_EXPORT
struct pw_context *pw_context_new(struct pw_loop *main_loop,
			    struct pw_properties *properties,
			    size_t user_data_size)
{
	struct impl *impl;
	struct pw_context *this;
	const char *name, *lib, *str;
	void *dbus_iface = NULL;
	uint32_t n_support;
	struct pw_properties *pr;
	struct spa_cpu *cpu;
	int res = 0;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL) {
		res = -errno;
		goto error_cleanup;
	}

	this = &impl->this;

	pw_log_debug(NAME" %p: new", this);

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL) {
		res = -errno;
		goto error_free;
	}

	this->properties = properties;

	pr = pw_properties_copy(properties);
	if ((str = pw_properties_get(pr, "context.data-loop." PW_KEY_LIBRARY_NAME_SYSTEM)))
		pw_properties_set(pr, PW_KEY_LIBRARY_NAME_SYSTEM, str);

	this->data_loop_impl = pw_data_loop_new(pr);
	if (this->data_loop_impl == NULL)  {
		res = -errno;
		goto error_free;
	}

	this->pool = pw_mempool_new(NULL);
	if (this->pool == NULL) {
		res = -errno;
		goto error_free_loop;
	}

	this->data_loop = pw_data_loop_get_loop(this->data_loop_impl);
	this->data_system = this->data_loop->system;
	this->main_loop = main_loop;

	n_support = pw_get_support(this->support, SPA_N_ELEMENTS(this->support));
	this->support[n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_System, this->main_loop->system);
	this->support[n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Loop, this->main_loop->loop);
	this->support[n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_LoopUtils, this->main_loop->utils);
	this->support[n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_DataSystem, this->data_system);
	this->support[n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_DataLoop, this->data_loop->loop);

	if ((cpu = spa_support_find(this->support, n_support, SPA_TYPE_INTERFACE_CPU)) != NULL)
		pw_properties_setf(properties, PW_KEY_CPU_MAX_ALIGN, "%u", spa_cpu_get_max_align(cpu));

	lib = pw_properties_get(properties, PW_KEY_LIBRARY_NAME_DBUS);
	if (lib == NULL)
		lib = "support/libspa-dbus";

	impl->dbus_handle = pw_load_spa_handle(lib,
			SPA_NAME_SUPPORT_DBUS, NULL,
			n_support, this->support);

	if (impl->dbus_handle == NULL ||
	    (res = spa_handle_get_interface(impl->dbus_handle,
						SPA_TYPE_INTERFACE_DBus, &dbus_iface)) < 0) {
			pw_log_warn(NAME" %p: can't load dbus interface: %s", this, spa_strerror(res));
	} else {
		this->support[n_support++] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_DBus, dbus_iface);
	}
	this->n_support = n_support;

	pw_array_init(&this->factory_lib, 32);
	pw_map_init(&this->globals, 128, 32);

	spa_list_init(&this->protocol_list);
	spa_list_init(&this->core_list);
	spa_list_init(&this->registry_resource_list);
	spa_list_init(&this->global_list);
	spa_list_init(&this->module_list);
	spa_list_init(&this->device_list);
	spa_list_init(&this->client_list);
	spa_list_init(&this->node_list);
	spa_list_init(&this->factory_list);
	spa_list_init(&this->link_list);
	spa_list_init(&this->control_list[0]);
	spa_list_init(&this->control_list[1]);
	spa_list_init(&this->export_list);
	spa_list_init(&this->driver_list);
	spa_hook_list_init(&this->listener_list);

	if ((name = pw_properties_get(properties, PW_KEY_CORE_NAME)) == NULL) {
		pw_properties_setf(properties,
				   PW_KEY_CORE_NAME, "pipewire-%s-%d",
				   pw_get_user_name(), getpid());
		name = pw_properties_get(properties, PW_KEY_CORE_NAME);
	}

	if ((res = pw_data_loop_start(this->data_loop_impl)) < 0)
		goto error_free_loop;

	this->info.change_mask = 0;
	this->info.user_name = pw_get_user_name();
	this->info.host_name = pw_get_host_name();
	this->info.version = pw_get_library_version();
	srandom(time(NULL));
	this->info.cookie = random();
	this->info.name = name;

	this->sc_pagesize = sysconf(_SC_PAGESIZE);

	this->global = pw_global_new(this,
				     PW_TYPE_INTERFACE_Core,
				     PW_VERSION_CORE,
				     pw_properties_new(
					     PW_KEY_USER_NAME, this->info.user_name,
					     PW_KEY_HOST_NAME, this->info.host_name,
					     PW_KEY_CORE_NAME, this->info.name,
					     PW_KEY_CORE_VERSION, this->info.version,
					     NULL),
				     global_bind,
				     this);
	if (this->global == NULL) {
		res = -errno;
		goto error_free_loop;
	}
	this->info.id = this->global->id;
	pw_properties_setf(this->properties, PW_KEY_OBJECT_ID, "%d", this->info.id);
	this->info.props = &this->properties->dict;

	pw_global_add_listener(this->global, &this->global_listener, &global_events, this);
	pw_global_register(this->global);

	if ((str = pw_properties_get(properties, PW_KEY_CORE_PROFILE_MODULES)) == NULL)
		str = "default";

	load_module_profile(this, str);

	pw_log_debug(NAME" %p: created", this);

	return this;

error_free_loop:
	pw_data_loop_destroy(this->data_loop_impl);
error_free:
	free(this);
error_cleanup:
	if (properties)
		pw_properties_free(properties);
	errno = -res;
	return NULL;
}

/** Destroy a context object
 *
 * \param context a context to destroy
 *
 * \memberof pw_context
 */
SPA_EXPORT
void pw_context_destroy(struct pw_context *context)
{
	struct impl *impl = SPA_CONTAINER_OF(context, struct impl, this);
	struct pw_global *global;
	struct pw_module *module;
	struct pw_device *device;
	struct pw_core *core;
	struct pw_resource *resource;
	struct pw_node *node;
	struct factory_entry *entry;

	pw_log_debug(NAME" %p: destroy", context);
	pw_context_emit_destroy(context);

	spa_hook_remove(&context->global_listener);

	spa_list_consume(core, &context->core_list, link)
		pw_core_disconnect(core);

	spa_list_consume(module, &context->module_list, link)
		pw_module_destroy(module);

	spa_list_consume(node, &context->node_list, link)
		pw_node_destroy(node);

	spa_list_consume(device, &context->device_list, link)
		pw_device_destroy(device);

	spa_list_consume(resource, &context->registry_resource_list, link)
		pw_resource_destroy(resource);

	spa_list_consume(global, &context->global_list, link)
		pw_global_destroy(global);

	pw_log_debug(NAME" %p: free", context);
	pw_context_emit_free(context);

	pw_mempool_destroy(context->pool);

	pw_data_loop_destroy(context->data_loop_impl);

	pw_properties_free(context->properties);

	if (impl->dbus_handle)
		pw_unload_spa_handle(impl->dbus_handle);

	pw_array_for_each(entry, &context->factory_lib) {
		regfree(&entry->regex);
		free(entry->lib);
	}
	pw_array_clear(&context->factory_lib);

	pw_map_clear(&context->globals);

	free(context);
}

SPA_EXPORT
void *pw_context_get_user_data(struct pw_context *context)
{
	return context->user_data;
}

SPA_EXPORT
const struct pw_core_info *pw_context_get_info(struct pw_context *context)
{
	return &context->info;
}

SPA_EXPORT
struct pw_global *pw_context_get_global(struct pw_context *context)
{
	return context->global;
}

SPA_EXPORT
void pw_context_add_listener(struct pw_context *context,
			  struct spa_hook *listener,
			  const struct pw_context_events *events,
			  void *data)
{
	spa_hook_list_append(&context->listener_list, listener, events, data);
}

SPA_EXPORT
const struct spa_support *pw_context_get_support(struct pw_context *context, uint32_t *n_support)
{
	*n_support = context->n_support;
	return context->support;
}

SPA_EXPORT
struct pw_loop *pw_context_get_main_loop(struct pw_context *context)
{
	return context->main_loop;
}

SPA_EXPORT
const struct pw_properties *pw_context_get_properties(struct pw_context *context)
{
	return context->properties;
}

/** Update context properties
 *
 * \param context a context
 * \param dict properties to update
 *
 * Update the context object with the given properties
 *
 * \memberof pw_context
 */
SPA_EXPORT
int pw_context_update_properties(struct pw_context *context, const struct spa_dict *dict)
{
	struct pw_resource *resource;
	int changed;

	changed = pw_properties_update(context->properties, dict);
	context->info.props = &context->properties->dict;

	pw_log_debug(NAME" %p: updated %d properties", context, changed);

	if (!changed)
		return 0;

	context->info.change_mask = PW_CORE_CHANGE_MASK_PROPS;

	pw_context_emit_info_changed(context, &context->info);

	if (context->global)
		spa_list_for_each(resource, &context->global->resource_list, link)
			pw_core_resource_info(resource, &context->info);

	context->info.change_mask = 0;

	return changed;
}

SPA_EXPORT
int pw_context_for_each_global(struct pw_context *context,
			    int (*callback) (void *data, struct pw_global *global),
			    void *data)
{
	struct pw_global *g, *t;
	int res;

	spa_list_for_each_safe(g, t, &context->global_list, link) {
		if (context->current_client &&
		    !PW_PERM_IS_R(pw_global_get_permissions(g, context->current_client)))
			continue;
		if ((res = callback(data, g)) != 0)
			return res;
	}
	return 0;
}

SPA_EXPORT
struct pw_global *pw_context_find_global(struct pw_context *context, uint32_t id)
{
	struct pw_global *global;

	global = pw_map_lookup(&context->globals, id);
	if (global == NULL) {
		errno = ENOENT;
		return NULL;
	}

	if (context->current_client &&
	    !PW_PERM_IS_R(pw_global_get_permissions(global, context->current_client))) {
		errno = EACCES;
		return NULL;
	}
	return global;
}

/** Find a port to link with
 *
 * \param context a context
 * \param other_port a port to find a link with
 * \param id the id of a port or SPA_ID_INVALID
 * \param props extra properties
 * \param n_format_filters number of filters
 * \param format_filters array of format filters
 * \param[out] error an error when something is wrong
 * \return a port that can be used to link to \a otherport or NULL on error
 *
 * \memberof pw_context
 */
struct pw_port *pw_context_find_port(struct pw_context *context,
				  struct pw_port *other_port,
				  uint32_t id,
				  struct pw_properties *props,
				  uint32_t n_format_filters,
				  struct spa_pod **format_filters,
				  char **error)
{
	struct pw_port *best = NULL;
	bool have_id;
	struct pw_node *n;

	have_id = id != SPA_ID_INVALID;

	pw_log_debug(NAME" %p: id:%u", context, id);

	spa_list_for_each(n, &context->node_list, link) {
		if (n->global == NULL)
			continue;

		if (other_port->node == n)
			continue;

		if (context->current_client &&
		    !PW_PERM_IS_R(pw_global_get_permissions(n->global, context->current_client)))
			continue;

		pw_log_debug(NAME" %p: node id:%d", context, n->global->id);

		if (have_id) {
			if (n->global->id == id) {
				pw_log_debug(NAME" %p: id:%u matches node %p", context, id, n);

				best =
				    pw_node_find_port(n,
						pw_direction_reverse(other_port->direction),
						SPA_ID_INVALID);
				if (best)
					break;
			}
		} else {
			struct pw_port *p, *pin, *pout;
			uint8_t buf[4096];
			struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
			struct spa_pod *dummy;

			p = pw_node_find_port(n,
					pw_direction_reverse(other_port->direction),
					SPA_ID_INVALID);
			if (p == NULL)
				continue;

			if (p->direction == PW_DIRECTION_OUTPUT) {
				pin = other_port;
				pout = p;
			} else {
				pin = p;
				pout = other_port;
			}

			if (pw_context_find_format(context,
						pout,
						pin,
						props,
						n_format_filters,
						format_filters,
						&dummy,
						&b,
						error) < 0) {
				free(*error);
				continue;
			}
			best = p;
			break;
		}
	}
	if (best == NULL) {
		asprintf(error, "No matching Node found");
	}
	return best;
}

/** Find a common format between two ports
 *
 * \param context a context object
 * \param output an output port
 * \param input an input port
 * \param props extra properties
 * \param n_format_filters number of format filters
 * \param format_filters array of format filters
 * \param[out] error an error when something is wrong
 * \return a common format of NULL on error
 *
 * Find a common format between the given ports. The format will
 * be restricted to a subset given with the format filters.
 *
 * \memberof pw_context
 */
int pw_context_find_format(struct pw_context *context,
			struct pw_port *output,
			struct pw_port *input,
			struct pw_properties *props,
			uint32_t n_format_filters,
			struct spa_pod **format_filters,
			struct spa_pod **format,
			struct spa_pod_builder *builder,
			char **error)
{
	uint32_t out_state, in_state;
	int res;
	uint32_t iidx = 0, oidx = 0;
	struct spa_pod_builder fb = { 0 };
	uint8_t fbuf[4096];
	struct spa_pod *filter;

	out_state = output->state;
	in_state = input->state;

	pw_log_debug(NAME" %p: finding best format %d %d", context, out_state, in_state);

	/* when a port is configured but the node is idle, we can reconfigure with a different format */
	if (out_state > PW_PORT_STATE_CONFIGURE && output->node->info.state == PW_NODE_STATE_IDLE)
		out_state = PW_PORT_STATE_CONFIGURE;
	if (in_state > PW_PORT_STATE_CONFIGURE && input->node->info.state == PW_NODE_STATE_IDLE)
		in_state = PW_PORT_STATE_CONFIGURE;

	pw_log_debug(NAME" %p: states %d %d", context, out_state, in_state);

	if (in_state == PW_PORT_STATE_CONFIGURE && out_state > PW_PORT_STATE_CONFIGURE) {
		/* only input needs format */
		spa_pod_builder_init(&fb, fbuf, sizeof(fbuf));
		if ((res = spa_node_port_enum_params_sync(output->node->node,
						     output->direction, output->port_id,
						     SPA_PARAM_Format, &oidx,
						     NULL, &filter, &fb)) != 1) {
			if (res < 0)
				asprintf(error, "error get output format: %s", spa_strerror(res));
			else
				asprintf(error, "no output formats");
			goto error;
		}
		pw_log_debug(NAME" %p: Got output format:", context);
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_format(2, NULL, filter);

		if ((res = spa_node_port_enum_params_sync(input->node->node,
						     input->direction, input->port_id,
						     SPA_PARAM_EnumFormat, &iidx,
						     filter, format, builder)) <= 0) {
			if (res < 0)
				asprintf(error, "error input enum formats: %s", spa_strerror(res));
			else
				asprintf(error, "no input formats");
			goto error;
		}
	} else if (out_state >= PW_PORT_STATE_CONFIGURE && in_state > PW_PORT_STATE_CONFIGURE) {
		/* only output needs format */
		spa_pod_builder_init(&fb, fbuf, sizeof(fbuf));
		if ((res = spa_node_port_enum_params_sync(input->node->node,
						     input->direction, input->port_id,
						     SPA_PARAM_Format, &iidx,
						     NULL, &filter, &fb)) != 1) {
			if (res < 0)
				asprintf(error, "error get input format: %s", spa_strerror(res));
			else
				asprintf(error, "no input format");
			goto error;
		}
		pw_log_debug(NAME" %p: Got input format:", context);
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_format(2, NULL, filter);

		if ((res = spa_node_port_enum_params_sync(output->node->node,
						     output->direction, output->port_id,
						     SPA_PARAM_EnumFormat, &oidx,
						     filter, format, builder)) <= 0) {
			if (res < 0)
				asprintf(error, "error output enum formats: %s", spa_strerror(res));
			else
				asprintf(error, "no output format");
			goto error;
		}
	} else if (in_state == PW_PORT_STATE_CONFIGURE && out_state == PW_PORT_STATE_CONFIGURE) {
	      again:
		/* both ports need a format */
		pw_log_debug(NAME" %p: do enum input %d", context, iidx);
		spa_pod_builder_init(&fb, fbuf, sizeof(fbuf));
		if ((res = spa_node_port_enum_params_sync(input->node->node,
						     input->direction, input->port_id,
						     SPA_PARAM_EnumFormat, &iidx,
						     NULL, &filter, &fb)) != 1) {
			if (res == 0 && iidx == 0) {
				asprintf(error, "no compatible formats");
				goto error;
			}
			if (res < 0)
				asprintf(error, "error input enum formats: %s", spa_strerror(res));
			else
				asprintf(error, "no more input formats");
			goto error;
		}
		pw_log_debug(NAME" %p: enum output %d with filter: %p", context, oidx, filter);
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_format(2, NULL, filter);

		if ((res = spa_node_port_enum_params_sync(output->node->node,
						     output->direction, output->port_id,
						     SPA_PARAM_EnumFormat, &oidx,
						     filter, format, builder)) != 1) {
			if (res == 0) {
				oidx = 0;
				goto again;
			}
			asprintf(error, "error output enum formats: %s", spa_strerror(res));
			goto error;
		}

		pw_log_debug(NAME" %p: Got filtered:", context);
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_format(2, NULL, *format);
	} else {
		res = -EBADF;
		asprintf(error, "error bad node state");
		goto error;
	}
	return res;

error:
	if (res == 0)
		res = -ENOENT;
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
 * \memberof pw_context
 */
SPA_EXPORT
struct pw_factory *pw_context_find_factory(struct pw_context *context,
					const char *name)
{
	struct pw_factory *factory;

	spa_list_for_each(factory, &context->factory_list, link) {
		if (strcmp(factory->info.name, name) == 0)
			return factory;
	}
	return NULL;
}

static int collect_nodes(struct pw_node *driver)
{
	struct spa_list queue;
	struct pw_node *n, *t;
	struct pw_port *p;
	struct pw_link *l;
	uint32_t max_quantum = 0;
	uint32_t min_quantum = 0;
	uint32_t quantum;

	spa_list_consume(t, &driver->slave_list, slave_link) {
		spa_list_remove(&t->slave_link);
		spa_list_init(&t->slave_link);
	}

	pw_log_info("driver %p: '%s'", driver, driver->name);

	spa_list_init(&queue);
	spa_list_append(&queue, &driver->sort_link);
	driver->visited = true;

	spa_list_consume(n, &queue, sort_link) {
		spa_list_remove(&n->sort_link);
		pw_node_set_driver(n, driver);

		if (n->quantum_size > 0) {
			if (min_quantum == 0 || n->quantum_size < min_quantum)
				min_quantum = n->quantum_size;
			if (n->quantum_size > max_quantum)
				max_quantum = n->quantum_size;
		}

		spa_list_for_each(p, &n->input_ports, link) {
			spa_list_for_each(l, &p->links, input_link) {
				t = l->output->node;
				if (!t->visited && t->active) {
					t->visited = true;
					spa_list_append(&queue, &t->sort_link);
				}
			}
		}
		spa_list_for_each(p, &n->output_ports, link) {
			spa_list_for_each(l, &p->links, output_link) {
				t = l->input->node;
				if (!t->visited && t->active) {
					t->visited = true;
					spa_list_append(&queue, &t->sort_link);
				}
			}
		}
	}

	quantum = min_quantum;
	if (quantum == 0)
		quantum = DEFAULT_QUANTUM;

	/* for now, we try to limit the latency between min and default, We can
	 * go to max but we should really only do this when in power save mode */
	driver->quantum_current = SPA_CLAMP(quantum, MIN_QUANTUM, DEFAULT_QUANTUM);

	return 0;
}

int pw_context_recalc_graph(struct pw_context *context)
{
	struct pw_node *n, *s, *target;

	/* start from all drivers and group all nodes that are linked
	 * to it. Some nodes are not (yet) linked to anything and they
	 * will end up 'unassigned' to a master. Other nodes are master
	 * and if they have active slaves, we can use them to schedule
	 * the unassigned nodes. */
	target = NULL;
	spa_list_for_each(n, &context->driver_list, driver_link) {
		uint32_t active_slaves;

		if (n->active && !n->visited)
			collect_nodes(n);

		/* from now on we are only interested in nodes that are
		 * a master. We're going to count the number of slaves it
		 * has. */
		if (!n->master)
			continue;

		active_slaves = 0;
		spa_list_for_each(s, &n->slave_list, slave_link) {
			pw_log_info(NAME" %p: driver %p: slave %p %s: %d",
					context, n, s, s->name, s->active);
			if (s != n && s->active)
				active_slaves++;
		}
		pw_log_info(NAME" %p: driver %p active slaves %d",
				context, n, active_slaves);

		/* if the master has active slaves, it is a target for our
		 * unassigned nodes */
		if (active_slaves > 0) {
			if (target == NULL)
				target = n;
		}
	}

	/* now go through all available nodes. The ones we didn't visit
	 * in collect_nodes() are not linked to any master. We assign them
	 * to an active master */
	spa_list_for_each(n, &context->node_list, link) {
		if (!n->visited) {

			pw_log_info(NAME" %p: unassigned node %p: '%s' %d %d", context,
					n, n->name, n->active, n->want_driver);

			if (!n->want_driver)
				continue;

			if (target != NULL) {
				if (n->quantum_size > 0 && n->quantum_size < target->quantum_current)
					target->quantum_current = SPA_MAX(MIN_QUANTUM, n->quantum_size);
			}
			pw_node_set_driver(n, target);
			pw_node_set_state(n, target && n->active ?
					PW_NODE_STATE_RUNNING : PW_NODE_STATE_IDLE);
		}
		n->visited = false;
	}

	/* assign final quantum and debug masters and slaves */
	spa_list_for_each(n, &context->driver_list, driver_link) {
		if (!n->master)
			continue;

		if (n->rt.position && n->quantum_current != n->rt.position->clock.duration)
			n->rt.position->clock.duration = n->quantum_current;

		pw_log_info(NAME" %p: master %p quantum:%u '%s'", context, n,
				n->quantum_current, n->name);

		spa_list_for_each(s, &n->slave_list, slave_link)
			pw_log_info(NAME" %p: slave %p: active:%d '%s'",
					context, s, s->active, s->name);
	}
	return 0;
}

SPA_EXPORT
int pw_context_add_spa_lib(struct pw_context *context,
		const char *factory_regexp, const char *lib)
{
	struct factory_entry *entry;
	int err;

	entry = pw_array_add(&context->factory_lib, sizeof(*entry));
	if (entry == NULL)
		return -errno;

	if ((err = regcomp(&entry->regex, factory_regexp, REG_EXTENDED | REG_NOSUB)) != 0) {
		char errbuf[1024];
		regerror(err, &entry->regex, errbuf, sizeof(errbuf));
		pw_log_error(NAME" %p: can compile regex: %s", context, errbuf);
		pw_array_remove(&context->factory_lib, entry);
		return -EINVAL;
	}

	entry->lib = strdup(lib);
	pw_log_debug(NAME" %p: map factory regex '%s' to '%s", context,
			factory_regexp, lib);
	return 0;
}

SPA_EXPORT
const char *pw_context_find_spa_lib(struct pw_context *context, const char *factory_name)
{
	struct factory_entry *entry;

	pw_array_for_each(entry, &context->factory_lib) {
		if (regexec(&entry->regex, factory_name, 0, NULL, 0) == 0)
			return entry->lib;
	}
	return NULL;
}

SPA_EXPORT
struct spa_handle *pw_context_load_spa_handle(struct pw_context *context,
		const char *factory_name,
		const struct spa_dict *info)
{
	const char *lib;
	const struct spa_support *support;
	uint32_t n_support;
	struct spa_handle *handle;

	pw_log_debug(NAME" %p: load factory %s", context, factory_name);

	lib = pw_context_find_spa_lib(context, factory_name);
	if (lib == NULL && info != NULL)
		lib = spa_dict_lookup(info, SPA_KEY_LIBRARY_NAME);
	if (lib == NULL) {
		pw_log_warn(NAME" %p: no library for %s: %m",
				context, factory_name);
		errno = ENOENT;
		return NULL;
	}

	support = pw_context_get_support(context, &n_support);

	handle = pw_load_spa_handle(lib, factory_name,
			info, n_support, support);

	return handle;
}

SPA_EXPORT
int pw_context_register_export_type(struct pw_context *context, struct pw_export_type *type)
{
	pw_log_debug("context %p: Add export type %d/%s to context", context, type->type,
			spa_debug_type_find_name(pw_type_info(), type->type));
	spa_list_append(&context->export_list, &type->link);
	return 0;
}

SPA_EXPORT
const struct pw_export_type *pw_context_find_export_type(struct pw_context *context, uint32_t type)
{
	const struct pw_export_type *t;
	spa_list_for_each(t, &context->export_list, link) {
		if (t->type == type)
			return t;
	}
	return NULL;
}
