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
#include <unistd.h>
#include <time.h>
#include <stdio.h>

#include <spa/lib/debug.h>

#include <pipewire/pipewire.h>
#include <pipewire/private.h>
#include <pipewire/global.h>

/** \cond */
struct global_impl {
	struct pw_global this;
};

/** \endcond */

uint32_t pw_global_get_permissions(struct pw_global *global, struct pw_client *client)
{
	struct pw_core *core = client->core;

	if (core->permission_func == NULL)
		return PW_PERM_RWX;

	return core->permission_func(global, client, core->permission_data);
}

/** Create and add a new global to the core
 *
 * \param core a core
 * \param owner an optional owner of the global
 * \param type the type of the global
 * \param n_ifaces number of interfaces
 * \param ifaces interface information
 * \param object the associated object
 * \param bind a function to bind to this global
 * \param[out] global a result global
 * \return true on success
 *
 * \memberof pw_core
 */
struct pw_global *
pw_core_add_global(struct pw_core *core,
	           struct pw_client *owner,
	           struct pw_global *parent,
		   uint32_t type,
		   uint32_t version,
		   pw_bind_func_t bind,
		   void *object)
{
	struct global_impl *impl;
	struct pw_global *this;
	struct pw_resource *registry;

	impl = calloc(1, sizeof(struct global_impl));
	if (impl == NULL)
		return NULL;

	this = &impl->this;

	this->core = core;
	this->owner = owner;
	this->type = type;
	this->version = version;
	this->bind = bind;
	this->object = object;

	this->id = pw_map_insert_new(&core->globals, this);

	if (owner)
		parent = owner->global;
	if (parent == NULL)
		parent = core->global;
	if (parent == NULL)
		parent = this;
	this->parent = parent;

	spa_list_insert(core->global_list.prev, &this->link);

	spa_hook_list_call(&core->listener_list, struct pw_core_events, global_added, this);

	pw_log_debug("global %p: new %u %s, owner %p", this, this->id,
			spa_type_map_get_type(core->type.map, this->type), owner);

	spa_list_for_each(registry, &core->registry_resource_list, link) {
		uint32_t permissions = pw_global_get_permissions(this, registry->client);
		if (PW_PERM_IS_R(permissions))
			pw_registry_resource_global(registry,
						    this->id,
						    this->parent->id,
						    permissions,
						    this->type,
						    this->version);
	}
	return this;
}

struct pw_core *pw_global_get_core(struct pw_global *global)
{
	return global->core;
}


struct pw_client *pw_global_get_owner(struct pw_global *global)
{
	return global->owner;
}

struct pw_global *pw_global_get_parent(struct pw_global *global)
{
	return global->parent;
}

uint32_t pw_global_get_type(struct pw_global *global)
{
	return global->type;
}

uint32_t pw_global_get_version(struct pw_global *global)
{
	return global->version;
}

void * pw_global_get_object(struct pw_global *global)
{
	return global->object;
}

uint32_t pw_global_get_id(struct pw_global *global)
{
	return global->id;
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
int
pw_global_bind(struct pw_global *global, struct pw_client *client, uint32_t permissions,
	       uint32_t version, uint32_t id)
{
	int res;

	if (global->bind == NULL)
		goto no_bind;

	if (global->version < version)
		goto wrong_version;

	res = global->bind(global, client, permissions, version, id);

	return res;

     wrong_version:
	res = SPA_RESULT_INCOMPATIBLE_VERSION;
	pw_core_resource_error(client->core_resource,
			       client->core_resource->id,
			     res, "id %d: interface version %d < %d",
			     id, global->version, version);
	return res;
     no_bind:
	res = SPA_RESULT_NOT_IMPLEMENTED;
	pw_core_resource_error(client->core_resource,
			       client->core_resource->id,
			     res, "can't bind object id %d to interface", id);
	return res;
}

/** Destroy a global
 *
 * \param global a global to destroy
 *
 * \memberof pw_global
 */
void pw_global_destroy(struct pw_global *global)
{
	struct pw_core *core = global->core;
	struct pw_resource *registry;

	pw_log_debug("global %p: destroy %u", global, global->id);

	spa_list_for_each(registry, &core->registry_resource_list, link) {
		uint32_t permissions = pw_global_get_permissions(global, registry->client);
		if (PW_PERM_IS_R(permissions))
			pw_registry_resource_global_remove(registry, global->id);
	}

	pw_map_remove(&core->globals, global->id);

	spa_list_remove(&global->link);
	spa_hook_list_call(&core->listener_list, struct pw_core_events, global_removed, global);

	pw_log_debug("global %p: free", global);
	free(global);
}
