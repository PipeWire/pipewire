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
#include "pipewire/module.h"
#include "modules/spa/spa-node.h"

#define AUDIOMIXER_LIB "audiomixer/libspa-audiomixer"

struct impl {
	struct pw_core *core;
	struct pw_type *t;
	struct pw_module *module;
	struct pw_properties *properties;

	void *hnd;
	const struct spa_handle_factory *factory;
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

	for (index = 0;; index++) {
		if ((res = enum_func(&factory, index)) < 0) {
			if (res != SPA_RESULT_ENUM_END)
				pw_log_error("can't enumerate factories: %d", res);
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

static struct pw_node *make_node(struct impl *impl)
{
	struct spa_handle *handle;
	int res;
	void *iface;
	struct spa_node *spa_node;
	struct spa_clock *spa_clock;
	struct pw_node *node;
	const struct spa_support *support;
	uint32_t n_support;

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

	if ((res = spa_handle_get_interface(handle, impl->t->spa_clock, &iface)) < 0) {
		iface = NULL;
	}
	spa_clock = iface;

	node = pw_spa_node_new(impl->core, NULL, pw_module_get_global(impl->module),
			       "audiomixer", false, spa_node, spa_clock, NULL);

	return node;

      interface_failed:
	spa_handle_clear(handle);
      init_failed:
	free(handle);
	return NULL;
}

static bool on_global(void *data, struct pw_global *global)
{
	struct impl *impl = data;
	struct pw_node *n, *node;
	struct pw_properties *properties;
	const char *str;
	char *error;
	struct pw_port *ip, *op;
	struct pw_link *link;

	if (pw_global_get_type(global) != impl->t->node)
		return true;

	n = pw_global_get_object(global);

	properties = pw_node_get_properties(n);
	if ((str = pw_properties_get(properties, "media.class")) == NULL)
		return true;

	if (strcmp(str, "Audio/Sink") != 0)
		return true;

	if ((ip = pw_node_get_free_port(n, PW_DIRECTION_INPUT)) == NULL)
		return true;

	node = make_node(impl);
	op = pw_node_get_free_port(node, PW_DIRECTION_OUTPUT);
	if (op == NULL)
		return true;

	link = pw_link_new(impl->core, pw_module_get_global(impl->module), op, ip, NULL, NULL, &error);
	pw_link_inc_idle(link);

	return true;
}

static bool module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	pw_log_debug("module %p: new", impl);

	impl->core = core;
	impl->t = pw_core_get_type(core);
	impl->module = module;
	impl->properties = properties;

	impl->factory = find_factory(impl);

	pw_core_for_each_global(core, on_global, impl);

	return true;
}

#if 0
static void module_destroy(struct impl *impl)
{
	pw_log_debug("module %p: destroy", impl);

	free(impl);
}
#endif

bool pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
