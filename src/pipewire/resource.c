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
#include "pipewire/type.h"

#include <spa/debug/types.h>

#define NAME "resource"

/** \cond */
struct impl {
	struct pw_resource this;
};
/** \endcond */

SPA_EXPORT
struct pw_resource *pw_resource_new(struct pw_client *client,
				    uint32_t id,
				    uint32_t permissions,
				    uint32_t type,
				    uint32_t version,
				    size_t user_data_size)
{
	struct impl *impl;
	struct pw_resource *this;
	int res;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	this->core = client->core;
	this->client = client;
	this->permissions = permissions;
	this->type = type;
	this->version = version;

	spa_hook_list_init(&this->listener_list);
	spa_hook_list_init(&this->object_listener_list);

	this->marshal = pw_protocol_get_marshal(client->protocol, type);
	if (this->marshal == NULL) {
		pw_log_error(NAME" %p: no marshal for type %s/%d", this,
				spa_debug_type_find_name(pw_type_info(), type),
				version);
		res = -EPROTO;
		goto error_clean;
	}

	if (id == SPA_ID_INVALID) {
		res = -EINVAL;
		goto error_clean;
	}

	if ((res = pw_map_insert_at(&client->objects, id, this)) < 0) {
		res = -errno;
		pw_log_error(NAME" %p: can't add id %u for client %p: %m",
			this, id, client);
		goto error_clean;
	}

	this->id = id;

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	this->impl = SPA_INTERFACE_INIT(
			type,
			this->marshal->version,
			this->marshal->event_marshal, this);

	pw_log_debug(NAME" %p: new %u %s/%d client %p marshal %p",
			this, id,
			spa_debug_type_find_name(pw_type_info(), type), version,
			client, this->marshal);
	pw_client_emit_resource_added(client, this);

	return this;

error_clean:
	free(impl);
	errno = -res;
	return NULL;
}

SPA_EXPORT
struct pw_client *pw_resource_get_client(struct pw_resource *resource)
{
	return resource->client;
}

SPA_EXPORT
uint32_t pw_resource_get_id(struct pw_resource *resource)
{
	return resource->id;
}

SPA_EXPORT
uint32_t pw_resource_get_permissions(struct pw_resource *resource)
{
	return resource->permissions;
}

SPA_EXPORT
uint32_t pw_resource_get_type(struct pw_resource *resource)
{
	return resource->type;
}

SPA_EXPORT
struct pw_protocol *pw_resource_get_protocol(struct pw_resource *resource)
{
	return resource->client->protocol;
}

SPA_EXPORT
void *pw_resource_get_user_data(struct pw_resource *resource)
{
	return resource->user_data;
}

SPA_EXPORT
void pw_resource_add_listener(struct pw_resource *resource,
			      struct spa_hook *listener,
			      const struct pw_resource_events *events,
			      void *data)
{
	spa_hook_list_append(&resource->listener_list, listener, events, data);
}

SPA_EXPORT
void pw_resource_add_object_listener(struct pw_resource *resource,
				struct spa_hook *listener,
				const void *funcs,
				void *data)
{
	spa_hook_list_append(&resource->object_listener_list, listener, funcs, data);
}

SPA_EXPORT
struct spa_hook_list *pw_resource_get_object_listeners(struct pw_resource *resource)
{
	return &resource->object_listener_list;
}

SPA_EXPORT
const struct pw_protocol_marshal *pw_resource_get_marshal(struct pw_resource *resource)
{
	return resource->marshal;
}

SPA_EXPORT
int pw_resource_ping(struct pw_resource *resource, int seq)
{
	int res = -EIO;
	struct pw_client *client = resource->client;

	if (client->core_resource != NULL) {
		pw_core_resource_ping(client->core_resource, resource->id, seq);
		res = client->send_seq;
		pw_log_debug(NAME" %p: %u seq:%d ping %d", resource, resource->id, seq, res);
	}
	return res;
}

SPA_EXPORT
void pw_resource_error(struct pw_resource *resource, int res, const char *error, ...)
{
	va_list ap;
	struct pw_client *client = resource->client;

	va_start(ap, error);
	if (client->core_resource != NULL)
		pw_core_resource_errorv(client->core_resource,
				resource->id, client->recv_seq, res, error, ap);
	va_end(ap);
}

SPA_EXPORT
void pw_resource_destroy(struct pw_resource *resource)
{
	struct pw_client *client = resource->client;

	pw_log_debug(NAME" %p: destroy %u", resource, resource->id);
	pw_resource_emit_destroy(resource);

	pw_map_insert_at(&client->objects, resource->id, NULL);
	pw_client_emit_resource_removed(client, resource);

	if (client->core_resource && !resource->removed)
		pw_core_resource_remove_id(client->core_resource, resource->id);

	pw_log_debug(NAME" %p: free %u", resource, resource->id);

	free(resource);
}
