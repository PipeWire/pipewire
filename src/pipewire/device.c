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

#include "pipewire/device.h"
#include "pipewire/private.h"
#include "pipewire/interfaces.h"
#include "pipewire/type.h"
#include "pipewire/keys.h"

struct impl {
	struct pw_device this;
};

#define pw_device_resource(r,m,v,...)	pw_resource_notify(r,struct pw_device_proxy_events,m,v,__VA_ARGS__)
#define pw_device_resource_info(r,...)	pw_device_resource(r,info,0,__VA_ARGS__)
#define pw_device_resource_param(r,...) pw_device_resource(r,param,0,__VA_ARGS__)

struct resource_data {
	struct spa_hook resource_listener;
	struct pw_device *device;
	struct pw_resource *resource;
};

struct node_data {
	struct spa_list link;
	struct pw_node *node;
	struct spa_handle *handle;
	uint32_t id;
	struct spa_hook node_listener;
};

SPA_EXPORT
struct pw_device *pw_device_new(struct pw_core *core,
				const char *name,
				struct pw_properties *properties,
				size_t user_data_size)
{
	struct impl *impl;
	struct pw_device *this;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		return NULL;

	this = &impl->this;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto no_mem;

	this->core = core;
	this->properties = properties;

	this->info.name = strdup(name);
	this->info.props = &properties->dict;
	this->info.params = this->params;
	spa_hook_list_init(&this->listener_list);

	spa_list_init(&this->node_list);

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(this, sizeof(struct impl), void);

	pw_log_debug("device %p: new %s", this, name);

	return this;

      no_mem:
	free(impl);
	return NULL;
}

SPA_EXPORT
void pw_device_destroy(struct pw_device *device)
{
	struct node_data *nd;

	pw_log_debug("device %p: destroy", device);
	pw_device_emit_destroy(device);

	spa_list_consume(nd, &device->node_list, link)
		pw_node_destroy(nd->node);

	if (device->registered)
		spa_list_remove(&device->link);

	if (device->global) {
		spa_hook_remove(&device->global_listener);
		pw_global_destroy(device->global);
	}
	free((char *)device->info.name);
	pw_properties_free(device->properties);

	free(device);
}

static void device_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	spa_list_remove(&resource->link);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = device_unbind_func,
};

struct result_device_params_data {
	void *data;
	int (*callback) (void *data, int seq,
			uint32_t id, uint32_t index, uint32_t next,
			struct spa_pod *param);
};

