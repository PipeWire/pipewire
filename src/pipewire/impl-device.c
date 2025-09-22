/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <spa/debug/types.h>
#include <spa/monitor/utils.h>
#include <spa/pod/filter.h>
#include <spa/pod/dynamic.h>
#include <spa/utils/string.h>
#include <spa/utils/json-pod.h>

#define PW_API_DEVICE_IMPL SPA_EXPORT
#include "pipewire/impl.h"
#include "pipewire/private.h"

PW_LOG_TOPIC_EXTERN(log_device);
#define PW_LOG_TOPIC_DEFAULT log_device

struct impl {
	struct pw_impl_device this;

	struct spa_list param_list;
	struct spa_list pending_list;

	unsigned int cache_params:1;
};

static const char * const global_keys[] = {
	PW_KEY_OBJECT_PATH,
	PW_KEY_MODULE_ID,
	PW_KEY_FACTORY_ID,
	PW_KEY_CLIENT_ID,
	PW_KEY_DEVICE_API,
	PW_KEY_DEVICE_DESCRIPTION,
	PW_KEY_DEVICE_NAME,
	PW_KEY_DEVICE_NICK,
	PW_KEY_MEDIA_CLASS,
	NULL
};

#define pw_device_resource(r,m,v,...)	pw_resource_call(r,struct pw_device_events,m,v,__VA_ARGS__)
#define pw_device_resource_info(r,...)	pw_device_resource(r,info,0,__VA_ARGS__)
#define pw_device_resource_param(r,...) pw_device_resource(r,param,0,__VA_ARGS__)

struct result_device_params_data {
	struct impl *impl;
	void *data;
	int (*callback) (void *data, int seq,
			uint32_t id, uint32_t index, uint32_t next,
			struct spa_pod *param);
	int seq;
	uint32_t count;
	unsigned int cache:1;
};

struct resource_data {
	struct pw_impl_device *device;
	struct pw_resource *resource;

	struct spa_hook resource_listener;
	struct spa_hook object_listener;

	uint32_t subscribe_ids[MAX_PARAMS];
	uint32_t n_subscribe_ids;

	/* for async replies */
	int seq;
	int orig_seq;
	int end;
	struct spa_param_info *pi;
	struct result_device_params_data data;
	struct spa_hook listener;
};

struct object_data {
	struct spa_list link;
	uint32_t id;
#define OBJECT_NODE	0
#define OBJECT_DEVICE	1
	uint32_t type;
	struct spa_handle *handle;
	struct spa_handle *subhandle;
	void *object;
	struct spa_hook listener;
};

static void object_destroy(struct object_data *od)
{
	switch (od->type) {
	case OBJECT_NODE:
		pw_impl_node_destroy(od->object);
		break;
	case OBJECT_DEVICE:
		pw_impl_device_destroy(od->object);
		break;
	}
}

static void object_update(struct object_data *od, const struct spa_dict *props)
{
	switch (od->type) {
	case OBJECT_NODE:
		pw_impl_node_update_properties(od->object, props);
		break;
	case OBJECT_DEVICE:
		pw_impl_device_update_properties(od->object, props);
		break;
	}
}

static void object_register(struct object_data *od, uint32_t device_id)
{
	char id[64];
	struct spa_dict_item it[1];
	snprintf(id, sizeof(id), "%u", device_id);

	it[0] = SPA_DICT_ITEM_INIT(PW_KEY_DEVICE_ID, id);
	switch (od->type) {
	case OBJECT_NODE:
		pw_impl_node_update_properties(od->object, &SPA_DICT_INIT_ARRAY(it));
		pw_impl_node_register(od->object, NULL);
		pw_impl_node_set_active(od->object, true);
		break;
	case OBJECT_DEVICE:
		pw_impl_device_update_properties(od->object, &SPA_DICT_INIT_ARRAY(it));
		pw_impl_device_register(od->object, NULL);
		break;
	}
}

struct match {
	struct pw_impl_device *device;
	int count;
};
#define MATCH_INIT(n) ((struct match){ .device = (n) })

static int execute_match(void *data, const char *location, const char *action,
		const char *val, size_t len)
{
	struct match *match = data;
	struct pw_impl_device *this = match->device;
	if (spa_streq(action, "update-props")) {
		match->count += pw_properties_update_string(this->properties, val, len);
	}
	return 1;
}

