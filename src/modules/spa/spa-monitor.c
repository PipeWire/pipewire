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

#include <stdio.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

#include <spa/node/node.h>
#include <spa/monitor/monitor.h>
#include <spa/pod/parser.h>
#include <spa/debug/pod.h>

#include <pipewire/log.h>
#include <pipewire/type.h>
#include <pipewire/node.h>
#include <pipewire/device.h>
#include <pipewire/keys.h>
#include <pipewire/pipewire.h>

#include "spa-monitor.h"
#include "spa-device.h"

struct monitor_object {
	uint32_t id;
	char *name;
	struct spa_list link;
	struct spa_handle *handle;
	uint32_t type;
	void *object;
	struct spa_hook object_listener;
};

struct impl {
	struct pw_spa_monitor this;

	struct pw_core *core;
	struct pw_global *parent;

	struct spa_list item_list;
};

static void device_free(void *data)
{
	struct monitor_object *obj = data;
	spa_hook_remove(&obj->object_listener);
	spa_list_remove(&obj->link);
	pw_unload_spa_handle(obj->handle);
	free(obj);
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.free = device_free
};

static struct monitor_object *add_object(struct pw_spa_monitor *this, uint32_t id,
		const struct spa_monitor_object_info *info, uint64_t now)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct pw_core *core = impl->core;
	int res;
	struct spa_handle *handle;
	struct monitor_object *obj;
	const char *name, *str;
	void *iface;
	struct pw_properties *props = NULL;

	if (info->props)
		props = pw_properties_new_dict(info->props);
	else
		props = pw_properties_new(NULL, NULL);

	if (props == NULL)
		return NULL;

	if ((name = pw_properties_get(props, PW_KEY_DEVICE_NAME)) == NULL)
		name = "unknown";

	pw_log_debug("monitor %p: add: \"%s\" (%u)", this, name, id);

	if ((str = pw_properties_get(props, PW_KEY_DEVICE_FORM_FACTOR)) != NULL)
		if (strcmp(str, "internal") == 0)
			now = 0;
	if (now != 0 && pw_properties_get(props, PW_KEY_DEVICE_PLUGGED) == NULL)
		pw_properties_setf(props, PW_KEY_DEVICE_PLUGGED, "%"PRIu64, now);

	handle = pw_core_load_spa_handle(core, info->factory_name,
			&props->dict);
	if (handle == NULL) {
		res = -errno;
		pw_log_error("can't make factory instance: %m");
		goto error_exit_free_props;
	}

	if ((res = spa_handle_get_interface(handle, info->type, &iface)) < 0) {
		pw_log_error("can't get %d interface: %s", info->type, spa_strerror(res));
		goto error_exit_free_handle;
	}

	obj = calloc(1, sizeof(struct monitor_object));
	if (obj == NULL) {
		res = -errno;
		goto error_exit_free_handle;
	}
	obj->id = id;
	obj->name = strdup(name);
	obj->handle = handle;
	obj->type = info->type;

	switch (obj->type) {
	case SPA_TYPE_INTERFACE_Device:
	{
		struct pw_device *device;
		device = pw_spa_device_new(core, NULL, impl->parent,
				      0, iface, handle, props, 0);
		pw_device_add_listener(device, &obj->object_listener,
				&device_events, obj);
		obj->object = device;
		break;
	}
	default:
		res = -ENOTSUP;
		pw_log_error("interface %d not implemented", obj->type);
		goto error_exit_free_object;
	}

	spa_list_append(&impl->item_list, &obj->link);

	return obj;

error_exit_free_object:
	free(obj->name);
	free(obj);
error_exit_free_handle:
	pw_unload_spa_handle(handle);
error_exit_free_props:
	pw_properties_free(props);
	errno = -res;
	return NULL;
}

static struct monitor_object *find_object(struct pw_spa_monitor *this, uint32_t id)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct monitor_object *obj;

	spa_list_for_each(obj, &impl->item_list, link) {
		if (obj->id == id) {
			return obj;
		}
	}
	return NULL;
}

