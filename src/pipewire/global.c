/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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
#include <unistd.h>
#include <time.h>
#include <stdio.h>

#include <pipewire/pipewire.h>
#include <pipewire/private.h>
#include <pipewire/global.h>

/** \cond */
struct global_impl {
	struct pw_global this;
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
	struct global_impl *impl;
	struct pw_global *this;

	impl = calloc(1, sizeof(struct global_impl));
	if (impl == NULL)
		return NULL;

	this = &impl->this;

	this->core = core;
	this->type = type;
	this->version = version;
	this->object = object;
	this->properties = properties;
	this->id = SPA_ID_INVALID;

	spa_hook_list_init(&this->listener_list);

	pw_log_debug("global %p: new %s", this,
			spa_type_map_get_type(core->type.map, this->type));

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

	global->id = pw_map_insert_new(&core->globals, global);

	spa_list_append(&core->global_list, &global->link);

	pw_log_debug("global %p: add %u owner %p parent %p", global, global->id, owner, parent);
	pw_core_events_global_added(core, global);

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
 * \param id the id
 *
 * Let \a client bind to \a global with the given version and id.
 * After binding, the client and the global object will be able to
 * exchange messages.
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
	pw_core_resource_error(client->core_resource,
			       client->core_resource->id,
			     res, "id %d: interface version %d < %d",
			     id, global->version, version);
	return res;
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
	struct pw_core *core = global->core;
	struct pw_resource *registry;

	pw_log_debug("global %p: destroy %u", global, global->id);
	pw_global_events_destroy(global);

	if (global->id != SPA_ID_INVALID) {
		spa_list_for_each(registry, &core->registry_resource_list, link) {
			uint32_t permissions = pw_global_get_permissions(global, registry->client);
			pw_log_debug("registry %p: global %d %08x", registry, global->id, permissions);
			if (PW_PERM_IS_R(permissions))
				pw_registry_resource_global_remove(registry, global->id);
		}

		pw_map_remove(&core->globals, global->id);

		spa_list_remove(&global->link);
		pw_core_events_global_removed(core, global);
	}

	pw_log_debug("global %p: free", global);
	pw_global_events_free(global);

	if (global->properties)
		pw_properties_free(global->properties);

	free(global);
}