static void check_properties(struct pw_impl_device *device)
{
	struct pw_context *context = device->context;
	const char *str;
	struct match match;

	match = MATCH_INIT(device);
	pw_context_conf_section_match_rules(context, "device.rules",
			&device->properties->dict, execute_match, &match);

	if ((str = pw_properties_get(device->properties, PW_KEY_DEVICE_NAME)) &&
	    (device->name == NULL || !spa_streq(str, device->name))) {
		free(device->name);
		device->name = strdup(str);
		pw_log_debug("%p: name '%s'", device, device->name);
	}
}

SPA_EXPORT
struct pw_impl_device *pw_context_create_device(struct pw_context *context,
				struct pw_properties *properties,
				size_t user_data_size)
{
	struct impl *impl;
	struct pw_impl_device *this;
	int res;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL) {
		res = -errno;
		goto error_cleanup;
	}
	spa_list_init(&impl->param_list);
	spa_list_init(&impl->pending_list);
	impl->cache_params = true;

	this = &impl->this;
	this->name = strdup("device");
	pw_log_debug("%p: new", this);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL) {
		res = -errno;
		goto error_free;
	}

	this->context = context;
	this->properties = properties;
	this->info.props = &properties->dict;
	this->info.params = this->params;
	spa_hook_list_init(&this->listener_list);

	spa_list_init(&this->object_list);

	if (user_data_size > 0)
		this->user_data = SPA_PTROFF(this, sizeof(struct impl), void);

	check_properties(this);

	return this;

error_free:
	free(impl);
error_cleanup:
	pw_properties_free(properties);
	errno = -res;
	return NULL;
}

SPA_EXPORT
void pw_impl_device_destroy(struct pw_impl_device *device)
{
	struct impl *impl = SPA_CONTAINER_OF(device, struct impl, this);
	struct object_data *od;

	pw_log_debug("%p: destroy", device);
	pw_impl_device_emit_destroy(device);

	spa_list_consume(od, &device->object_list, link)
		object_destroy(od);

	if (device->registered)
		spa_list_remove(&device->link);

	if (device->device)
		spa_hook_remove(&device->listener);

	if (device->global) {
		spa_hook_remove(&device->global_listener);
		pw_global_destroy(device->global);
	}
	pw_log_debug("%p: free", device);
	pw_impl_device_emit_free(device);

	pw_param_clear(&impl->param_list, SPA_ID_INVALID);
	pw_param_clear(&impl->pending_list, SPA_ID_INVALID);

	spa_hook_list_clean(&device->listener_list);

	pw_properties_free(device->properties);
	free(device->name);

	free(device);
}

static void remove_busy_resource(struct resource_data *d)
{
	struct pw_impl_device *device = d->device;
	struct impl *impl = SPA_CONTAINER_OF(device, struct impl, this);

	if (d->end != -1) {
		if (d->pi && d->data.cache) {
			pw_param_update(&impl->param_list, &impl->pending_list, 0, NULL);
			d->pi->user = 1;
			d->pi = NULL;
		}
		spa_hook_remove(&d->listener);
		d->end = -1;
		pw_impl_client_set_busy(d->resource->client, false);
	}
}

static void resource_destroy(void *data)
{
	struct resource_data *d = data;
	remove_busy_resource(d);
	spa_hook_remove(&d->resource_listener);
	spa_hook_remove(&d->object_listener);
}

static void resource_pong(void *data, int seq)
{
	struct resource_data *d = data;
	struct pw_resource *resource = d->resource;
	pw_log_debug("%p: resource %p: got pong %d", d->device,
			resource, seq);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = resource_destroy,
	.pong = resource_pong,
};

static void result_device_params(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct result_device_params_data *d = data;
	struct impl *impl = d->impl;
	pw_log_debug("%p: type %d", impl, type);

	switch (type) {
	case SPA_RESULT_TYPE_DEVICE_PARAMS:
	{
		const struct spa_result_device_params *r = result;
		d->callback(d->data, seq, r->id, r->index, r->next, r->param);
		if (d->cache) {
			pw_log_debug("%p: add param %d", impl, r->id);
			if (d->count++ == 0)
				pw_param_add(&impl->pending_list, seq, r->id, NULL);
			pw_param_add(&impl->pending_list, seq, r->id, r->param);
		}
		break;
	}
	default:
		break;
	}
}

