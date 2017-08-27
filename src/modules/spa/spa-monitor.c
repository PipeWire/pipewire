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

#include <stdio.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

#include <spa/node.h>
#include <spa/monitor.h>
#include <spa/pod-iter.h>

#include <pipewire/log.h>
#include <pipewire/type.h>
#include <pipewire/node.h>

#include "spa-monitor.h"
#include "spa-node.h"

struct monitor_item {
	char *id;
	struct spa_list link;
	struct pw_node *node;
	struct spa_handle *handle;
};

struct impl {
	struct pw_spa_monitor this;

	struct pw_core *core;
	struct pw_type *t;
	struct pw_global *parent;

	void *hnd;

	struct spa_list item_list;
};

static void add_item(struct pw_spa_monitor *this, struct spa_monitor_item *item)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int res;
	struct spa_handle *handle;
	struct monitor_item *mitem;
	void *node_iface;
	void *clock_iface;
	struct pw_properties *props = NULL;
	const char *name, *id, *klass;
	struct spa_handle_factory *factory;
	struct spa_pod *info = NULL;
	struct pw_type *t = pw_core_get_type(impl->core);
	const struct spa_support *support;
	uint32_t n_support;

	spa_pod_object_query(&item->object,
			     t->monitor.name, SPA_POD_TYPE_STRING, &name,
			     t->monitor.id, SPA_POD_TYPE_STRING, &id,
			     t->monitor.klass, SPA_POD_TYPE_STRING, &klass,
			     t->monitor.factory, SPA_POD_TYPE_POINTER, &factory,
			     t->monitor.info, SPA_POD_TYPE_STRUCT, &info, 0);

	pw_log_debug("monitor %p: add: \"%s\" (%s)", this, name, id);

	props = pw_properties_new(NULL, NULL);

	if (info) {
		struct spa_pod_iter it;

		spa_pod_iter_pod(&it, info);
		while (true) {
			const char *key, *val;
			if (!spa_pod_iter_get
			    (&it, SPA_POD_TYPE_STRING, &key, SPA_POD_TYPE_STRING, &val, 0))
				break;
			pw_properties_set(props, key, val);
		}
	}
	pw_properties_set(props, "media.class", klass);

	support = pw_core_get_support(impl->core, &n_support);

	handle = calloc(1, factory->size);
	if ((res = spa_handle_factory_init(factory,
					   handle,
					   &props->dict,
					   support,
					   n_support)) < 0) {
		pw_log_error("can't make factory instance: %d", res);
		return;
	}
	if ((res = spa_handle_get_interface(handle, t->spa_node, &node_iface)) < 0) {
		pw_log_error("can't get NODE interface: %d", res);
		return;
	}
	if ((res = spa_handle_get_interface(handle, t->spa_clock, &clock_iface)) < 0) {
		pw_log_info("no CLOCK interface: %d", res);
		clock_iface = NULL;
	}


	mitem = calloc(1, sizeof(struct monitor_item));
	mitem->id = strdup(id);
	mitem->handle = handle;
	mitem->node = pw_spa_node_new(impl->core, NULL, impl->parent, name,
				      false, node_iface, clock_iface, props, 0);

	spa_list_append(&impl->item_list, &mitem->link);
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
	pw_node_destroy(mitem->node);
	spa_list_remove(&mitem->link);
	spa_handle_clear(mitem->handle);
	free(mitem->handle);
	free(mitem->id);
	free(mitem);
}

static void remove_item(struct pw_spa_monitor *this, struct spa_monitor_item *item)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct monitor_item *mitem;
	const char *name, *id;
	struct pw_type *t = pw_core_get_type(impl->core);

	spa_pod_object_query(&item->object,
			     t->monitor.name, SPA_POD_TYPE_STRING, &name,
			     t->monitor.id, SPA_POD_TYPE_STRING, &id, 0);

	pw_log_debug("monitor %p: remove: \"%s\" (%s)", this, name, id);
	mitem = find_item(this, id);
	if (mitem)
		destroy_item(mitem);
}

static void on_monitor_event(void *data, struct spa_event *event)
{
	struct impl *impl = data;
	struct pw_spa_monitor *this = &impl->this;
	struct pw_type *t = pw_core_get_type(impl->core);

	if (SPA_EVENT_TYPE(event) == t->monitor.Added) {
		struct spa_monitor_item *item = SPA_POD_CONTENTS(struct spa_event, event);
		add_item(this, item);
	} else if (SPA_EVENT_TYPE(event) == t->monitor.Removed) {
		struct spa_monitor_item *item = SPA_POD_CONTENTS(struct spa_event, event);
		remove_item(this, item);
	} else if (SPA_EVENT_TYPE(event) == t->monitor.Changed) {
		struct spa_monitor_item *item = SPA_POD_CONTENTS(struct spa_event, event);
		const char *name;

		spa_pod_object_query(&item->object,
				     t->monitor.name, SPA_POD_TYPE_STRING, &name, 0);

		pw_log_debug("monitor %p: changed: \"%s\"", this, name);
	}
}

static void update_monitor(struct pw_core *core, const char *name)
{
	const char *monitors;
	struct spa_dict_item item;
	const struct pw_properties *props;
	struct spa_dict dict = SPA_DICT_INIT(1, &item);

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
	struct pw_type *t = pw_core_get_type(core);

	asprintf(&filename, "%s/%s.so", dir, lib);

	if ((hnd = dlopen(filename, RTLD_NOW)) == NULL) {
		pw_log_error("can't load %s: %s", filename, dlerror());
		goto open_failed;
	}
	if ((enum_func = dlsym(hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		pw_log_error("can't find enum function");
		goto no_symbol;
	}

	for (index = 0;; index++) {
		if ((res = enum_func(&factory, index)) < 0) {
			if (res != SPA_RESULT_ENUM_END)
				pw_log_error("can't enumerate factories: %d", res);
			goto enum_failed;
		}
		if (strcmp(factory->name, factory_name) == 0)
			break;
	}
	support = pw_core_get_support(core, &n_support);
	handle = calloc(1, factory->size);
	if ((res = spa_handle_factory_init(factory,
					   handle, NULL, support, n_support)) < 0) {
		pw_log_error("can't make factory instance: %d", res);
		goto init_failed;
	}
	if ((res = spa_handle_get_interface(handle, t->spa_monitor, &iface)) < 0) {
		pw_log_error("can't get MONITOR interface: %d", res);
		goto interface_failed;
	}

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	impl->core = core;
	impl->t = t;
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

	for (index = 0;; index++) {
		struct spa_monitor_item *item;
		int res;

		if ((res = spa_monitor_enum_items(this->monitor, &item, index)) < 0) {
			if (res != SPA_RESULT_ENUM_END)
				pw_log_debug("spa_monitor_enum_items: got error %d\n", res);
			break;
		}
		add_item(this, item);
	}
	spa_monitor_set_callbacks(this->monitor, &callbacks, impl);

	return this;

      interface_failed:
	spa_handle_clear(handle);
      init_failed:
	free(handle);
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