void destroy_object(struct monitor_object *obj)
{
	switch (obj->type) {
	case SPA_TYPE_INTERFACE_Node:
		pw_node_destroy(obj->object);
		break;
	case SPA_TYPE_INTERFACE_Device:
		pw_device_destroy(obj->object);
		break;
	default:
		break;
	}
}

static void change_object(struct pw_spa_monitor *this, struct monitor_object *obj,
		const struct spa_monitor_object_info *info, uint64_t now)
{
	pw_log_debug("monitor %p: change: \"%s\" (%u)", this, obj->name, obj->id);
}

static int on_monitor_object_info(void *data, uint32_t id,
		const struct spa_monitor_object_info *info)
{
	struct impl *impl = data;
	struct pw_spa_monitor *this = &impl->this;
	struct timespec now;
	uint64_t now_nsec;
	struct monitor_object *obj;

	clock_gettime(CLOCK_MONOTONIC, &now);
	now_nsec = SPA_TIMESPEC_TO_NSEC(&now);

	obj = find_object(this, id);

	if (info == NULL) {
		if (obj == NULL)
			return -ENODEV;

		pw_log_debug("monitor %p: remove: (%s) %u", this, obj->name, id);
		destroy_object(obj);
	} else if (obj == NULL) {
		obj = add_object(this, id, info, now_nsec);
		if (obj == NULL)
			return -errno;
	} else {
		change_object(this, obj, info, now_nsec);
	}
	return 0;
}

static void update_monitor(struct pw_core *core, const char *name)
{
	const char *monitors;
	struct spa_dict_item item;
	const struct pw_properties *props;
	struct spa_dict dict = SPA_DICT_INIT(&item, 1);

	props = pw_core_get_properties(core);

	if (props)
		monitors = pw_properties_get(props, PW_KEY_CORE_MONITORS);
	else
		monitors = NULL;

	item.key = PW_KEY_CORE_MONITORS;
	if (monitors == NULL)
		item.value = name;
	else
		asprintf((char **) &item.value, "%s,%s", monitors, name);

	pw_core_update_properties(core, &dict);

	if (monitors != NULL)
		free((void *) item.value);
}

static const struct spa_monitor_callbacks callbacks = {
	SPA_VERSION_MONITOR_CALLBACKS,
	.object_info = on_monitor_object_info,
};

struct pw_spa_monitor *pw_spa_monitor_load(struct pw_core *core,
		struct pw_global *parent,
		const char *factory_name,
		const char *system_name,
		struct pw_properties *properties,
		size_t user_data_size)
{
	struct impl *impl;
	struct pw_spa_monitor *this;
	struct spa_handle *handle;
	int res;
	void *iface;

	handle = pw_core_load_spa_handle(core,
			factory_name,
			properties ? &properties->dict : NULL);
	if (handle == NULL) {
		res = -errno;
		goto error_exit;
	}

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Monitor, &iface)) < 0) {
		pw_log_error("can't get MONITOR interface: %s", spa_strerror(res));
		goto error_exit_unload;
	}

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL) {
		res = -errno;
		goto error_exit_unload;
	}

	impl->core = core;
	impl->parent = parent;
	spa_list_init(&impl->item_list);

	this = &impl->this;
	this->monitor = iface;
	this->factory_name = strdup(factory_name);
	this->system_name = strdup(system_name);
	this->handle = handle;
	this->properties = properties;

        if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	update_monitor(core, this->system_name);

	spa_monitor_set_callbacks(this->monitor, &callbacks, impl);

	return this;

error_exit_unload:
	pw_unload_spa_handle(handle);
error_exit:
	errno = -res;
	return NULL;
}

void pw_spa_monitor_destroy(struct pw_spa_monitor *monitor)
{
	struct impl *impl = SPA_CONTAINER_OF(monitor, struct impl, this);
	struct monitor_object *obj, *tmp;

	pw_log_debug("spa-monitor %p: destroy", impl);

	spa_list_for_each_safe(obj, tmp, &impl->item_list, link)
		destroy_object(obj);

	pw_unload_spa_handle(monitor->handle);
	free(monitor->factory_name);
	free(monitor->system_name);
	if (monitor->properties)
		pw_properties_free(monitor->properties);
	free(impl);
}
