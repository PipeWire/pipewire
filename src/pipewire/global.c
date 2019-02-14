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

#include <pipewire/private.h>
#include <pipewire/global.h>
#include <pipewire/interfaces.h>
#include <pipewire/type.h>

#include <spa/debug/types.h>

/** \cond */
struct impl {
	struct pw_global this;
	bool registered;
};

/** \endcond */

SPA_EXPORT
uint32_t pw_global_get_permissions(struct pw_global *global, struct pw_client *client)
{
	uint32_t perms = PW_PERM_RWX;

	if (client->permission_func != NULL)
		perms &= client->permission_func(global, client, client->permission_data);

	return perms;
}

/** Create a new global
 *
 * \param core a core object
 * \param type the type of the global
 * \param version the version of the type
 * \param properties extra properties
 * \param bind a function to bind to this global
 * \param object the associated object
 * \return a result global
 *
 * \memberof pw_global
 */
SPA_EXPORT
struct pw_global *
pw_global_new(struct pw_core *core,
	      uint32_t type,
	      uint32_t version,
	      struct pw_properties *properties,
	      void *object)
{
	struct impl *impl;
	struct pw_global *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	this = &impl->this;

	this->core = core;
	this->type = type;
	this->version = version;
	this->object = object;
	this->properties = properties;
	this->id = pw_map_insert_new(&core->globals, this);

	spa_list_init(&this->resource_list);
	spa_hook_list_init(&this->listener_list);

	pw_log_debug("global %p: new %s %d", this,
			spa_debug_type_find_name(pw_type_info(), this->type),
			this->id);

	return this;
}

/** register a global to the core registry
 *
 * \param global a global to add
 * \param owner an optional owner client of the global
 * \param parent an optional parent of the global
 * \return 0 on success < 0 errno value on failure
 *
 * \memberof pw_global
 */
SPA_EXPORT
int
pw_global_register(struct pw_global *global,
		   struct pw_client *owner,
		   struct pw_global *parent)
{
	struct impl *impl = SPA_CONTAINER_OF(global, struct impl, this);
	struct pw_resource *registry;
	struct pw_core *core = global->core;

	global->owner = owner;
	if (owner && parent == NULL)
		parent = owner->global;
	if (parent == NULL)
		parent = core->global;
	if (parent == NULL)
		parent = global;
	global->parent = parent;

	spa_list_append(&core->global_list, &global->link);
	impl->registered = true;

	spa_list_for_each(registry, &core->registry_resource_list, link) {
		uint32_t permissions = pw_global_get_permissions(global, registry->client);
		pw_log_debug("registry %p: global %d %08x", registry, global->id, permissions);
		if (PW_PERM_IS_R(permissions))
			pw_registry_resource_global(registry,
						    global->id,
						    global->parent->id,
						    permissions,
						    global->type,
						    global->version,
						    global->properties ?
						        &global->properties->dict : NULL);
	}

	pw_global_events_registering(global);

	pw_log_debug("global %p: add %u owner %p parent %p", global, global->id, owner, parent);
	pw_core_events_global_added(core, global);

	return 0;
}

static int global_unregister(struct pw_global *global)
{
	struct impl *impl = SPA_CONTAINER_OF(global, struct impl, this);
	struct pw_core *core = global->core;
	struct pw_resource *resource;

	if (!impl->registered)
		return 0;

	spa_list_for_each(resource, &core->registry_resource_list, link) {
		uint32_t permissions = pw_global_get_permissions(global, resource->client);
		pw_log_debug("registry %p: global %d %08x", resource, global->id, permissions);
		if (PW_PERM_IS_R(permissions))
			pw_registry_resource_global_remove(resource, global->id);
	}
	spa_list_consume(resource, &global->resource_list, link)
		pw_resource_destroy(resource);

	spa_list_remove(&global->link);
	pw_map_remove(&core->globals, global->id);
	pw_core_events_global_removed(core, global);

	impl->registered = false;

	return 0;
}

SPA_EXPORT
struct pw_core *pw_global_get_core(struct pw_global *global)
{
	return global->core;
}

SPA_EXPORT
struct pw_client *pw_global_get_owner(struct pw_global *global)
{
	return global->owner;
}

SPA_EXPORT
struct pw_global *pw_global_get_parent(struct pw_global *global)
{
	return global->parent;
}

SPA_EXPORT
uint32_t pw_global_get_type(struct pw_global *global)
{
	return global->type;
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
 * \param version the version
 * \param id the id of the resource
 *
 * Let \a client bind to \a global with the given version and id.
 * After binding, the client and the global object will be able to
 * exchange messages on the proxy/resource with \a id.
 *
 * \memberof pw_global
 */
SPA_EXPORT
int
pw_global_bind(struct pw_global *global, struct pw_client *client, uint32_t permissions,
              uint32_t version, uint32_t id)
{
	int res;

	if (global->version < version)
		goto wrong_version;

	pw_global_events_bind(global, client, permissions, version, id);

	return 0;

      wrong_version:
	res = -EINVAL;
	pw_core_resource_errorf(client->core_resource, id,
			res, "id %d: interface version %d < %d",
			id, global->version, version);
	return res;
}

SPA_EXPORT
int pw_global_update_permissions(struct pw_global *global, struct pw_client *client,
		uint32_t old_permissions, uint32_t new_permissions)
{
	struct pw_core *core = global->core;
	struct pw_resource *resource, *t;

	spa_list_for_each(resource, &core->registry_resource_list, link) {
		if (resource->client != client)
			continue;

		if (PW_PERM_IS_R(old_permissions) && !PW_PERM_IS_R(new_permissions)) {
			pw_registry_resource_global_remove(resource, global->id);
		}
		else if (!PW_PERM_IS_R(old_permissions) && PW_PERM_IS_R(new_permissions)) {
			pw_registry_resource_global(resource,
						    global->id,
						    global->parent->id,
						    new_permissions,
						    global->type,
						    global->version,
						    global->properties ?
						        &global->properties->dict : NULL);
		}
	}
	spa_list_for_each_safe(resource, t, &global->resource_list, link) {
		if (resource->client != client)
			continue;

		/* don't ever destroy the core resource */
		if (!PW_PERM_IS_R(new_permissions) && global->id != 0)
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
 * \memberof pw_global
 */
SPA_EXPORT
void pw_global_destroy(struct pw_global *global)
{
	struct pw_resource *resource;

	pw_log_debug("global %p: destroy %u", global, global->id);
	pw_global_events_destroy(global);

	global_unregister(global);

	spa_list_consume(resource, &global->resource_list, link)
		pw_resource_destroy(resource);

	pw_log_debug("global %p: free", global);
	pw_global_events_free(global);

	if (global->properties)
		pw_properties_free(global->properties);

	free(global);
}
