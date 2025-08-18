/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>

#include <pipewire/impl.h>
#include <pipewire/private.h>

#include <spa/debug/types.h>
#include <spa/utils/string.h>

PW_LOG_TOPIC_EXTERN(log_global);
#define PW_LOG_TOPIC_DEFAULT log_global

/** \cond */
struct impl {
	struct pw_global this;
};
/** \endcond */

SPA_EXPORT
uint32_t pw_global_get_permissions(struct pw_global *global, struct pw_impl_client *client)
{
	uint32_t permissions = global->permission_mask;
	if (client->permission_func != NULL)
		permissions &= client->permission_func(global, client, client->permission_data);
	return permissions;
}

/** Create a new global
 *
 * \param context a context object
 * \param type the type of the global
 * \param version the version of the type
 * \param properties extra properties
 * \param func a function to bind to this global
 * \param object the associated object
 * \return a result global
 *
 */
SPA_EXPORT
struct pw_global *
pw_global_new(struct pw_context *context,
	      const char *type,
	      uint32_t version,
	      uint32_t permission_mask,
	      struct pw_properties *properties,
	      pw_global_bind_func_t func,
	      void *object)
{
	struct impl *impl;
	struct pw_global *this;
	uint64_t serial;
	int res;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		return NULL;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL) {
		res = -errno;
		goto error_cleanup;
	}

	this = &impl->this;

	this->context = context;
	this->type = type;
	this->version = version;
	this->permission_mask = permission_mask;
	this->func = func;
	this->object = object;
	this->properties = properties;
	this->id = pw_map_insert_new(&context->globals, this);
	if (this->id == SPA_ID_INVALID) {
		res = -errno;
		pw_log_error("%p: can't allocate new id: %m", this);
		goto error_free;
	}
	this->serial = SPA_ID_INVALID;

	spa_list_init(&this->resource_list);
	spa_hook_list_init(&this->listener_list);

	serial = pw_global_get_serial(this);
	res = pw_properties_setf(properties, PW_KEY_OBJECT_SERIAL, "%" PRIu64, serial);
	if (res < 0) {
		pw_global_destroy(this);
		errno = -res;
		return NULL;
	}

	pw_log_debug("%p: new %s %d", this, this->type, this->id);

	return this;

error_free:
	free(impl);
error_cleanup:
	pw_properties_free(properties);
	errno = -res;
	return NULL;
}

SPA_EXPORT
uint64_t pw_global_get_serial(struct pw_global *global)
{
	struct pw_context *context = global->context;
	if (global->serial == SPA_ID_INVALID)
		global->serial = context->serial++;
	if ((uint32_t)context->serial == SPA_ID_INVALID)
		context->serial++;
	return global->serial;
}

/** register a global to the context registry
 *
 * \param global a global to add
 * \return 0 on success < 0 errno value on failure
 *
 */
SPA_EXPORT
int pw_global_register(struct pw_global *global)
{
	struct pw_resource *registry;
	struct pw_context *context = global->context;
	struct pw_impl_client *client;

	if (global->registered)
		return -EEXIST;

	spa_list_append(&context->global_list, &global->link);
	global->registered = true;

	global->generation = ++context->generation;

	spa_list_for_each(registry, &context->registry_resource_list, link) {
		uint32_t permissions = pw_global_get_permissions(global, registry->client);
		pw_log_debug("registry %p: global %d %08x serial:%"PRIu64" generation:%"PRIu64,
				registry, global->id, permissions, global->serial, global->generation);
		if (PW_PERM_IS_R(permissions))
			pw_registry_resource_global(registry,
						    global->id,
						    permissions,
						    global->type,
						    global->version,
						    &global->properties->dict);
	}

	/* Ensure a message is sent also to clients without registries, to force
	 * generation number update. */
	spa_list_for_each(client, &context->client_list, link) {
		uint32_t permissions;

		if (client->sent_generation >= context->generation)
			continue;
		if (!client->core_resource)
			continue;

		permissions = pw_global_get_permissions(global, client);
		if (PW_PERM_IS_R(permissions)) {
			pw_log_debug("impl-client %p: (no registry) global %d %08x serial:%"PRIu64
					" generation:%"PRIu64, client, global->id, permissions, global->serial,
					global->generation);
			pw_core_resource_done(client->core_resource, SPA_ID_INVALID, 0);
		}
	}

	pw_log_debug("%p: registered %u", global, global->id);
	pw_context_emit_global_added(context, global);

	return 0;
}

static int global_unregister(struct pw_global *global)
{
	struct pw_context *context = global->context;
	struct pw_resource *resource;

	if (!global->registered)
		return 0;

	spa_list_for_each(resource, &context->registry_resource_list, link) {
		uint32_t permissions = pw_global_get_permissions(global, resource->client);
		pw_log_debug("registry %p: global %d %08x", resource, global->id, permissions);
		if (PW_PERM_IS_R(permissions))
			pw_registry_resource_global_remove(resource, global->id);
	}

	spa_list_remove(&global->link);
	global->registered = false;
	global->serial = SPA_ID_INVALID;

	pw_log_debug("%p: unregistered %u", global, global->id);
	pw_context_emit_global_removed(context, global);

	return 0;
}

SPA_EXPORT
struct pw_context *pw_global_get_context(struct pw_global *global)
{
	return global->context;
}

SPA_EXPORT
const char * pw_global_get_type(struct pw_global *global)
{
	return global->type;
}

SPA_EXPORT
bool pw_global_is_type(struct pw_global *global, const char *type)
{
	return spa_streq(global->type, type);
}

