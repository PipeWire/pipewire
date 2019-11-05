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

#include <errno.h>

#include <spa/debug/types.h>
#include <spa/monitor/utils.h>

#include "pipewire/pipewire.h"
#include "pipewire/device.h"
#include "pipewire/private.h"
#include "pipewire/interfaces.h"
#include "pipewire/type.h"
#include "pipewire/keys.h"

#define NAME "device"

struct impl {
	struct pw_device this;
};

#define pw_device_resource(r,m,v,...)	pw_resource_call(r,struct pw_device_proxy_events,m,v,__VA_ARGS__)
#define pw_device_resource_info(r,...)	pw_device_resource(r,info,0,__VA_ARGS__)
#define pw_device_resource_param(r,...) pw_device_resource(r,param,0,__VA_ARGS__)

struct result_device_params_data {
	void *data;
	int (*callback) (void *data, int seq,
			uint32_t id, uint32_t index, uint32_t next,
			struct spa_pod *param);
};

struct resource_data {
	struct pw_device *device;
	struct pw_resource *resource;

	struct spa_hook resource_listener;
	struct spa_hook object_listener;

	/* for async replies */
	int seq;
	int end;
	struct result_device_params_data data;
	struct spa_hook listener;
};

struct object_data {
	struct spa_list link;
	uint32_t id;
	uint32_t type;
	struct spa_handle *handle;
	void *object;
	struct spa_hook listener;
};

static void object_destroy(struct object_data *od)
{
	switch (od->type) {
	case SPA_TYPE_INTERFACE_Node:
		pw_node_destroy(od->object);
		break;
	case SPA_TYPE_INTERFACE_Device:
		pw_device_destroy(od->object);
		break;
	}
}

static void object_update(struct object_data *od, const struct spa_dict *props)
{
	switch (od->type) {
	case SPA_TYPE_INTERFACE_Node:
		pw_node_update_properties(od->object, props);
		break;
	case SPA_TYPE_INTERFACE_Device:
		pw_device_update_properties(od->object, props);
		break;
	}
}

static void object_register(struct object_data *od)
{
	switch (od->type) {
	case SPA_TYPE_INTERFACE_Node:
		pw_node_register(od->object, NULL);
		pw_node_set_active(od->object, true);
		break;
	case SPA_TYPE_INTERFACE_Device:
		pw_device_register(od->object, NULL);
		break;
	}
}

static void check_properties(struct pw_device *device)
{
	const char *str;

	if ((str = pw_properties_get(device->properties, PW_KEY_DEVICE_NAME))) {
		free(device->name);
		device->name = strdup(str);
		pw_log_info(NAME" %p: name '%s'", device, device->name);
	}
}

SPA_EXPORT
struct pw_device *pw_device_new(struct pw_core *core,
				struct pw_properties *properties,
				size_t user_data_size)
{
	struct impl *impl;
	struct pw_device *this;
	int res;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL) {
		res = -errno;
		goto error_cleanup;
	}

	this = &impl->this;
	pw_log_debug(NAME" %p: new", this);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL) {
		res = -errno;
		goto error_free;
	}

	this->core = core;
	this->properties = properties;

	this->info.props = &properties->dict;
	this->info.params = this->params;
	spa_hook_list_init(&this->listener_list);

	spa_list_init(&this->object_list);

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(this, sizeof(struct impl), void);

	check_properties(this);

	return this;

error_free:
	free(impl);
error_cleanup:
	if (properties)
		pw_properties_free(properties);
	errno = -res;
	return NULL;
}

SPA_EXPORT
void pw_device_destroy(struct pw_device *device)
{
	struct object_data *od;

	pw_log_debug(NAME" %p: destroy", device);
	pw_device_emit_destroy(device);

	spa_list_consume(od, &device->object_list, link)
		object_destroy(od);

	if (device->registered)
		spa_list_remove(&device->link);

	if (device->global) {
		spa_hook_remove(&device->global_listener);
		pw_global_destroy(device->global);
	}
	pw_log_debug(NAME" %p: free", device);
	pw_device_emit_free(device);

	pw_properties_free(device->properties);
	free(device->name);

	free(device);
}

