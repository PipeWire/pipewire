/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <assert.h>

#include "pipewire/private.h"
#include "pipewire/protocol.h"
#include "pipewire/resource.h"
#include "pipewire/type.h"

#include <spa/debug/types.h>

PW_LOG_TOPIC_EXTERN(log_resource);
#define PW_LOG_TOPIC_DEFAULT log_resource

/** \cond */
struct impl {
	struct pw_resource this;
};
/** \endcond */

SPA_EXPORT
struct pw_resource *pw_resource_new(struct pw_impl_client *client,
				    uint32_t id,
				    uint32_t permissions,
				    const char *type,
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
	this->refcount = 1;
	this->context = client->context;
	this->client = client;
	this->permissions = permissions;
	this->type = type;
	this->version = version;
	this->bound_id = SPA_ID_INVALID;

	spa_hook_list_init(&this->listener_list);
	spa_hook_list_init(&this->object_listener_list);

	if (id == SPA_ID_INVALID) {
		res = -EINVAL;
		goto error_clean;
	}

	if ((res = pw_map_insert_at(&client->objects, id, this)) < 0) {
		pw_log_error("%p: can't add id %u for client %p: %s",
			this, id, client, spa_strerror(res));
		goto error_clean;
	}
	this->id = id;

	if ((res = pw_resource_install_marshal(this, false)) < 0) {
		pw_log_error("%p: no marshal for type %s/%d: %s", this,
				type, version, spa_strerror(res));
		goto error_clean;
	}


	if (user_data_size > 0)
		this->user_data = SPA_PTROFF(impl, sizeof(struct impl), void);

	pw_log_debug("%p: new %u type %s/%d client:%p marshal:%p",
			this, id, type, version, client, this->marshal);

	pw_impl_client_emit_resource_added(client, this);

	return this;

error_clean:
	free(impl);
	errno = -res;
	return NULL;
}

SPA_EXPORT
int pw_resource_install_marshal(struct pw_resource *this, bool implementor)
{
	struct pw_impl_client *client = this->client;
	const struct pw_protocol_marshal *marshal;

	marshal = pw_protocol_get_marshal(client->protocol,
			this->type, this->version,
			implementor ? PW_PROTOCOL_MARSHAL_FLAG_IMPL : 0);
	if (marshal == NULL)
		return -EPROTO;

	this->marshal = marshal;
	this->type = marshal->type;

	this->impl = SPA_INTERFACE_INIT(
			this->type,
			this->marshal->version,
			this->marshal->server_marshal, this);
	return 0;
}

SPA_EXPORT
struct pw_impl_client *pw_resource_get_client(struct pw_resource *resource)
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
const char *pw_resource_get_type(struct pw_resource *resource, uint32_t *version)
{
	if (version)
		*version = resource->version;
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
	struct pw_impl_client *client = resource->client;

	if (client->core_resource != NULL) {
		pw_core_resource_ping(client->core_resource, resource->id, seq);
		res = client->send_seq;
		pw_log_debug("%p: %u seq:%d ping %d", resource, resource->id, seq, res);
	}
	return res;
}

SPA_EXPORT
int pw_resource_set_bound_id(struct pw_resource *resource, uint32_t global_id)
{
	struct pw_impl_client *client = resource->client;

	resource->bound_id = global_id;

	if (client->core_resource != NULL) {
		struct pw_global *global = pw_map_lookup(&resource->context->globals, global_id);
		const struct spa_dict *dict = global ? &global->properties->dict : NULL;

		pw_log_debug("%p: %u global_id:%u %d", resource, resource->id, global_id,
				client->core_resource->version);

		if (client->core_resource->version >= 4)
			pw_core_resource_bound_props(client->core_resource, resource->id, global_id,
					dict);
		else
			pw_core_resource_bound_id(client->core_resource, resource->id, global_id);
	}
	return 0;
}

SPA_EXPORT
uint32_t pw_resource_get_bound_id(struct pw_resource *resource)
{
	return resource->bound_id;
}

static void SPA_PRINTF_FUNC(4, 0)
pw_resource_errorv_id(struct pw_resource *resource, uint32_t id, int res, const char *error, va_list ap)
{
	struct pw_impl_client *client;

	if (resource) {
		client = resource->client;
		if (client->core_resource != NULL)
			pw_core_resource_errorv(client->core_resource,
					id, client->recv_seq, res, error, ap);
	} else {
		pw_logtv(SPA_LOG_LEVEL_ERROR, PW_LOG_TOPIC_DEFAULT, error, ap);
	}
}

SPA_EXPORT
void pw_resource_errorf(struct pw_resource *resource, int res, const char *error, ...)
{
	va_list ap;
	va_start(ap, error);
	if (resource)
		pw_resource_errorv_id(resource, resource->id, res, error, ap);
	else
		pw_logtv(SPA_LOG_LEVEL_ERROR, PW_LOG_TOPIC_DEFAULT, error, ap);
	va_end(ap);
}

SPA_EXPORT
void pw_resource_errorf_id(struct pw_resource *resource, uint32_t id, int res, const char *error, ...)
{
	va_list ap;
	va_start(ap, error);
	if (resource)
		pw_resource_errorv_id(resource, id, res, error, ap);
	else
		pw_logtv(SPA_LOG_LEVEL_ERROR, PW_LOG_TOPIC_DEFAULT, error, ap);
	va_end(ap);
}

SPA_EXPORT
void pw_resource_error(struct pw_resource *resource, int res, const char *error)
{
	struct pw_impl_client *client;
	if (resource) {
		client = resource->client;
		if (client->core_resource != NULL)
			pw_core_resource_error(client->core_resource,
					resource->id, client->recv_seq, res, error);
	} else {
		pw_log_error("%s: %s", error, spa_strerror(res));
	}
}

SPA_EXPORT
void pw_resource_ref(struct pw_resource *resource)
{
	assert(resource->refcount > 0);
	resource->refcount++;
}

SPA_EXPORT
void pw_resource_unref(struct pw_resource *resource)
{
	assert(resource->refcount > 0);
	if (--resource->refcount > 0)
		return;

	pw_log_debug("%p: free %u", resource, resource->id);
	assert(resource->destroyed);

#if DEBUG_LISTENERS
	{
		struct spa_hook *h;
		spa_list_for_each(h, &resource->object_listener_list.list, link) {
			pw_log_warn("%p: resource %u: leaked object listener %p",
					resource, resource->id, h);
			break;
		}
		spa_list_for_each(h, &resource->listener_list.list, link) {
			pw_log_warn("%p: resource %u: leaked listener %p",
					resource, resource->id, h);
			break;
		}
	}
#endif
	spa_hook_list_clean(&resource->listener_list);
	spa_hook_list_clean(&resource->object_listener_list);

	free(resource);
}

SPA_EXPORT
void pw_resource_destroy(struct pw_resource *resource)
{
	struct pw_impl_client *client = resource->client;

	pw_log_debug("%p: destroy %u", resource, resource->id);
	assert(!resource->destroyed);
	resource->destroyed = true;

	if (resource->global) {
		spa_list_remove(&resource->link);
		resource->global = NULL;
	}

	pw_resource_emit_destroy(resource);

	pw_map_insert_at(&client->objects, resource->id, NULL);
	pw_impl_client_emit_resource_removed(client, resource);

	if (client->core_resource && !resource->removed)
		pw_core_resource_remove_id(client->core_resource, resource->id);

	pw_resource_unref(resource);
}

SPA_EXPORT
void pw_resource_remove(struct pw_resource *resource)
{
	resource->removed = true;
	pw_resource_destroy(resource);
}