SPA_EXPORT
uint32_t pw_global_get_version(struct pw_global *global)
{
	return global->version;
}

SPA_EXPORT
const struct pw_properties *pw_global_get_properties(struct pw_global *global)
{
	return global->properties;
}

SPA_EXPORT
int pw_global_update_keys(struct pw_global *global,
		     const struct spa_dict *dict, const char * const keys[])
{
	return pw_properties_update_keys(global->properties, dict, keys);
}

SPA_EXPORT
void * pw_global_get_object(struct pw_global *global)
{
	return global->object;
}

SPA_EXPORT
uint32_t pw_global_get_id(struct pw_global *global)
{
	return global->id;
}

SPA_EXPORT
int pw_global_add_resource(struct pw_global *global, struct pw_resource *resource)
{
	resource->global = global;
	pw_log_debug("%p: resource %p id:%d global:%d", global, resource,
			resource->id, global->id);
	spa_list_append(&global->resource_list, &resource->link);
	pw_resource_set_bound_id(resource, global->id);
	return 0;
}

SPA_EXPORT
int pw_global_for_each_resource(struct pw_global *global,
			   int (*callback) (void *data, struct pw_resource *resource),
			   void *data)
{
	struct pw_resource *resource, *t;
	int res;

	spa_list_for_each_safe(resource, t, &global->resource_list, link)
		if ((res = callback(data, resource)) != 0)
			return res;
	return 0;
}

SPA_EXPORT
void pw_global_add_listener(struct pw_global *global,
			    struct spa_hook *listener,
			    const struct pw_global_events *events,
			    void *data)
{
	spa_hook_list_append(&global->listener_list, listener, events, data);
}

/** Bind to a global
 *
 * \param global the global to bind to
 * \param client the client that binds
 * \param permissions the \ref pw_permission
 * \param version the version
 * \param id the id of the resource
 *
 * Let \a client bind to \a global with the given version and id.
 * After binding, the client and the global object will be able to
 * exchange messages on the proxy/resource with \a id.
 *
 */
SPA_EXPORT int
pw_global_bind(struct pw_global *global, struct pw_impl_client *client, uint32_t permissions,
              uint32_t version, uint32_t id)
{
	int res;

	if (global->version < version)
		goto error_version;

	if ((res = global->func(global->object, client, permissions, version, id)) < 0)
		goto error_bind;

	return res;

error_version:
	res = -EPROTO;
	if (client->core_resource)
		pw_core_resource_errorf(client->core_resource, id, client->recv_seq,
				res, "id %d: interface version %d < %d",
				id, global->version, version);
	goto error_exit;
error_bind:
	if (client->core_resource)
		pw_core_resource_errorf(client->core_resource, id, client->recv_seq,
			res, "can't bind global %u/%u: %d (%s)", id, version,
			res, spa_strerror(res));
	goto error_exit;

error_exit:
	pw_log_error("%p: can't bind global %u/%u: %d (%s)", global, id,
			version, res, spa_strerror(res));
	pw_map_insert_at(&client->objects, id, NULL);
	if (client->core_resource)
		pw_core_resource_remove_id(client->core_resource, id);
	return res;
}

SPA_EXPORT
int pw_global_update_permissions(struct pw_global *global, struct pw_impl_client *client,
		uint32_t old_permissions, uint32_t new_permissions)
{
	struct pw_context *context = global->context;
	struct pw_resource *resource, *t;
	bool do_hide, do_show;

	do_hide = PW_PERM_IS_R(old_permissions) && !PW_PERM_IS_R(new_permissions);
	do_show = !PW_PERM_IS_R(old_permissions) && PW_PERM_IS_R(new_permissions);

	pw_log_debug("%p: client %p permissions changed %d %08x -> %08x",
			global, client, global->id, old_permissions, new_permissions);

	pw_global_emit_permissions_changed(global, client, old_permissions, new_permissions);

	spa_list_for_each(resource, &context->registry_resource_list, link) {
		if (resource->client != client)
			continue;

		if (do_hide) {
			pw_log_debug("client %p: resource %p hide global %d",
					client, resource, global->id);
			pw_registry_resource_global_remove(resource, global->id);
		}
		else if (do_show) {
			pw_log_debug("client %p: resource %p show global %d serial:%"PRIu64,
					client, resource, global->id, global->serial);
			pw_registry_resource_global(resource,
						    global->id,
						    new_permissions,
						    global->type,
						    global->version,
						    &global->properties->dict);
		}
	}

	spa_list_for_each_safe(resource, t, &global->resource_list, link) {
		if (resource->client != client)
			continue;

		/* don't ever destroy the core resource */
		if (!PW_PERM_IS_R(new_permissions) && global->id != PW_ID_CORE)
			pw_resource_destroy(resource);
		else
			resource->permissions = new_permissions;
	}
	return 0;
}

/** Destroy a global
 *
 * \param global a global to destroy
 *
 */
SPA_EXPORT
void pw_global_destroy(struct pw_global *global)
{
	struct pw_resource *resource;
	struct pw_context *context = global->context;

	if (global->destroyed)
		return;

	global->destroyed = true;

	pw_log_debug("%p: destroy %u", global, global->id);
	pw_global_emit_destroy(global);

	spa_list_consume(resource, &global->resource_list, link)
		pw_resource_destroy(resource);

	global_unregister(global);

	pw_log_debug("%p: free", global);
	pw_global_emit_free(global);

	pw_map_remove(&context->globals, global->id);
	spa_hook_list_clean(&global->listener_list);

	pw_properties_free(global->properties);

	free(global);
}