static void device_unbind_func(void *data)
{
	struct resource_data *d = data;
	struct pw_resource *resource = d->resource;
	spa_list_remove(&resource->link);
}

static void device_pong(void *data, int seq)
{
	struct resource_data *d = data;
	struct pw_resource *resource = d->resource;
	pw_log_debug(NAME" %p: resource %p: got pong %d", d->device,
			resource, seq);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = device_unbind_func,
	.pong = device_pong,
};

static void result_device_params(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct result_device_params_data *d = data;
	switch (type) {
	case SPA_RESULT_TYPE_DEVICE_PARAMS:
	{
		const struct spa_result_device_params *r = result;
		d->callback(d->data, seq, r->id, r->index, r->next, r->param);
		break;
	}
	default:
		break;
	}
}

SPA_EXPORT
int pw_device_for_each_param(struct pw_device *device,
			     int seq, uint32_t param_id,
			     uint32_t index, uint32_t max,
			     const struct spa_pod *filter,
			     int (*callback) (void *data, int seq,
					      uint32_t id, uint32_t index, uint32_t next,
					      struct spa_pod *param),
			     void *data)
{
	int res;
	struct result_device_params_data user_data = { data, callback };
	struct spa_hook listener;
	static const struct spa_device_events device_events = {
		SPA_VERSION_DEVICE_EVENTS,
		.result = result_device_params,
	};

	if (max == 0)
		max = UINT32_MAX;

	pw_log_debug(NAME" %p: params %s %u %u", device,
			spa_debug_type_find_name(spa_type_param, param_id),
			index, max);

	spa_zero(listener);
	spa_device_add_listener(device->device, &listener,
			&device_events, &user_data);
	res = spa_device_enum_params(device->device, seq,
			param_id, index, max, filter);
	spa_hook_remove(&listener);

	return res;
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

	pw_log_debug(NAME" %p: async result %d %d (%d/%d)", d->device,
			res, seq, d->seq, d->end);

	if (seq == d->end) {
		spa_hook_remove(&d->listener);
		d->end = -1;
		pw_client_set_busy(d->resource->client, false);
	}
	if (seq == d->seq)
		result_device_params(&d->data, seq, res, type, result);

}

static int device_enum_params(void *object, int seq, uint32_t id, uint32_t start, uint32_t num,
		const struct spa_pod *filter)
{
	struct resource_data *data = object;
	struct pw_resource *resource = data->resource;
	struct pw_device *device = data->device;
	struct pw_client *client = resource->client;
	int res;
	static const struct spa_device_events device_events = {
		SPA_VERSION_DEVICE_EVENTS,
		.result = result_device_params_async,
	};

	res = pw_device_for_each_param(device, seq, id, start, num,
				filter, reply_param, data);

	if (res < 0) {
		pw_core_resource_error(client->core_resource,
				resource->id, seq, res, spa_strerror(res));
	} else if (SPA_RESULT_IS_ASYNC(res)) {
		pw_client_set_busy(client, true);
		data->data.data = data;
		data->data.callback = reply_param;
		if (data->end == -1)
			spa_device_add_listener(device->device, &data->listener,
				&device_events, data);
		data->seq = res;
		data->end = spa_device_sync(device->device, res);
	}

	return res;
}

static int device_set_param(void *object, uint32_t id, uint32_t flags,
                           const struct spa_pod *param)
{
	struct resource_data *data = object;
	struct pw_resource *resource = data->resource;
	struct pw_device *device = data->device;
	int res;

	if ((res = spa_device_set_param(device->device, id, flags, param)) < 0)
		pw_resource_error(resource, res, spa_strerror(res));
	return res;
}