SPA_EXPORT
int pw_impl_device_for_each_param(struct pw_impl_device *device,
			     int seq, uint32_t param_id,
			     uint32_t index, uint32_t max,
			     const struct spa_pod *filter,
			     int (*callback) (void *data, int seq,
					      uint32_t id, uint32_t index, uint32_t next,
					      struct spa_pod *param),
			     void *data)
{
	int res;
	struct impl *impl = SPA_CONTAINER_OF(device, struct impl, this);
	struct result_device_params_data user_data = { impl, data, callback, seq, 0, false };
	struct spa_hook listener;
	struct spa_param_info *pi;
	static const struct spa_device_events device_events = {
		SPA_VERSION_DEVICE_EVENTS,
		.result = result_device_params,
	};

	pi = pw_param_info_find(device->info.params, device->info.n_params, param_id);
	if (pi == NULL)
		return -ENOENT;

	if (max == 0)
		max = UINT32_MAX;

	pw_log_debug("%p: params id:%d (%s) index:%u max:%u cached:%d", device, param_id,
			spa_debug_type_find_name(spa_type_param, param_id),
			index, max, pi->user);

	if (pi->user == 1) {
		struct pw_param *p;
		uint8_t buffer[4096];
		struct spa_pod_dynamic_builder b;
	        struct spa_result_device_params result;
		uint32_t count = 0;

		result.id = param_id;
		result.next = 0;

		spa_list_for_each(p, &impl->param_list, link) {
			if (p->id != param_id)
				continue;

			result.index = result.next++;
			if (result.index < index)
				continue;

			spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 4096);
			if (spa_pod_filter(&b.b, &result.param, p->param, filter) == 0) {
				pw_log_debug("%p: %d param %u", device, seq, result.index);
				result_device_params(&user_data, seq, 0, SPA_RESULT_TYPE_DEVICE_PARAMS, &result);
				count++;
			}
			spa_pod_dynamic_builder_clean(&b);

			if (count == max)
				break;
		}
		res = 0;
	} else {
		user_data.cache = impl->cache_params &&
			(filter == NULL && index == 0 && max == UINT32_MAX);

		spa_zero(listener);
		spa_device_add_listener(device->device, &listener,
				&device_events, &user_data);
		res = spa_device_enum_params(device->device, seq,
				param_id, index, max, filter);
		spa_hook_remove(&listener);

		if (!SPA_RESULT_IS_ASYNC(res) && user_data.cache) {
			pw_param_update(&impl->param_list, &impl->pending_list, 0, NULL);
			pi->user = 1;
		}
	}

	return res;
}

SPA_EXPORT
int pw_impl_device_set_param(struct pw_impl_device *device,
		uint32_t id, uint32_t flags, const struct spa_pod *param)
{
	pw_log_debug("%p: set_param id:%d (%s) flags:%08x param:%p", device, id,
			spa_debug_type_find_name(spa_type_param, id), flags, param);
	return spa_device_set_param(device->device, id, flags, param);
}


static int reply_param(void *data, int seq, uint32_t id,
		uint32_t index, uint32_t next, struct spa_pod *param)
{
	struct resource_data *d = data;
	pw_device_resource_param(d->resource, seq, id, index, next, param);
	return 0;
}

static void result_device_params_async(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct resource_data *d = data;

	pw_log_debug("%p: async result %d %d (%d/%d)", d->device,
			res, seq, d->seq, d->end);

	if (seq == d->seq)
		result_device_params(&d->data, d->orig_seq, res, type, result);
	if (seq == d->end)
		remove_busy_resource(d);
}

static int device_enum_params(void *object, int seq, uint32_t id, uint32_t start, uint32_t num,
		const struct spa_pod *filter)
{
	struct resource_data *data = object;
	struct pw_resource *resource = data->resource;
	struct pw_impl_device *device = data->device;
	struct impl *impl = SPA_CONTAINER_OF(device, struct impl, this);
	struct pw_impl_client *client = resource->client;
	int res;
	static const struct spa_device_events device_events = {
		SPA_VERSION_DEVICE_EVENTS,
		.result = result_device_params_async,
	};

	res = pw_impl_device_for_each_param(device, seq, id, start, num,
				filter, reply_param, data);

	if (res < 0) {
		pw_resource_errorf(resource, res,
				"enum params id:%d (%s) failed", id,
				spa_debug_type_find_name(spa_type_param, id));
	} else if (SPA_RESULT_IS_ASYNC(res)) {
		pw_impl_client_set_busy(client, true);
		data->data.impl = impl;
		data->data.data = data;
		data->data.callback = reply_param;
		data->data.count = 0;
		data->data.cache = impl->cache_params &&
			(filter == NULL && start == 0);
		if (data->end == -1)
			spa_device_add_listener(device->device, &data->listener,
				&device_events, data);
		data->pi = pw_param_info_find(device->info.params,
				device->info.n_params, id);
		data->orig_seq = seq;
		data->seq = res;
		data->end = spa_device_sync(device->device, res);
	}

	return res;
}

