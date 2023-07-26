/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Collabora Ltd. */
/*                         @author George Kiagiadakis <george.kiagiadakis@collabora.com> */
/* SPDX-License-Identifier: MIT */

#include <stdbool.h>
#include <string.h>

#include <pipewire/impl.h>
#include <pipewire/extensions/session-manager.h>

#include <spa/pod/filter.h>
#include <spa/pod/dynamic.h>

#include "endpoint-stream.h"
#include "client-endpoint.h"

#define NAME "endpoint-stream"

struct resource_data {
	struct endpoint_stream *stream;
	struct spa_hook object_listener;
	uint32_t n_subscribe_ids;
	uint32_t subscribe_ids[32];
};

#define pw_endpoint_stream_resource(r,m,v,...)	\
	pw_resource_call(r,struct pw_endpoint_stream_events,m,v,__VA_ARGS__)
#define pw_endpoint_stream_resource_info(r,...)	\
	pw_endpoint_stream_resource(r,info,0,__VA_ARGS__)
#define pw_endpoint_stream_resource_param(r,...)	\
	pw_endpoint_stream_resource(r,param,0,__VA_ARGS__)

static int endpoint_stream_enum_params (void *object, int seq,
				uint32_t id, uint32_t start, uint32_t num,
				const struct spa_pod *filter)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct endpoint_stream *this = data->stream;
	struct spa_pod *result;
	struct spa_pod *param;
	uint8_t buffer[2048];
	struct spa_pod_dynamic_builder b = { 0 };
	uint32_t index;
	uint32_t next = start;
	uint32_t count = 0;

	while (true) {
		index = next++;
		if (index >= this->n_params)
			break;

		param = this->params[index];

		if (param == NULL || !spa_pod_is_object_id(param, id))
			continue;

		spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 4096);
		if (spa_pod_filter(&b.b, &result, param, filter) == 0) {
			pw_log_debug(NAME" %p: %d param %u", this, seq, index);
			pw_endpoint_stream_resource_param(resource, seq, id, index, next, result);
			count++;
		}
		spa_pod_dynamic_builder_clean(&b);

		if (count == num)
			break;
	}
	return 0;
}

static int endpoint_stream_subscribe_params (void *object, uint32_t *ids, uint32_t n_ids)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	uint32_t i;

	n_ids = SPA_MIN(n_ids, SPA_N_ELEMENTS(data->subscribe_ids));
	data->n_subscribe_ids = n_ids;

	for (i = 0; i < n_ids; i++) {
		data->subscribe_ids[i] = ids[i];
		pw_log_debug(NAME" %p: resource %d subscribe param %u",
			data->stream, pw_resource_get_id(resource), ids[i]);
		endpoint_stream_enum_params(resource, 1, ids[i], 0, UINT32_MAX, NULL);
	}
	return 0;
}

static int endpoint_stream_set_param (void *object, uint32_t id, uint32_t flags,
				const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct endpoint_stream *this = data->stream;

	pw_client_endpoint_resource_set_param(this->client_ep->resource,
						id, flags, param);

	return 0;
}

static const struct pw_endpoint_stream_methods methods = {
	PW_VERSION_ENDPOINT_STREAM_METHODS,
	.subscribe_params = endpoint_stream_subscribe_params,
	.enum_params = endpoint_stream_enum_params,
	.set_param = endpoint_stream_set_param,
};

struct emit_param_data {
	struct endpoint_stream *this;
	struct spa_pod *param;
	uint32_t id;
	uint32_t index;
	uint32_t next;
};

static int emit_param(void *_data, struct pw_resource *resource)
{
	struct emit_param_data *d = _data;
	struct resource_data *data;
	uint32_t i;

	data = pw_resource_get_user_data(resource);
	for (i = 0; i < data->n_subscribe_ids; i++) {
		if (data->subscribe_ids[i] == d->id) {
			pw_endpoint_stream_resource_param(resource, 1,
				d->id, d->index, d->next, d->param);
		}
	}
	return 0;
}

static void endpoint_stream_notify_subscribed(struct endpoint_stream *this,
					uint32_t index, uint32_t next)
{
	struct pw_global *global = this->global;
	struct emit_param_data data;
	struct spa_pod *param = this->params[index];

	if (!param || !spa_pod_is_object (param))
		return;

	data.this = this;
	data.param = param;
	data.id = SPA_POD_OBJECT_ID (param);
	data.index = index;
	data.next = next;

	pw_global_for_each_resource(global, emit_param, &data);
}

static int emit_info(void *data, struct pw_resource *resource)
{
	struct endpoint_stream *this = data;
	pw_endpoint_stream_resource_info(resource, &this->info);
	return 0;
}