static const struct pw_device_proxy_methods device_methods = {
	PW_VERSION_DEVICE_PROXY_METHODS,
	.enum_params = device_enum_params,
	.set_param = device_set_param
};

static int
global_bind(void *_data, struct pw_client *client, uint32_t permissions,
		  uint32_t version, uint32_t id)
{
	struct pw_device *this = _data;
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

	pw_log_debug(NAME" %p: bound to %d", this, resource->id);

	spa_list_append(&global->resource_list, &resource->link);

	this->info.change_mask = PW_DEVICE_CHANGE_MASK_ALL;
	pw_device_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

error_resource:
	pw_log_error(NAME" %p: can't create device resource: %m", this);
	return -errno;
}

static void global_destroy(void *object)
{
	struct pw_device *device = object;
	spa_hook_remove(&device->global_listener);
	device->global = NULL;
	pw_device_destroy(device);
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy,
};

SPA_EXPORT
int pw_device_register(struct pw_device *device,
		       struct pw_properties *properties)
{
	struct pw_core *core = device->core;
	struct object_data *od;
	const char *keys[] = {
		PW_KEY_OBJECT_PATH,
		PW_KEY_MODULE_ID,
		PW_KEY_FACTORY_ID,
		PW_KEY_CLIENT_ID,
		PW_KEY_DEVICE_DESCRIPTION,
		PW_KEY_DEVICE_NAME,
		PW_KEY_DEVICE_NICK,
		PW_KEY_MEDIA_CLASS,
		NULL
	};

	if (device->registered)
		goto error_existed;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		return -errno;

	pw_properties_update_keys(properties, &device->properties->dict, keys);

        device->global = pw_global_new(core,
				       PW_TYPE_INTERFACE_Device,
				       PW_VERSION_DEVICE_PROXY,
				       properties,
				       global_bind,
				       device);
	if (device->global == NULL)
		return -errno;

	spa_list_append(&core->device_list, &device->link);
	device->registered = true;

	device->info.id = device->global->id;
	pw_properties_setf(device->properties, PW_KEY_OBJECT_ID, "%d", device->info.id);
	device->info.props = &device->properties->dict;

	pw_global_add_listener(device->global, &device->global_listener, &global_events, device);
	pw_global_register(device->global);

	spa_list_for_each(od, &device->object_list, link)
		object_register(od);

	return 0;

error_existed:
	if (properties)
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
}

static const struct pw_node_events node_object_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = on_object_destroy,
	.free = on_object_free,
};

static const struct pw_device_events device_object_events = {
	PW_VERSION_DEVICE_EVENTS,
	.destroy = on_object_destroy,
	.free = on_object_free,
};

static void emit_info_changed(struct pw_device *device)
{
	struct pw_resource *resource;

	pw_device_emit_info_changed(device, &device->info);

	if (device->global)
		spa_list_for_each(resource, &device->global->resource_list, link)
			pw_device_resource_info(resource, &device->info);

	device->info.change_mask = 0;
}

static int update_properties(struct pw_device *device, const struct spa_dict *dict)
{
	int changed;

	changed = pw_properties_update(device->properties, dict);
	device->info.props = &device->properties->dict;

	pw_log_debug(NAME" %p: updated %d properties", device, changed);

	if (!changed)
		return 0;

	device->info.change_mask |= PW_DEVICE_CHANGE_MASK_PROPS;

	return changed;
}

static void device_info(void *data, const struct spa_device_info *info)
{
	struct pw_device *device = data;
	if (info->change_mask & SPA_DEVICE_CHANGE_MASK_PROPS) {
		update_properties(device, info->props);
	}
	if (info->change_mask & SPA_DEVICE_CHANGE_MASK_PARAMS) {
		device->info.change_mask |= PW_DEVICE_CHANGE_MASK_PARAMS;
		device->info.n_params = SPA_MIN(info->n_params, SPA_N_ELEMENTS(device->params));
		memcpy(device->info.params, info->params,
				device->info.n_params * sizeof(struct spa_param_info));
	}
	emit_info_changed(device);
}

