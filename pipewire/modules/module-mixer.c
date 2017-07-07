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

#include "pipewire/server/core.h"
#include "pipewire/server/module.h"
#include "pipewire/modules/spa/spa-node.h"

#define AUDIOMIXER_LIB "audiomixer/libspa-audiomixer"

struct impl {
	struct pw_core *core;
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

	handle = calloc(1, impl->factory->size);
	if ((res = spa_handle_factory_init(impl->factory,
					   handle,
					   NULL, impl->core->support, impl->core->n_support)) < 0) {
		pw_log_error("can't make factory instance: %d", res);
		goto init_failed;
	}
	if ((res = spa_handle_get_interface(handle, impl->core->type.spa_node, &iface)) < 0) {
		pw_log_error("can't get interface %d", res);
		goto interface_failed;
	}
	spa_node = iface;

	if ((res = spa_handle_get_interface(handle, impl->core->type.spa_clock, &iface)) < 0) {
		iface = NULL;
	}
	spa_clock = iface;

	node = pw_spa_node_new(impl->core, NULL, "audiomixer", false, spa_node, spa_clock, NULL);

	return node;

      interface_failed:
	spa_handle_clear(handle);
      init_failed:
	free(handle);
	return NULL;
}

static struct impl *module_new(struct pw_core *core, struct pw_properties *properties)
{
	struct impl *impl;
	struct pw_node *n;

	impl = calloc(1, sizeof(struct impl));
	pw_log_debug("module %p: new", impl);

	impl->core = core;
	impl->properties = properties;

	impl->factory = find_factory(impl);

	spa_list_for_each(n, &core->node_list, link) {
		const char *str;
		char *error;
		struct pw_node *node;
		struct pw_port *ip, *op;

		if (n->global == NULL)
			continue;

		if (n->properties == NULL)
			continue;

		if ((str = pw_properties_get(n->properties, "media.class")) == NULL)
			continue;

		if (strcmp(str, "Audio/Sink") != 0)
			continue;

		if ((ip = pw_node_get_free_port(n, PW_DIRECTION_INPUT)) == NULL)
			continue;

		node = make_node(impl);
		op = pw_node_get_free_port(node, PW_DIRECTION_OUTPUT);
		if (op == NULL)
			continue;

		n->idle_used_input_links++;
		node->idle_used_output_links++;

		pw_link_new(core, op, ip, NULL, NULL, &error);
	}
	return impl;
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
	module_new(module->core, NULL);
	return true;
}
