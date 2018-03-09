/* PipeWire
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include "config.h"

#include "pipewire/core.h"
#include "pipewire/link.h"
#include "pipewire/log.h"
#include "pipewire/module.h"
#include "pipewire/type.h"
#include "modules/spa/spa-node.h"

#define AUDIOMIXER_LIB "audiomixer/libspa-audiomixer"

struct impl {
	struct pw_core *core;
	struct pw_type *t;
	struct pw_module *module;
	struct spa_hook core_listener;
	struct spa_hook module_listener;
	struct pw_properties *properties;

	void *hnd;
	const struct spa_handle_factory *factory;

	struct spa_list node_list;
};

struct node_data {
	struct spa_list link;
	struct pw_node *node;
};

static const struct spa_handle_factory *find_factory(struct impl *impl)
{
	spa_handle_factory_enum_func_t enum_func;
	uint32_t index;
	const struct spa_handle_factory *factory = NULL;
	int res;
	char *filename;
	const char *dir;

	if ((dir = getenv("SPA_PLUGIN_DIR")) == NULL)
		dir = PLUGINDIR;

	asprintf(&filename, "%s/%s.so", dir, AUDIOMIXER_LIB);

	if ((impl->hnd = dlopen(filename, RTLD_NOW)) == NULL) {
		pw_log_error("can't load %s: %s", AUDIOMIXER_LIB, dlerror());
		goto open_failed;
	}
	if ((enum_func = dlsym(impl->hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		pw_log_error("can't find enum function");
		goto no_symbol;
	}

	for (index = 0;;) {
		if ((res = enum_func(&factory, &index)) <= 0) {
			if (res != 0)
				pw_log_error("can't enumerate factories: %s", spa_strerror(res));
			goto enum_failed;
		}
		if (strcmp(factory->name, "audiomixer") == 0)
			break;
	}
	free(filename);
	return factory;

      enum_failed:
      no_symbol:
	dlclose(impl->hnd);
	impl->hnd = NULL;
      open_failed:
	free(filename);
	return NULL;
}

static struct pw_node *make_node(struct impl *impl, struct pw_properties *properties)
{
	struct spa_handle *handle;
	int res;
	void *iface;
	struct spa_node *spa_node;
	struct pw_node *node;
	const struct spa_support *support;
	uint32_t n_support;
	struct node_data *nd;

	support = pw_core_get_support(impl->core, &n_support);

	handle = calloc(1, impl->factory->size);
	if ((res = spa_handle_factory_init(impl->factory,
					   handle,
					   NULL, support, n_support)) < 0) {
		pw_log_error("can't make factory instance: %d", res);
		goto init_failed;
	}
	if ((res = spa_handle_get_interface(handle, impl->t->spa_node, &iface)) < 0) {
		pw_log_error("can't get interface %d", res);
		goto interface_failed;
	}
	spa_node = iface;

	pw_properties_set(properties, "media.class", "Audio/Mixer");

	node = pw_spa_node_new(impl->core, NULL, pw_module_get_global(impl->module),
			       "audiomixer", PW_SPA_NODE_FLAG_ACTIVATE,
			       spa_node, handle,
			       properties,
			       sizeof(struct node_data));

	nd = pw_spa_node_get_user_data(node);
	nd->node = node;
	spa_list_append(&impl->node_list, &nd->link);

	return node;

      interface_failed:
	spa_handle_clear(handle);
      init_failed:
	free(handle);
	return NULL;
}

static int on_global(void *data, struct pw_global *global)
{
	struct impl *impl = data;
	struct pw_node *n, *node;
	const struct pw_properties *properties;
	const char *str;
	char *error;
	struct pw_port *ip, *op;
	struct pw_link *link;

	if (pw_global_get_type(global) != impl->t->node)
		return 0;

	n = pw_global_get_object(global);

	properties = pw_node_get_properties(n);
	if ((str = pw_properties_get(properties, "media.class")) == NULL ||
	    strcmp(str, "Audio/Sink") != 0)
		return 0;

	if ((ip = pw_node_get_free_port(n, PW_DIRECTION_INPUT)) == NULL)
		return 0;

	node = make_node(impl, pw_properties_copy(properties));
	op = pw_node_get_free_port(node, PW_DIRECTION_OUTPUT);
	if (op == NULL)
		return 0;

	link = pw_link_new(impl->core,
			   op,
			   ip,
			   NULL,
			   pw_properties_new(PW_LINK_PROP_PASSIVE, "true", NULL),
			   &error, 0);
	pw_link_register(link, NULL, pw_module_get_global(impl->module), NULL);

	return 0;
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	struct node_data *nd, *t;

	spa_hook_remove(&impl->module_listener);
	spa_hook_remove(&impl->core_listener);

	spa_list_for_each_safe(nd, t, &impl->node_list, link)
		pw_node_destroy(nd->node);

	if (impl->properties)
		pw_properties_free(impl->properties);

	free(impl);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
};

static void
core_global_added(void *data, struct pw_global *global)
{
	on_global(data, global);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
        .global_added = core_global_added,
};

static int module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -ENOMEM;

	pw_log_debug("module %p: new", impl);

	impl->core = core;
	impl->t = pw_core_get_type(core);
	impl->module = module;
	impl->properties = properties;

	impl->factory = find_factory(impl);

	spa_list_init(&impl->node_list);

	pw_core_for_each_global(core, on_global, impl);

	pw_core_add_listener(core, &impl->core_listener, &core_events, impl);
	pw_module_add_listener(module, &impl->module_listener, &module_events, impl);

	return 0;
}

int pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
