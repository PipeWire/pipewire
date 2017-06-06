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

#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

#include <spa/node.h>

#include "spa-node.h"

struct impl {
	struct pw_spa_node this;
	struct pw_core *core;

	void *hnd;
};

struct pw_spa_node *pw_spa_node_load(struct pw_core *core,
				     const char *lib,
				     const char *factory_name,
				     const char *name,
				     struct pw_properties *properties, setup_node_t setup_func)
{
	struct pw_spa_node *this;
	struct impl *impl;
	struct spa_node *spa_node;
	struct spa_clock *spa_clock;
	int res;
	struct spa_handle *handle;
	void *hnd;
	uint32_t index;
	spa_handle_factory_enum_func_t enum_func;
	const struct spa_handle_factory *factory;
	void *iface;

	if ((hnd = dlopen(lib, RTLD_NOW)) == NULL) {
		pw_log_error("can't load %s: %s", lib, dlerror());
		return NULL;
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

	handle = calloc(1, factory->size);
	if ((res = spa_handle_factory_init(factory,
					   handle, NULL, core->support, core->n_support)) < 0) {
		pw_log_error("can't make factory instance: %d", res);
		goto init_failed;
	}
	if ((res = spa_handle_get_interface(handle, core->type.spa_node, &iface)) < 0) {
		pw_log_error("can't get node interface %d", res);
		goto interface_failed;
	}
	spa_node = iface;

	if ((res = spa_handle_get_interface(handle, core->type.spa_clock, &iface)) < 0) {
		iface = NULL;
	}
	spa_clock = iface;

	impl = calloc(1, sizeof(struct impl));
	impl->core = core;
	impl->hnd = hnd;
	this = &impl->this;

	if (setup_func != NULL) {
		if (setup_func(core, spa_node, properties) != SPA_RESULT_OK) {
			pw_log_debug("Unrecognized properties");
		}
	}

	this->node = pw_node_new(core, NULL, name, false, spa_node, spa_clock, properties);
	this->lib = strdup(lib);
	this->factory_name = strdup(factory_name);
	this->handle = handle;

	return this;

      interface_failed:
	spa_handle_clear(handle);
      init_failed:
	free(handle);
      enum_failed:
      no_symbol:
	dlclose(hnd);
	return NULL;
}

void pw_spa_node_destroy(struct pw_spa_node *node)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);

	pw_log_debug("spa-node %p: destroy", impl);
	pw_signal_emit(&node->destroy_signal, node);

	pw_node_destroy(node->node);

	spa_handle_clear(node->handle);
	free(node->handle);
	free(node->lib);
	free(node->factory_name);
	dlclose(impl->hnd);
	free(impl);
}