static void result_device_params(void *data, int seq, int res, const void *result)
{
	struct result_device_params_data *d = data;
	const struct spa_result_device_params *r =
		(const struct spa_result_device_params *)result;
	d->callback(d->data, seq, r->id, r->index, r->next, r->param);
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

	pw_log_debug("device %p: params %s %u %u", device,
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

static int device_enum_params(void *object, int seq, uint32_t id, uint32_t start, uint32_t num,
		const struct spa_pod *filter)
{
	struct resource_data *data = object;
	struct pw_resource *resource = data->resource;
	struct pw_device *device = data->device;
	struct pw_client *client = resource->client;
	int res;

	if ((res = pw_device_for_each_param(device, seq, id, start, num,
				filter, reply_param, data)) < 0)
		pw_core_resource_error(client->core_resource,
				resource->id, seq, res, spa_strerror(res));
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
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	data->device = this;
	data->resource = resource;
	pw_resource_add_listener(resource, &data->resource_listener, &resource_events, resource);

	pw_resource_set_implementation(resource, &device_methods, data);

	pw_log_debug("device %p: bound to %d", this, resource->id);

	spa_list_append(&global->resource_list, &resource->link);

	this->info.change_mask = PW_DEVICE_CHANGE_MASK_ALL;
	pw_device_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

      no_mem:
	pw_log_error("can't create device resource");
	return -ENOMEM;
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
		       struct pw_client *owner,
		       struct pw_global *parent,
		       struct pw_properties *properties)
{
	struct pw_core *core = device->core;
	struct node_data *nd;
	const char *str;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		return -ENOMEM;

	pw_properties_set(properties, PW_KEY_DEVICE_NAME, device->info.name);
	if ((str = pw_properties_get(device->properties, PW_KEY_MEDIA_CLASS)) != NULL)
		pw_properties_set(properties, PW_KEY_MEDIA_CLASS, str);

	spa_list_append(&core->device_list, &device->link);
	device->registered = true;

        device->global = pw_global_new(core,
				       PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE_PROXY,
				       properties,
				       global_bind,
				       device);
	if (device->global == NULL)
		return -ENOMEM;

	device->info.id = device->global->id;
	pw_global_add_listener(device->global, &device->global_listener, &global_events, device);
	pw_global_register(device->global, owner, parent);

	spa_list_for_each(nd, &device->node_list, link) {
		pw_node_register(nd->node, NULL, device->global, NULL);
		pw_node_set_active(nd->node, true);
	}
	return 0;
}

static void node_destroy(void *data)
{
	struct node_data *nd = data;
	spa_list_remove(&nd->link);
	spa_handle_clear(nd->handle);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = node_destroy,
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

	pw_log_debug("device %p: updated %d properties", device, changed);

	if (!changed)
		return 0;

	device->info.props = &device->properties->dict;
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

static void device_add(struct pw_device *device, uint32_t id,
		const struct spa_device_object_info *info)
{
	const struct spa_support *support;
	uint32_t n_support;
	struct pw_node *node;
	struct node_data *nd;
	struct pw_properties *props;
	int res;
	void *iface;

	if (info->type != SPA_TYPE_INTERFACE_Node) {
		pw_log_warn("device %p: unknown type %d", device, info->type);
		return;
	}

	pw_log_debug("device %p: add node %d", device, id);
	support = pw_core_get_support(device->core, &n_support);

	props = pw_properties_copy(device->properties);
	if (info->props)
		pw_properties_update(props, info->props);

	node = pw_node_new(device->core,
			   device->info.name,
			   props,
			   sizeof(struct node_data) +
			   spa_handle_factory_get_size(info->factory, info->props));

	nd = pw_node_get_user_data(node);
	nd->id = id;
	nd->node = node;
	nd->handle = SPA_MEMBER(nd, sizeof(struct node_data), void);
	pw_node_add_listener(node, &nd->node_listener, &node_events, nd);
	spa_list_append(&device->node_list, &nd->link);

	if ((res = spa_handle_factory_init(info->factory,
					   nd->handle,
					   info->props,
					   support,
					   n_support)) < 0) {
		pw_log_error("can't make factory instance: %d", res);
		goto error;;
        }

	if ((res = spa_handle_get_interface(nd->handle, info->type, &iface)) < 0) {
		pw_log_error("can't get NODE interface: %d", res);
		goto error;;
	}

	pw_node_set_implementation(node, iface);

	if (device->global) {
		pw_node_register(node, NULL, device->global, NULL);
		pw_node_set_active(node, true);
	}
	return;

error:
	pw_node_destroy(node);
	return;
}

static struct node_data *find_node(struct pw_device *device, uint32_t id)
{
	struct node_data *nd;
	spa_list_for_each(nd, &device->node_list, link) {
		if (nd->id == id)
			return nd;
	}
	return NULL;
}

static void device_object_info(void *data, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct pw_device *device = data;
	struct node_data *nd;

	nd = find_node(device, id);

	if (info == NULL) {
		if (nd) {
			pw_log_debug("device %p: remove node %d", device, id);
			pw_node_destroy(nd->node);
		}
		else {
			pw_log_warn("device %p: unknown node %d", device, id);
		}
	}
	else if (nd != NULL) {
		if (info->change_mask & SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS)
			pw_node_update_properties(nd->node, info->props);
	}
	else {
		device_add(device, id, info);
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
	pw_log_debug("device %p: implementation %p", device, spa_device);

	if (device->device) {
		pw_log_error("device %p: implementation existed %p",
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
