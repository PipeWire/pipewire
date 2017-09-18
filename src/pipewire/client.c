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

#include <string.h>

#include "pipewire/pipewire.h"
#include "pipewire/interfaces.h"

#include "pipewire/client.h"
#include "pipewire/private.h"
#include "pipewire/resource.h"

/** \cond */
struct impl {
	struct pw_client this;
};

struct resource_data {
	struct spa_hook resource_listener;
};

/** \endcond */

static void client_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	spa_list_remove(&resource->link);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = client_unbind_func,
};

static int
client_bind_func(struct pw_global *global,
		 struct pw_client *client, uint32_t permissions,
		 uint32_t version, uint32_t id)
{
	struct pw_client *this = global->object;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	pw_resource_add_listener(resource, &data->resource_listener, &resource_events, resource);

	pw_log_debug("client %p: bound to %d", this, resource->id);

	spa_list_insert(this->resource_list.prev, &resource->link);

	this->info.change_mask = ~0;
	pw_client_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return SPA_RESULT_OK;

      no_mem:
	pw_log_error("can't create client resource");
	pw_resource_error(client->core_resource, SPA_RESULT_NO_MEMORY, "no memory");
	return SPA_RESULT_NO_MEMORY;
}

/** Make a new client object
 *
 * \param core a \ref pw_core object to register the client with
 * \param ucred a ucred structure or NULL when unknown
 * \param properties optional client properties, ownership is taken
 * \return a newly allocated client object
 *
 * \memberof pw_client
 */
struct pw_client *pw_client_new(struct pw_core *core,
				struct ucred *ucred,
				struct pw_properties *properties,
				size_t user_data_size)
{
	struct pw_client *this;
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	pw_log_debug("client %p: new", this);

	this->core = core;
	if ((this->ucred_valid = (ucred != NULL)))
		this->ucred = *ucred;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		return NULL;

	if (ucred) {
		pw_properties_setf(properties, PW_CLIENT_PROP_UCRED_PID, "%d", ucred->pid);
		pw_properties_setf(properties, PW_CLIENT_PROP_UCRED_UID, "%d", ucred->uid);
		pw_properties_setf(properties, PW_CLIENT_PROP_UCRED_GID, "%d", ucred->gid);
	}

	this->properties = properties;

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	spa_list_init(&this->resource_list);
	spa_hook_list_init(&this->listener_list);

	pw_map_init(&this->objects, 0, 32);
	pw_map_init(&this->types, 0, 32);

	this->info.props = this->properties ? &this->properties->dict : NULL;

	return this;
}

void pw_client_register(struct pw_client *client,
			struct pw_client *owner,
			struct pw_global *parent)
{
	struct pw_core *core = client->core;

	pw_log_debug("client %p: register parent %d", client, parent ? parent->id : SPA_ID_INVALID);
	spa_list_insert(core->client_list.prev, &client->link);
	client->global = pw_core_add_global(core, owner, parent, core->type.client, PW_VERSION_CLIENT,
			   client_bind_func, client);
	client->info.id = client->global->id;
}

struct pw_core *pw_client_get_core(struct pw_client *client)
{
	return client->core;
}

struct pw_resource *pw_client_get_core_resource(struct pw_client *client)
{
	return client->core_resource;
}

struct pw_resource *pw_client_find_resource(struct pw_client *client, uint32_t id)
{
	return pw_map_lookup(&client->objects, id);
}

struct pw_global *pw_client_get_global(struct pw_client *client)
{
	return client->global;
}

const struct pw_properties *pw_client_get_properties(struct pw_client *client)
{
	return client->properties;
}

const struct ucred *pw_client_get_ucred(struct pw_client *client)
{
	if (!client->ucred_valid)
		return NULL;

	return &client->ucred;
}

void *pw_client_get_user_data(struct pw_client *client)
{
	return client->user_data;
}

static void destroy_resource(void *object, void *data)
{
	if (object)
		pw_resource_destroy(object);
}

/** Destroy a client object
 *
 * \param client the client to destroy
 *
 * \memberof pw_client
 */
void pw_client_destroy(struct pw_client *client)
{
	struct pw_resource *resource, *tmp;
	struct impl *impl = SPA_CONTAINER_OF(client, struct impl, this);

	pw_log_debug("client %p: destroy", client);
	spa_hook_list_call(&client->listener_list, struct pw_client_events, destroy);

	if (client->global) {
		spa_list_remove(&client->link);
		pw_global_destroy(client->global);
	}

	spa_list_for_each_safe(resource, tmp, &client->resource_list, link)
	    pw_resource_destroy(resource);

	pw_map_for_each(&client->objects, destroy_resource, client);

	spa_hook_list_call(&client->listener_list, struct pw_client_events, free);
	pw_log_debug("client %p: free", impl);

	pw_map_clear(&client->objects);
	pw_map_clear(&client->types);

	if (client->properties)
		pw_properties_free(client->properties);

	free(impl);
}

void pw_client_add_listener(struct pw_client *client,
			    struct spa_hook *listener,
			    const struct pw_client_events *events,
			    void *data)
{
	spa_hook_list_append(&client->listener_list, listener, events, data);
}

const struct pw_client_info *pw_client_get_info(struct pw_client *client)
{
	return &client->info;
}

/** Update client properties
 *
 * \param client the client
 * \param dict a \ref spa_dict with properties
 *
 * Add all properties in \a dict to the client properties. Existing
 * properties are overwritten. Items can be removed by setting the value
 * to NULL.
 *
 * \memberof pw_client
 */
void pw_client_update_properties(struct pw_client *client, const struct spa_dict *dict)
{
	struct pw_resource *resource;

	if (client->properties == NULL) {
		if (dict)
			client->properties = pw_properties_new_dict(dict);
	} else {
		uint32_t i;

		for (i = 0; i < dict->n_items; i++)
			pw_properties_set(client->properties,
					  dict->items[i].key, dict->items[i].value);
	}

	client->info.change_mask |= PW_CLIENT_CHANGE_MASK_PROPS;
	client->info.props = client->properties ? &client->properties->dict : NULL;

	spa_hook_list_call(&client->listener_list, struct pw_client_events, info_changed, &client->info);

	spa_list_for_each(resource, &client->resource_list, link)
		pw_client_resource_info(resource, &client->info);

	client->info.change_mask = 0;
}

void pw_client_set_busy(struct pw_client *client, bool busy)
{
	if (client->busy != busy) {
		pw_log_debug("client %p: busy %d", client, busy);
		client->busy = busy;
		spa_hook_list_call(&client->listener_list, struct pw_client_events, busy_changed, busy);
	}
}
