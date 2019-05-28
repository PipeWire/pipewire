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

#include "spa-monitor.h"
#include "spa-device.h"

struct monitor_item {
	char *id;
	struct spa_list link;
	struct spa_handle *handle;
	uint32_t type;
	void *iface;
	void *object;
	struct spa_hook object_listener;
};

struct impl {
	struct pw_spa_monitor this;

	struct pw_core *core;
	struct pw_global *parent;

	void *hnd;

	struct spa_list item_list;
};

static void device_free(void *data)
{
	struct monitor_item *mitem = data;
	spa_hook_remove(&mitem->object_listener);
	spa_list_remove(&mitem->link);
	spa_handle_clear(mitem->handle);
	free(mitem->handle);
	free(mitem->id);
	free(mitem);
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.free = device_free
};

static struct monitor_item *add_item(struct pw_spa_monitor *this,
		struct spa_pod *item, uint64_t now)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int res;
	struct spa_handle *handle;
	struct monitor_item *mitem;
	void *iface;
	struct pw_properties *props = NULL;
	const char *name, *id, *klass, *str;
	struct spa_handle_factory *factory;
	enum spa_monitor_item_state state;
	struct spa_pod *info = NULL;
	const struct spa_support *support;
	uint32_t n_support, type, ftype;

	if (spa_pod_parse_object(item,
			SPA_TYPE_OBJECT_MonitorItem, NULL,
			SPA_MONITOR_ITEM_id,      SPA_POD_String(&id),
			SPA_MONITOR_ITEM_state,   SPA_POD_Id(&state),
			SPA_MONITOR_ITEM_name,    SPA_POD_String(&name),
			SPA_MONITOR_ITEM_class,   SPA_POD_String(&klass),
			SPA_MONITOR_ITEM_factory, SPA_POD_Pointer(&ftype, &factory),
			SPA_MONITOR_ITEM_type,    SPA_POD_Id(&type),
			SPA_MONITOR_ITEM_info,    SPA_POD_Pod(&info)) < 0) {
		pw_log_warn("monitor %p: could not parse item", this);
		spa_debug_pod(0, NULL, item);
		return NULL;
	}

	pw_log_debug("monitor %p: add: \"%s\" (%s)", this, name, id);

	props = pw_properties_new(NULL, NULL);

	if (info) {
		struct spa_pod_parser prs;
		struct spa_pod_frame f;

		spa_pod_parser_pod(&prs, info);
		if (spa_pod_parser_push_struct(&prs, &f) == 0) {
			while (true) {
				const char *key, *val;
				if (spa_pod_parser_get(&prs,
						SPA_POD_String(&key),
						SPA_POD_String(&val), NULL) < 0)
					break;
				pw_properties_set(props, key, val);
			}
		}
	}

	if ((str = pw_properties_get(props, PW_KEY_DEVICE_FORM_FACTOR)) != NULL)
		if (strcmp(str, "internal") == 0)
			now = 0;
	if (now != 0 && pw_properties_get(props, PW_KEY_DEVICE_PLUGGED) == NULL)
		pw_properties_setf(props, PW_KEY_DEVICE_PLUGGED, "%"PRIu64, now);

	support = pw_core_get_support(impl->core, &n_support);

        handle = calloc(1, spa_handle_factory_get_size(factory, NULL));
	if ((res = spa_handle_factory_init(factory,
					   handle,
					   &props->dict,
					   support,
					   n_support)) < 0) {
		pw_properties_free(props);
		pw_log_error("can't make factory instance: %d", res);
		return NULL;
	}


	if ((res = spa_handle_get_interface(handle, type, &iface)) < 0) {
		pw_log_error("can't get %d interface: %d", type, res);
		pw_properties_free(props);
		return NULL;
	}

	mitem = calloc(1, sizeof(struct monitor_item));
	mitem->id = strdup(id);
	mitem->handle = handle;
	mitem->type = type;

	switch (type) {
	case SPA_TYPE_INTERFACE_Device:
	{
		struct pw_device *device;
		device = pw_spa_device_new(impl->core, NULL, impl->parent, name,
				      0, iface, handle, props, 0);
		pw_device_add_listener(device, &mitem->object_listener,
				&device_events, mitem);
		mitem->object = device;
		break;
	}
	default:
		pw_log_error("interface %d not implemented", type);
		free(mitem->id);
		free(mitem);
		return NULL;
	}

	spa_list_append(&impl->item_list, &mitem->link);

	return mitem;
}

static struct monitor_item *find_item(struct pw_spa_monitor *this, const char *id)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct monitor_item *mitem;

	spa_list_for_each(mitem, &impl->item_list, link) {
		if (strcmp(mitem->id, id) == 0) {
			return mitem;
		}
	}
	return NULL;
}

void destroy_item(struct monitor_item *mitem)
{
	switch (mitem->type) {
	case SPA_TYPE_INTERFACE_Node:
		pw_node_destroy(mitem->object);
		break;
	case SPA_TYPE_INTERFACE_Device:
		pw_device_destroy(mitem->object);
		break;
	default:
		break;
	}
}

static void remove_item(struct pw_spa_monitor *this, struct spa_pod *item, uint64_t now)
{
	struct monitor_item *mitem;
	const char *name, *id;

	if (spa_pod_parse_object(item,
			SPA_TYPE_OBJECT_MonitorItem, NULL,
			SPA_MONITOR_ITEM_name, SPA_POD_String(&name),
			SPA_MONITOR_ITEM_id,   SPA_POD_String(&id)) < 0)
		return;

	pw_log_debug("monitor %p: remove: \"%s\" (%s)", this, name, id);
	mitem = find_item(this, id);
	if (mitem)
		destroy_item(mitem);
}

