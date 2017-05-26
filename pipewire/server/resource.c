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

#include "pipewire/client/interfaces.h"
#include "pipewire/server/resource.h"

struct impl {
	struct pw_resource this;
};

struct pw_resource *pw_resource_new(struct pw_client *client,
				    uint32_t id, uint32_t type, void *object, pw_destroy_t destroy)
{
	struct impl *impl;
	struct pw_resource *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	this->core = client->core;
	this->client = client;
	this->type = type;
	this->object = object;
	this->destroy = destroy;

	pw_signal_init(&this->destroy_signal);

	if (id == SPA_ID_INVALID) {
		this->id = pw_map_insert_new(&client->objects, this);
	} else if (!pw_map_insert_at(&client->objects, id, this))
		goto in_use;

	this->id = id;

	pw_log_debug("resource %p: new for client %p id %u", this, client, this->id);
	pw_signal_emit(&client->resource_added, client, this);

	return this;

      in_use:
	pw_log_debug("resource %p: id %u in use for client %p", this, id, client);
	free(impl);
	return NULL;
}

void pw_resource_destroy(struct pw_resource *resource)
{
	struct pw_client *client = resource->client;

	pw_log_trace("resource %p: destroy %u", resource, resource->id);
	pw_signal_emit(&resource->destroy_signal, resource);

	pw_map_insert_at(&client->objects, resource->id, NULL);
	pw_signal_emit(&client->resource_removed, client, resource);

	if (resource->destroy)
		resource->destroy(resource);

	if (client->core_resource)
		pw_core_notify_remove_id(client->core_resource, resource->id);

	free(resource);
}
