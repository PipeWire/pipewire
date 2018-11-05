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

#include <string.h>

#include "pipewire/interfaces.h"
#include "pipewire/private.h"
#include "pipewire/protocol.h"
#include "pipewire/resource.h"

/** \cond */
struct impl {
	struct pw_resource this;
};
/** \endcond */

struct pw_resource *pw_resource_new(struct pw_client *client,
				    uint32_t id,
				    uint32_t permissions,
				    uint32_t type,
				    uint32_t version,
				    size_t user_data_size)
{
	struct impl *impl;
	struct pw_resource *this;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	this->core = client->core;
	this->client = client;
	this->permissions = permissions;
	this->type = type;
	this->version = version;

	spa_hook_list_init(&this->implementation_list);
	spa_hook_list_append(&this->implementation_list, &this->implementation, NULL, NULL);
	spa_hook_list_init(&this->listener_list);

	if (id == SPA_ID_INVALID) {
		id = pw_map_insert_new(&client->objects, this);
	} else if (!pw_map_insert_at(&client->objects, id, this))
		goto in_use;

	this->id = id;

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	this->marshal = pw_protocol_get_marshal(client->protocol, type);

	pw_log_debug("resource %p: new for client %p id %u", this, client, id);
	pw_client_events_resource_added(client, this);

	return this;

      in_use:
	pw_log_debug("resource %p: id %u in use for client %p", this, id, client);
	free(impl);
	return NULL;
}

struct pw_client *pw_resource_get_client(struct pw_resource *resource)
{
	return resource->client;
}

uint32_t pw_resource_get_id(struct pw_resource *resource)
{
	return resource->id;
}

uint32_t pw_resource_get_permissions(struct pw_resource *resource)
{
	return resource->permissions;
}

uint32_t pw_resource_get_type(struct pw_resource *resource)
{
	return resource->type;
}

struct pw_protocol *pw_resource_get_protocol(struct pw_resource *resource)
{
	return resource->client->protocol;
}

void *pw_resource_get_user_data(struct pw_resource *resource)
{
	return resource->user_data;
}

void pw_resource_add_listener(struct pw_resource *resource,
			      struct spa_hook *listener,
			      const struct pw_resource_events *events,
			      void *data)
{
	spa_hook_list_append(&resource->listener_list, listener, events, data);
}

void pw_resource_set_implementation(struct pw_resource *resource,
				    const void *implementation,
				    void *data)
{
	struct pw_client *client = resource->client;

	resource->implementation.funcs = implementation;
	resource->implementation.data = data;

	pw_client_events_resource_impl(client, resource);
}

void pw_resource_add_override(struct pw_resource *resource,
			      struct spa_hook *listener,
			      const void *implementation,
			      void *data)
{
	spa_hook_list_prepend(&resource->implementation_list, listener, implementation, data);
}

struct spa_hook_list *pw_resource_get_implementation(struct pw_resource *resource)
{
	return &resource->implementation_list;
}

const struct pw_protocol_marshal *pw_resource_get_marshal(struct pw_resource *resource)
{
	return resource->marshal;
}

void pw_resource_error(struct pw_resource *resource, uint32_t id, int result, const char *error)
{
	if (resource->client->core_resource)
		pw_core_resource_error(resource->client->core_resource, id, result, error);
}

void pw_resource_destroy(struct pw_resource *resource)
{
	struct pw_client *client = resource->client;

	pw_log_debug("resource %p: destroy %u", resource, resource->id);
	pw_resource_events_destroy(resource);

	pw_map_insert_at(&client->objects, resource->id, NULL);
	pw_client_events_resource_removed(client, resource);

	if (client->core_resource)
		pw_core_resource_remove_id(client->core_resource, resource->id);

	pw_log_debug("resource %p: free", resource);
	free(resource);
}
