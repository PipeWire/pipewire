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

#include "session.h"
#include "client-session.h"

#define NAME "session"

struct resource_data {
	struct session *session;
	struct spa_hook object_listener;
	uint32_t n_subscribe_ids;
	uint32_t subscribe_ids[32];
};

#define pw_session_resource(r,m,v,...)	\
	pw_resource_call(r,struct pw_session_events,m,v,__VA_ARGS__)
#define pw_session_resource_info(r,...)	\
	pw_session_resource(r,info,0,__VA_ARGS__)
#define pw_session_resource_param(r,...)	\
	pw_session_resource(r,param,0,__VA_ARGS__)

static int session_enum_params (void *object, int seq,
				uint32_t id, uint32_t start, uint32_t num,
				const struct spa_pod *filter)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct session *this = data->session;
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
			pw_session_resource_param(resource, seq, id, index, next, result);
			count++;
		}
		spa_pod_dynamic_builder_clean(&b);

		if (count == num)
			break;
	}
	return 0;
}

static int session_subscribe_params (void *object, uint32_t *ids, uint32_t n_ids)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	uint32_t i;

	n_ids = SPA_MIN(n_ids, SPA_N_ELEMENTS(data->subscribe_ids));
	data->n_subscribe_ids = n_ids;

	for (i = 0; i < n_ids; i++) {
		data->subscribe_ids[i] = ids[i];
		pw_log_debug(NAME" %p: resource %d subscribe param %u",
			data->session, pw_resource_get_id(resource), ids[i]);
		session_enum_params(resource, 1, ids[i], 0, UINT32_MAX, NULL);
	}
	return 0;
}

static int session_set_param (void *object, uint32_t id, uint32_t flags,
				const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct session *this = data->session;

	pw_client_session_resource_set_param(this->client_sess->resource,
						id, flags, param);

	return 0;
}

static const struct pw_session_methods methods = {
	PW_VERSION_SESSION_METHODS,
	.subscribe_params = session_subscribe_params,
	.enum_params = session_enum_params,
	.set_param = session_set_param,
};

struct emit_param_data {
	struct session *this;
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
			pw_session_resource_param(resource, 1,
				d->id, d->index, d->next, d->param);
		}
	}
	return 0;
}

static void session_notify_subscribed(struct session *this,
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
	struct session *this = data;
	pw_session_resource_info(resource, &this->info);
	return 0;
}

int session_update(struct session *this,
			uint32_t change_mask,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct pw_session_info *info)
{
	if (change_mask & PW_CLIENT_SESSION_UPDATE_PARAMS) {
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
			session_notify_subscribed(this, i, i+1);
		}
	}

	if (change_mask & PW_CLIENT_SESSION_UPDATE_INFO) {
		if (info->change_mask & PW_SESSION_CHANGE_MASK_PROPS)
			pw_properties_update(this->props, info->props);

		if (info->change_mask & PW_SESSION_CHANGE_MASK_PARAMS) {
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
		this->info.change_mask = info->change_mask;
		pw_global_for_each_resource(this->global, emit_info, this);
		this->info.change_mask = 0;
	}

	return 0;

      no_mem:
	pw_log_error(NAME" can't update: no memory");
	pw_resource_error(this->client_sess->resource, -ENOMEM,
			NAME" can't update: no memory");
	return -ENOMEM;
}

static int session_bind(void *_data, struct pw_impl_client *client,
			uint32_t permissions, uint32_t version, uint32_t id)
{
	struct session *this = _data;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions,
			pw_global_get_type(global), version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	data->session = this;

	pw_resource_add_object_listener(resource, &data->object_listener,
					&methods, resource);

	pw_log_debug(NAME" %p: bound to %d", this, pw_resource_get_id(resource));
	pw_global_add_resource(global, resource);

	this->info.change_mask = PW_SESSION_CHANGE_MASK_ALL;
	pw_session_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

      no_mem:
	pw_log_error(NAME" can't create resource: no memory");
	pw_resource_error(this->client_sess->resource, -ENOMEM,
			NAME" can't create resource: no memory");
	return -ENOMEM;
}

int session_init(struct session *this,
		struct client_session *client_sess,
		struct pw_context *context,
		struct pw_properties *properties)
{
	static const char * const keys[] = {
		PW_KEY_OBJECT_SERIAL,
		PW_KEY_FACTORY_ID,
		PW_KEY_CLIENT_ID,
		NULL
	};

	pw_log_debug(NAME" %p: new", this);

	this->client_sess = client_sess;
	this->props = properties;

	this->global = pw_global_new (context,
			PW_TYPE_INTERFACE_Session,
			PW_VERSION_SESSION,
			PW_SESSION_PERM_MASK,
			NULL, session_bind, this);
	if (!this->global)
		goto no_mem;

	pw_properties_setf(this->props, PW_KEY_OBJECT_ID, "%u",
			pw_global_get_id(this->global));
	pw_properties_setf(this->props, PW_KEY_OBJECT_SERIAL, "%"PRIu64,
			pw_global_get_serial(this->global));

	this->info.version = PW_VERSION_SESSION_INFO;
	this->info.id = pw_global_get_id(this->global);
	this->info.props = &this->props->dict;

	pw_global_update_keys(this->global, &this->props->dict, keys);

	pw_resource_set_bound_id(client_sess->resource, this->info.id);

	return pw_global_register(this->global);

      no_mem:
	pw_log_error(NAME" - can't create - out of memory");
	return -ENOMEM;
}

void session_clear(struct session *this)
{
	uint32_t i;

	pw_log_debug(NAME" %p: destroy", this);

	pw_global_destroy(this->global);

	for (i = 0; i < this->n_params; i++)
		free(this->params[i]);
	free(this->params);

	free(this->info.params);

	pw_properties_free(this->props);
}