static void device_add_object(struct pw_device *device, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct pw_core *core = device->core;
	struct spa_handle *handle;
	struct pw_properties *props;
	int res;
	void *iface;
	struct object_data *od = NULL;

	if (info->factory_name == NULL) {
		pw_log_debug(NAME" %p: missing factory name", device);
		return;
	}

	handle = pw_core_load_spa_handle(core, info->factory_name, info->props);
	if (handle == NULL) {
		pw_log_warn(NAME" %p: can't load handle %s: %m",
				device, info->factory_name);
		return;
	}

	if ((res = spa_handle_get_interface(handle, info->type, &iface)) < 0) {
		pw_log_error(NAME" %p: can't get NODE interface: %s", device,
				spa_strerror(res));
		return;
	}

	props = pw_properties_copy(device->properties);
	if (info->props && props)
		pw_properties_update(props, info->props);

	switch (info->type) {
	case SPA_TYPE_INTERFACE_Node:
	{
		struct pw_node *node;
		node = pw_node_new(core, props, sizeof(struct object_data));

		od = pw_node_get_user_data(node);
		od->object = node;
		pw_node_add_listener(node, &od->listener, &node_object_events, od);
		pw_node_set_implementation(node, iface);
		break;
	}
	case SPA_TYPE_INTERFACE_Device:
	{
		struct pw_device *dev;
		dev = pw_device_new(core, props, sizeof(struct object_data));

		od = pw_device_get_user_data(dev);
		od->object = dev;
		pw_device_add_listener(dev, &od->listener, &device_object_events, od);
		pw_device_set_implementation(dev, iface);
		break;
	}
	default:
		pw_log_warn(NAME" %p: unknown type %d", device, info->type);
		pw_properties_free(props);
		break;
	}

	if (od) {
		od->id = id;
		od->type = info->type;
		od->handle = handle;
		spa_list_append(&device->object_list, &od->link);
		if (device->global)
			object_register(od);
	}
	return;
}

static struct object_data *find_object(struct pw_device *device, uint32_t id)
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
	struct pw_device *device = data;
	struct object_data *od;

	od = find_object(device, id);

	if (info == NULL) {
		if (od) {
			pw_log_debug(NAME" %p: remove node %d", device, id);
			object_destroy(od);
		}
		else {
			pw_log_warn(NAME" %p: unknown node %d", device, id);
		}
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

SPA_EXPORT
int pw_device_set_implementation(struct pw_device *device, struct spa_device *spa_device)
{
	pw_log_debug(NAME" %p: implementation %p", device, spa_device);

	if (device->device) {
		pw_log_error(NAME" %p: implementation existed %p",
				device, device->device);
		return -EEXIST;
	}
	device->device = spa_device;
	spa_device_add_listener(device->device,
			&device->listener, &device_events, device);

	return 0;
}

SPA_EXPORT
struct spa_device *pw_device_get_implementation(struct pw_device *device)
{
	return device->device;
}

SPA_EXPORT
const struct pw_properties *pw_device_get_properties(struct pw_device *device)
{
	return device->properties;
}

SPA_EXPORT
int pw_device_update_properties(struct pw_device *device, const struct spa_dict *dict)
{
	int changed = update_properties(device, dict);
	emit_info_changed(device);
	return changed;
}

SPA_EXPORT
void *pw_device_get_user_data(struct pw_device *device)
{
	return device->user_data;
}

SPA_EXPORT
struct pw_global *pw_device_get_global(struct pw_device *device)
{
	return device->global;
}

SPA_EXPORT
void pw_device_add_listener(struct pw_device *device,
			    struct spa_hook *listener,
			    const struct pw_device_events *events,
			    void *data)
{
	spa_hook_list_append(&device->listener_list, listener, events, data);
}