int endpoint_stream_update(struct endpoint_stream *this,
			uint32_t change_mask,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct pw_endpoint_stream_info *info)
{
	if (change_mask & PW_CLIENT_ENDPOINT_UPDATE_PARAMS) {
		uint32_t i;

		pw_log_debug(NAME" %p: update %d params", this, n_params);

		for (i = 0; i < this->n_params; i++)
			free(this->params[i]);
		this->n_params = n_params;
		if (this->n_params == 0) {
			free(this->params);
			this->params = NULL;
		} else {
			void *p;
			p = pw_reallocarray(this->params, n_params, sizeof(struct spa_pod*));
			if (p == NULL) {
				free(this->params);
				this->params = NULL;
				this->n_params = 0;
				goto no_mem;
			}
			this->params = p;
		}
		for (i = 0; i < this->n_params; i++) {
			this->params[i] = params[i] ? spa_pod_copy(params[i]) : NULL;
			endpoint_stream_notify_subscribed(this, i, i+1);
		}
	}

	if (change_mask & PW_CLIENT_ENDPOINT_UPDATE_INFO) {
		if (info->change_mask & PW_ENDPOINT_STREAM_CHANGE_MASK_LINK_PARAMS) {
			free(this->info.link_params);
			this->info.link_params = spa_pod_copy(info->link_params);
		}

		if (info->change_mask & PW_ENDPOINT_STREAM_CHANGE_MASK_PROPS)
			pw_properties_update(this->props, info->props);

		if (info->change_mask & PW_ENDPOINT_STREAM_CHANGE_MASK_PARAMS) {
			this->info.n_params = info->n_params;
			if (info->n_params == 0) {
				free(this->info.params);
				this->info.params = NULL;
			} else {
				void *p;
				p = pw_reallocarray(this->info.params, info->n_params, sizeof(struct spa_param_info));
				if (p == NULL) {
					free(this->info.params);
					this->info.params = NULL;
					this->info.n_params = 0;
					goto no_mem;
				}
				this->info.params = p;
				memcpy(this->info.params, info->params, info->n_params * sizeof(struct spa_param_info));
			}
		}

		if (!this->info.name)
			this->info.name = info->name ? strdup(info->name) : NULL;

		this->info.change_mask = info->change_mask;
		pw_global_for_each_resource(this->global, emit_info, this);
		this->info.change_mask = 0;
	}

	return 0;

      no_mem:
	pw_log_error(NAME" can't update: no memory");
	pw_resource_error(this->client_ep->resource, -ENOMEM,
			NAME" can't update: no memory");
	return -ENOMEM;
}

static int endpoint_stream_bind(void *_data, struct pw_impl_client *client,
			uint32_t permissions, uint32_t version, uint32_t id)
{
	struct endpoint_stream *this = _data;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions,
			pw_global_get_type(global), version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	data->stream = this;
	pw_resource_add_object_listener(resource, &data->object_listener,
					&methods, resource);

	pw_log_debug(NAME" %p: bound to %d", this, pw_resource_get_id(resource));
	pw_global_add_resource(global, resource);

	this->info.change_mask = PW_ENDPOINT_STREAM_CHANGE_MASK_ALL;
	pw_endpoint_stream_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

      no_mem:
	pw_log_error(NAME" can't create resource: no memory");
	pw_resource_error(this->client_ep->resource, -ENOMEM,
			NAME" can't create resource: no memory");
	return -ENOMEM;
}

int endpoint_stream_init(struct endpoint_stream *this,
		uint32_t id, uint32_t endpoint_id,
		struct client_endpoint *client_ep,
		struct pw_context *context,
		struct pw_properties *properties)
{
	pw_log_debug(NAME" %p: new", this);

	this->client_ep = client_ep;
	this->id = id;
	this->props = properties;

	pw_properties_setf(properties, PW_KEY_ENDPOINT_ID, "%u", endpoint_id);

	properties = pw_properties_copy(properties);
	if (!properties)
		goto no_mem;

	this->global = pw_global_new (context,
			PW_TYPE_INTERFACE_EndpointStream,
			PW_VERSION_ENDPOINT_STREAM,
			PW_ENDPOINT_STREAM_PERM_MASK,
			properties, endpoint_stream_bind, this);
	if (!this->global)
		goto no_mem;

	pw_properties_setf(this->props, PW_KEY_OBJECT_ID, "%u",
			pw_global_get_id(this->global));
	pw_properties_setf(this->props, PW_KEY_OBJECT_SERIAL, "%"PRIu64,
			pw_global_get_serial(this->global));

	this->info.version = PW_VERSION_ENDPOINT_STREAM_INFO;
	this->info.id = pw_global_get_id(this->global);
	this->info.endpoint_id = endpoint_id;
	this->info.props = &this->props->dict;

	return pw_global_register(this->global);

      no_mem:
	pw_log_error(NAME" - can't create - out of memory");
	return -ENOMEM;
}

void endpoint_stream_clear(struct endpoint_stream *this)
{
	uint32_t i;

	pw_log_debug(NAME" %p: destroy", this);

	pw_global_destroy(this->global);

	for (i = 0; i < this->n_params; i++)
		free(this->params[i]);
	free(this->params);

	free(this->info.name);
	free(this->info.link_params);
	free(this->info.params);

	pw_properties_free(this->props);
}