static int device_subscribe_params(void *object, uint32_t *ids, uint32_t n_ids)
{
	struct resource_data *data = object;
	struct pw_resource *resource = data->resource;
	uint32_t i;

	n_ids = SPA_MIN(n_ids, SPA_N_ELEMENTS(data->subscribe_ids));
	data->n_subscribe_ids = n_ids;

	for (i = 0; i < n_ids; i++) {
		data->subscribe_ids[i] = ids[i];
		pw_log_debug("%p: resource %p subscribe param id:%d (%s)",
				data->device, resource, ids[i],
				spa_debug_type_find_name(spa_type_param, ids[i]));
		device_enum_params(data, 1, ids[i], 0, UINT32_MAX, NULL);
	}
	return 0;
}

static void result_device_done(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct resource_data *d = data;

	pw_log_debug("%p: async result %d %d (%d/%d)", d->device,
			res, seq, d->seq, d->end);

	if (seq == d->end)
		remove_busy_resource(d);
}

static int device_set_param(void *object, uint32_t id, uint32_t flags,
                           const struct spa_pod *param)
{
	struct resource_data *data = object;
	struct pw_resource *resource = data->resource;
	struct pw_impl_device *device = data->device;
	struct pw_impl_client *client = resource->client;
	int res;
	static const struct spa_device_events device_events = {
		SPA_VERSION_DEVICE_EVENTS,
		.result = result_device_done,
	};

	if ((res = spa_device_set_param(device->device, id, flags, param)) < 0) {
		pw_resource_errorf(resource, res,
				"set param id:%d (%s) flags:%08x failed", id,
				spa_debug_type_find_name(spa_type_param, id), flags);
	} else if (SPA_RESULT_IS_ASYNC(res)) {
		pw_impl_client_set_busy(client, true);
		data->data.data = data;
		if (data->end == -1)
			spa_device_add_listener(device->device, &data->listener,
				&device_events, data);
		data->seq = res;
		data->end = spa_device_sync(device->device, res);
	}
	return res;
}

static const struct pw_device_methods device_methods = {
	PW_VERSION_DEVICE_METHODS,
	.subscribe_params = device_subscribe_params,
	.enum_params = device_enum_params,
	.set_param = device_set_param
};

static int
global_bind(void *object, struct pw_impl_client *client, uint32_t permissions,
		  uint32_t version, uint32_t id)
{
	struct pw_impl_device *this = object;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto error_resource;

	data = pw_resource_get_user_data(resource);
	data->device = this;
	data->resource = resource;
	data->end = -1;

	pw_resource_add_listener(resource,
			&data->resource_listener,
			&resource_events, data);
	pw_resource_add_object_listener(resource,
			&data->object_listener,
			&device_methods, data);

	pw_log_debug("%p: bound to %d", this, resource->id);
	pw_global_add_resource(global, resource);

	this->info.change_mask = PW_DEVICE_CHANGE_MASK_ALL;
	pw_device_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

error_resource:
	pw_log_error("%p: can't create device resource: %m", this);
	return -errno;
}

static void global_free(void *data)
{
	struct pw_impl_device *device = data;
	spa_hook_remove(&device->global_listener);
	device->global = NULL;
	pw_impl_device_destroy(device);
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.free = global_free,
};

SPA_EXPORT
int pw_impl_device_register(struct pw_impl_device *device,
		       struct pw_properties *properties)
{
	struct pw_context *context = device->context;
	struct object_data *od;

	if (device->registered)
		goto error_existed;

        device->global = pw_global_new(context,
				       PW_TYPE_INTERFACE_Device,
				       PW_VERSION_DEVICE,
				       PW_DEVICE_PERM_MASK,
				       properties,
				       global_bind,
				       device);
	if (device->global == NULL)
		return -errno;