static void change_item(struct pw_spa_monitor *this, struct spa_pod *item, uint64_t now)
{
	struct monitor_item *mitem;
	const char *name, *id;
	enum spa_monitor_item_state state;

	if (spa_pod_parse_object(item,
			SPA_TYPE_OBJECT_MonitorItem, NULL,
			SPA_MONITOR_ITEM_name,  SPA_POD_String(&name),
			SPA_MONITOR_ITEM_state, SPA_POD_Id(&state),
			SPA_MONITOR_ITEM_id,    SPA_POD_String(&id)) < 0)
		return;

	pw_log_debug("monitor %p: change: \"%s\" (%s)", this, name, id);
	mitem = find_item(this, id);
	if (mitem == NULL)
		mitem = add_item(this, item, now);
	if (mitem == NULL)
		return;

	switch (state) {
	case SPA_MONITOR_ITEM_STATE_Available:
		break;
	case SPA_MONITOR_ITEM_STATE_Disabled:
	case SPA_MONITOR_ITEM_STATE_Unavailable:
		break;
	default:
		break;
	}
}

static int on_monitor_event(void *data, const struct spa_event *event)
{
	struct impl *impl = data;
	struct pw_spa_monitor *this = &impl->this;
	struct timespec now;
	uint64_t now_nsec;
	struct spa_pod *item;

	clock_gettime(CLOCK_MONOTONIC, &now);
	now_nsec = SPA_TIMESPEC_TO_NSEC(&now);

	item = SPA_POD_CONTENTS(struct spa_event, event);
	switch (SPA_MONITOR_EVENT_ID(event)) {
	case SPA_MONITOR_EVENT_Added:
		add_item(this, item, now_nsec);
		break;
	case SPA_MONITOR_EVENT_Removed:
		remove_item(this, item, now_nsec);
		break;
	case SPA_MONITOR_EVENT_Changed:
		change_item(this, item, now_nsec);
		break;
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
		monitors = pw_properties_get(props, "monitors");
	else
		monitors = NULL;

	item.key = "monitors";
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
	.event = on_monitor_event,
};

struct pw_spa_monitor *pw_spa_monitor_load(struct pw_core *core,
					   struct pw_global *parent,
					   const char *dir,
					   const char *lib,
					   const char *factory_name,
					   const char *system_name,
					   size_t user_data_size)
{
	struct impl *impl;
	struct pw_spa_monitor *this;
	struct spa_handle *handle;
	int res;
	void *iface;
	void *hnd;
	uint32_t index;
	spa_handle_factory_enum_func_t enum_func;
	const struct spa_handle_factory *factory;
	char *filename;
	const struct spa_support *support;
	uint32_t n_support;

	asprintf(&filename, "%s/%s.so", dir, lib);

	if ((hnd = dlopen(filename, RTLD_NOW)) == NULL) {
		pw_log_error("can't load %s: %s", filename, dlerror());
		goto open_failed;
	}
	if ((enum_func = dlsym(hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		pw_log_error("can't find enum function");
		goto no_symbol;
	}

	for (index = 0;;) {
		if ((res = enum_func(&factory, &index)) <= 0) {
			if (res != 0)
				pw_log_error("can't enumerate factories: %s", spa_strerror(res));
			goto enum_failed;
		}
		if (strcmp(factory->name, factory_name) == 0)
			break;
	}
	handle = calloc(1, spa_handle_factory_get_size(factory, NULL));
	if (handle == NULL)
		goto no_mem;

	support = pw_core_get_support(core, &n_support);

	if ((res = spa_handle_factory_init(factory,
					   handle, NULL, support, n_support)) < 0) {
		pw_log_error("can't make factory instance: %d", res);
		goto init_failed;
	}
	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Monitor, &iface)) < 0) {
		pw_log_error("can't get MONITOR interface: %d", res);
		goto interface_failed;
	}

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	impl->core = core;
	impl->parent = parent;
	impl->hnd = hnd;

	this = &impl->this;
	this->monitor = iface;
	this->lib = filename;
	this->factory_name = strdup(factory_name);
	this->system_name = strdup(system_name);
	this->handle = handle;

        if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	update_monitor(core, this->system_name);

	spa_list_init(&impl->item_list);

	spa_monitor_set_callbacks(this->monitor, &callbacks, impl);

	return this;

      interface_failed:
	spa_handle_clear(handle);
      init_failed:
	free(handle);
      no_mem:
      enum_failed:
      no_symbol:
	dlclose(hnd);
      open_failed:
	free(filename);
	return NULL;

}

void pw_spa_monitor_destroy(struct pw_spa_monitor *monitor)
{
	struct impl *impl = SPA_CONTAINER_OF(monitor, struct impl, this);
	struct monitor_item *mitem, *tmp;

	pw_log_debug("spa-monitor %p: destroy", impl);

	spa_list_for_each_safe(mitem, tmp, &impl->item_list, link)
		destroy_item(mitem);

	spa_handle_clear(monitor->handle);
	free(monitor->handle);
	free(monitor->lib);
	free(monitor->factory_name);
	free(monitor->system_name);

	dlclose(impl->hnd);
	free(impl);
}