	spa_list_append(&context->device_list, &device->link);
	device->registered = true;

	device->info.id = device->global->id;
	pw_properties_setf(device->properties, PW_KEY_OBJECT_ID, "%d", device->info.id);
	pw_properties_setf(device->properties, PW_KEY_OBJECT_SERIAL, "%"PRIu64,
			pw_global_get_serial(device->global));

	pw_global_update_keys(device->global, device->info.props, global_keys);

	pw_impl_device_emit_initialized(device);

	pw_global_add_listener(device->global, &device->global_listener, &global_events, device);
	pw_global_register(device->global);

	spa_list_for_each(od, &device->object_list, link)
		object_register(od, device->info.id);

	return 0;

error_existed:
	pw_properties_free(properties);
	return -EEXIST;
}

static void on_object_destroy(void *data)
{
	struct object_data *od = data;
	spa_list_remove(&od->link);
}

static void on_object_free(void *data)
{
	struct object_data *od = data;
	pw_unload_spa_handle(od->handle);
	if (od->subhandle)
		pw_unload_spa_handle(od->subhandle);
}

static const struct pw_impl_node_events node_object_events = {
	PW_VERSION_IMPL_NODE_EVENTS,
	.destroy = on_object_destroy,
	.free = on_object_free,
};

static const struct pw_impl_device_events device_object_events = {
	PW_VERSION_IMPL_DEVICE_EVENTS,
	.destroy = on_object_destroy,
	.free = on_object_free,
};

static void emit_info_changed(struct pw_impl_device *device)
{
	struct pw_resource *resource;

	pw_impl_device_emit_info_changed(device, &device->info);

	if (device->global) {
		if (device->info.change_mask & PW_DEVICE_CHANGE_MASK_PROPS)
			pw_global_update_keys(device->global, device->info.props, global_keys);
		spa_list_for_each(resource, &device->global->resource_list, link)
			pw_device_resource_info(resource, &device->info);
	}
	device->info.change_mask = 0;
}

static int update_properties(struct pw_impl_device *device, const struct spa_dict *dict, bool filter)
{
	static const char * const ignored[] = {
		PW_KEY_OBJECT_ID,
		PW_KEY_MODULE_ID,
		PW_KEY_FACTORY_ID,
		PW_KEY_CLIENT_ID,
		NULL
	};

	int changed;

	changed = pw_properties_update_ignore(device->properties, dict, filter ? ignored : NULL);

	pw_log_debug("%p: updated %d properties", device, changed);

	if (changed) {
		check_properties(device);
		device->info.change_mask |= PW_DEVICE_CHANGE_MASK_PROPS;
	}
	return changed;
}

static int resource_is_subscribed(struct pw_resource *resource, uint32_t id)
{
	struct resource_data *data = pw_resource_get_user_data(resource);
	uint32_t i;

	for (i = 0; i < data->n_subscribe_ids; i++) {
		if (data->subscribe_ids[i] == id)
			return 1;
	}
	return 0;
}

static int notify_param(void *data, int seq, uint32_t id,
		uint32_t index, uint32_t next, struct spa_pod *param)
{
	struct pw_impl_device *device = data;
	struct pw_resource *resource;

	spa_list_for_each(resource, &device->global->resource_list, link) {
		if (!resource_is_subscribed(resource, id))
			continue;

		pw_log_debug("%p: resource %p notify param %d", device, resource, id);
		pw_device_resource_param(resource, seq, id, index, next, param);
	}
	return 0;
}

static void emit_params(struct pw_impl_device *device, uint32_t *changed_ids, uint32_t n_changed_ids)
{
	uint32_t i;
	int res;

	if (device->global == NULL)
		return;

	pw_log_debug("%p: emit %d params", device, n_changed_ids);

	for (i = 0; i < n_changed_ids; i++) {
		struct pw_resource *resource;
		int subscribed = 0;

		/* first check if anyone is subscribed */
		spa_list_for_each(resource, &device->global->resource_list, link) {
			if ((subscribed = resource_is_subscribed(resource, changed_ids[i])))
				break;
		}
		if (!subscribed)
			continue;

		if ((res = pw_impl_device_for_each_param(device, 1, changed_ids[i], 0, UINT32_MAX,
					NULL, notify_param, device)) < 0) {
			pw_log_error("%p: error %d (%s)", device, res, spa_strerror(res));
		}
	}
}

static void device_info(void *data, const struct spa_device_info *info)
{
	struct pw_impl_device *device = data;
	uint32_t changed_ids[MAX_PARAMS], n_changed_ids = 0;

	pw_log_debug("%p: flags:%08"PRIx64" change_mask:%08"PRIx64,
			device, info->flags, info->change_mask);

	if (info->change_mask & SPA_DEVICE_CHANGE_MASK_PROPS) {
		update_properties(device, info->props, true);
	}
	if (info->change_mask & SPA_DEVICE_CHANGE_MASK_PARAMS) {
		uint32_t i;

		device->info.change_mask |= PW_DEVICE_CHANGE_MASK_PARAMS;
		device->info.n_params = SPA_MIN(info->n_params, SPA_N_ELEMENTS(device->params));

		for (i = 0; i < device->info.n_params; i++) {
			uint32_t id = info->params[i].id;

			pw_log_debug("%p: param %d id:%d (%s) %08x:%08x", device, i,
					id, spa_debug_type_find_name(spa_type_param, id),
					device->info.params[i].flags, info->params[i].flags);

			device->info.params[i].id = device->params[i].id;
			if (device->info.params[i].flags == info->params[i].flags)
				continue;

			pw_log_debug("%p: update param %d", device, id);
			device->info.params[i] = info->params[i];
			device->info.params[i].user = 0;

			if (info->params[i].flags & SPA_PARAM_INFO_READ)
				changed_ids[n_changed_ids++] = id;
                }
	}
	emit_info_changed(device);

	if (n_changed_ids > 0)
		emit_params(device, changed_ids, n_changed_ids);
}

static void device_add_object(struct pw_impl_device *device, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct pw_context *context = device->context;
	struct spa_handle *handle, *subhandle = NULL;
	spa_autoptr(pw_properties) props = NULL;
	const struct pw_properties *p;
	int res;
	void *iface;
	struct object_data *od = NULL;
	const char *str;

	if (info->factory_name == NULL) {
		pw_log_debug("%p: missing factory name", device);
		return;
	}

	props = pw_properties_new(NULL, NULL);
	if (props == NULL) {
		pw_log_warn("%p: allocation error: %m", device);
		return;
	}
	if (info->props)
		pw_properties_update(props, info->props);
	if ((str = pw_properties_get(device->properties, "device.object.properties")))
		pw_properties_update_string(props, str, strlen(str));

	p = pw_context_get_properties(context);
	pw_properties_set(props, "clock.quantum-limit",
			pw_properties_get(p, "default.clock.quantum-limit"));

	handle = pw_context_load_spa_handle(context, info->factory_name, &props->dict);
	if (handle == NULL) {
		pw_log_warn("%p: can't load handle %s: %m",
				device, info->factory_name);
		goto cleanup;
	}

	if ((res = spa_handle_get_interface(handle, info->type, &iface)) < 0) {
		pw_log_error("%p: can't get %s interface: %s", device, info->type,
				spa_strerror(res));
		goto cleanup;
	}

	if (spa_streq(info->type, SPA_TYPE_INTERFACE_Node)) {
		struct pw_impl_node *node;

		if ((str = pw_properties_get(props, "node.adapter")) != NULL) {
			char name[64];
			snprintf(name, sizeof(name), "%s.follower", str);
	                pw_properties_setf(props, name, "pointer:%p", iface);

			subhandle = handle;

			handle = pw_context_load_spa_handle(context, str, &props->dict);
			if (handle == NULL) {
				pw_log_warn("%p: can't load handle %s: %m", device, str);
				goto cleanup;
			}
			if ((res = spa_handle_get_interface(handle, info->type, &iface)) < 0) {
				pw_log_error("%p: can't get %s interface: %s", device, info->type,
						spa_strerror(res));
				goto cleanup;
			}
		}

		node = pw_context_create_node(context, spa_steal_ptr(props),
				sizeof(struct object_data));
		if (node == NULL)
			goto cleanup;

		od = pw_impl_node_get_user_data(node);
		od->object = node;
		od->type = OBJECT_NODE;
		pw_impl_node_add_listener(node, &od->listener, &node_object_events, od);
		pw_impl_node_set_implementation(node, iface);
	} else if (spa_streq(info->type, SPA_TYPE_INTERFACE_Device)) {
		struct pw_impl_device *dev;
		dev = pw_context_create_device(context, spa_steal_ptr(props),
				sizeof(struct object_data));
		if (dev == NULL)
			goto cleanup;

		od = pw_impl_device_get_user_data(dev);
		od->object = dev;
		od->type = OBJECT_DEVICE;
		pw_impl_device_add_listener(dev, &od->listener, &device_object_events, od);
		pw_impl_device_set_implementation(dev, iface);
	} else {
		pw_log_warn("%p: unknown type %s", device, info->type);
		goto cleanup;
	}
	if (od) {
		od->id = id;
		od->handle = handle;
		od->subhandle = subhandle;
		spa_list_append(&device->object_list, &od->link);
		if (device->global)
			object_register(od, device->info.id);
	}
	return;

cleanup:
	if (handle)
		pw_unload_spa_handle(handle);
	if (subhandle)
		pw_unload_spa_handle(subhandle);
	return;
}

static struct object_data *find_object(struct pw_impl_device *device, uint32_t id)
{
	struct object_data *od;
	spa_list_for_each(od, &device->object_list, link) {
		if (od->id == id)
			return od;
	}
	return NULL;
}

static void device_object_info(void *data, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct pw_impl_device *device = data;
	struct object_data *od;

	od = find_object(device, id);

	if (info == NULL) {
		pw_log_debug("%p: remove node %d (%p)", device, id, od);
		if (od)
			object_destroy(od);
	}
	else if (od != NULL) {
		if (info->change_mask & SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS)
			object_update(od, info->props);
	}
	else {
		device_add_object(device, id, info);
	}
}

static const struct spa_device_events device_events = {
	SPA_VERSION_DEVICE_EVENTS,
	.info = device_info,
	.object_info = device_object_info,
};

static int handle_device_param(struct pw_impl_device *device, const char *key, const char *value)
{
	const struct spa_type_info *ti;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_pod *pod;
	int res;

	ti = spa_debug_type_find_short(spa_type_param, key);
	if (ti == NULL)
		return -ENOENT;

	if ((res = spa_json_to_pod(&b, 0, ti, value, strlen(value))) < 0)
		return res;

	if ((pod = spa_pod_builder_deref(&b, 0)) == NULL)
		return -ENOSPC;

	if ((res = pw_impl_device_set_param(device, ti->type, 0, pod)) < 0)
		return res;

	return 0;
}

SPA_EXPORT
int pw_impl_device_set_implementation(struct pw_impl_device *device, struct spa_device *spa_device)
{
	int res;
	const struct spa_dict_item *it;

	pw_log_debug("%p: implementation %p", device, spa_device);

	if (device->device) {
		pw_log_error("%p: implementation existed %p",
				device, device->device);
		return -EEXIST;
	}
	device->device = spa_device;
	res = spa_device_add_listener(device->device,
			&device->listener, &device_events, device);

again:
	spa_dict_for_each(it, &device->properties->dict) {
		if (spa_strstartswith(it->key, "device.param.")) {
			if ((res = handle_device_param(device, &it->key[13], it->value)) < 0)
				pw_log_warn("can't set device param: %s", spa_strerror(res));
			pw_properties_set(device->properties, it->key, NULL);
			goto again;
		}
	}
	return res;
}

SPA_EXPORT
struct spa_device *pw_impl_device_get_implementation(struct pw_impl_device *device)
{
	return device->device;
}

SPA_EXPORT
const struct pw_properties *pw_impl_device_get_properties(struct pw_impl_device *device)
{
	return device->properties;
}

SPA_EXPORT
int pw_impl_device_update_properties(struct pw_impl_device *device, const struct spa_dict *dict)
{
	int changed = update_properties(device, dict, false);
	emit_info_changed(device);
	return changed;
}

SPA_EXPORT
void *pw_impl_device_get_user_data(struct pw_impl_device *device)
{
	return device->user_data;
}

SPA_EXPORT
struct pw_global *pw_impl_device_get_global(struct pw_impl_device *device)
{
	return device->global;
}

SPA_EXPORT
void pw_impl_device_add_listener(struct pw_impl_device *device,
			    struct spa_hook *listener,
			    const struct pw_impl_device_events *events,
			    void *data)
{
	spa_hook_list_append(&device->listener_list, listener, events, data);
}
