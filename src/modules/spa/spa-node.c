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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/debug/types.h>

#include "spa-node.h"
#include "pipewire/node.h"
#include "pipewire/port.h"
#include "pipewire/log.h"
#include "pipewire/private.h"

struct impl {
	struct pw_node *this;

	struct pw_client *owner;
	struct pw_global *parent;

	enum pw_spa_node_flags flags;

	void *hnd;
        struct spa_handle *handle;
        struct spa_node *node;          /**< handle to SPA node */
	char *lib;
	char *factory_name;

	struct spa_hook node_listener;
	int init_pending;

	void *user_data;

	int async_init:1;
};

static void spa_node_free(void *data)
{
	struct impl *impl = data;
	struct pw_node *node = impl->this;

	pw_log_debug("spa-node %p: free", node);

	spa_hook_remove(&impl->node_listener);
	if (impl->handle) {
		spa_handle_clear(impl->handle);
		free(impl->handle);
	}
	free(impl->lib);
	free(impl->factory_name);
	if (impl->hnd)
		dlclose(impl->hnd);
}

static void complete_init(struct impl *impl)
{
        struct pw_node *this = impl->this;

	impl->init_pending = SPA_ID_INVALID;
	if (SPA_FLAG_CHECK(impl->flags, PW_SPA_NODE_FLAG_DISABLE))
		pw_node_set_enabled(this, false);

	if (SPA_FLAG_CHECK(impl->flags, PW_SPA_NODE_FLAG_ACTIVATE))
		pw_node_set_active(this, true);

	if (!SPA_FLAG_CHECK(impl->flags, PW_SPA_NODE_FLAG_NO_REGISTER))
		pw_node_register(this, impl->owner, impl->parent, NULL);
	else
		pw_node_initialized(this);
}

static void spa_node_result(void *data, int seq, int res, const void *result)
{
	struct impl *impl = data;
	struct pw_node *node = impl->this;

	if (seq == impl->init_pending) {
		pw_log_debug("spa-node %p: init complete event %d %d", node, seq, res);
		complete_init(impl);
	}
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.free = spa_node_free,
	.result = spa_node_result,
};

struct pw_node *
pw_spa_node_new(struct pw_core *core,
		struct pw_client *owner,
		struct pw_global *parent,
		const char *name,
		enum pw_spa_node_flags flags,
		struct spa_node *node,
		struct spa_handle *handle,
		struct pw_properties *properties,
		size_t user_data_size)
{
	struct pw_node *this;
	struct impl *impl;
	int res;

	this = pw_node_new(core, name, properties, sizeof(struct impl) + user_data_size);
	if (this == NULL)
		return NULL;

	impl = this->user_data;
	impl->this = this;
	impl->owner = owner;
	impl->parent = parent;
	impl->node = node;
	impl->flags = flags;

	if (user_data_size > 0)
                impl->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	pw_node_add_listener(this, &impl->node_listener, &node_events, impl);
	res = pw_node_set_implementation(this, impl->node);

	if (res < 0)
		goto clean_node;

	if (flags & PW_SPA_NODE_FLAG_ASYNC) {
		impl->init_pending = spa_node_sync(impl->node, res);
	} else {
		complete_init(impl);
	}
	return this;

    clean_node:
	pw_node_destroy(this);
	return NULL;

}

void *pw_spa_node_get_user_data(struct pw_node *node)
{
	struct impl *impl = node->user_data;
	return impl->user_data;
}

static int
setup_props(struct pw_core *core, struct spa_node *spa_node, struct pw_properties *pw_props)
{
	int res;
	struct spa_pod *props;
	void *state = NULL;
	const char *key;
	uint32_t index = 0;
	uint8_t buf[2048];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod_prop *prop = NULL;

	res = spa_node_enum_params_sync(spa_node,
					SPA_PARAM_Props, &index, NULL, &props,
					&b);
	if (res != 1) {
		if (res < 0)
			pw_log_debug("spa_node_get_props failed: %d", res);
		return res;
	}

	while ((key = pw_properties_iterate(pw_props, &state))) {
		uint32_t type = 0;

		type = spa_debug_type_find_type(NULL, key);
		if (type == SPA_TYPE_None)
			continue;

		if ((prop = spa_pod_find_prop(props, prop, type))) {
			const char *value = pw_properties_get(pw_props, key);

			pw_log_info("configure prop %s", key);

			switch(prop->value.type) {
			case SPA_TYPE_Bool:
				SPA_POD_VALUE(struct spa_pod_bool, &prop->value) =
					pw_properties_parse_bool(value);
				break;
			case SPA_TYPE_Id:
				SPA_POD_VALUE(struct spa_pod_id, &prop->value) =
					spa_debug_type_find_type(NULL, value);
				break;
			case SPA_TYPE_Int:
				SPA_POD_VALUE(struct spa_pod_int, &prop->value) =
					pw_properties_parse_int(value);
				break;
			case SPA_TYPE_Long:
				SPA_POD_VALUE(struct spa_pod_long, &prop->value) =
					pw_properties_parse_int64(value);
				break;
			case SPA_TYPE_Float:
				SPA_POD_VALUE(struct spa_pod_float, &prop->value) =
					pw_properties_parse_float(value);
				break;
			case SPA_TYPE_Double:
				SPA_POD_VALUE(struct spa_pod_double, &prop->value) =
					pw_properties_parse_double(value);
				break;
			case SPA_TYPE_String:
				break;
			default:
				break;
			}
		}
	}

	if ((res = spa_node_set_param(spa_node, SPA_PARAM_Props, 0, props)) < 0) {
		pw_log_debug("spa_node_set_props failed: %d", res);
		return res;
	}
	return 0;
}


struct pw_node *pw_spa_node_load(struct pw_core *core,
				 struct pw_client *owner,
				 struct pw_global *parent,
				 const char *lib,
				 const char *factory_name,
				 const char *name,
				 enum pw_spa_node_flags flags,
				 struct pw_properties *properties,
				 size_t user_data_size)
{
	struct pw_node *this;
	struct impl *impl;
	struct spa_node *spa_node;
	int res;
	struct spa_handle *handle;
	void *hnd;
	uint32_t index;
	spa_handle_factory_enum_func_t enum_func;
	const struct spa_handle_factory *factory;
	void *iface;
	char *filename;
	const char *dir;
	const struct spa_support *support;
	uint32_t n_support;

	if ((dir = getenv("SPA_PLUGIN_DIR")) == NULL)
		dir = PLUGINDIR;

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

	support = pw_core_get_support(core, &n_support);

        handle = calloc(1, spa_handle_factory_get_size(factory, NULL));
	if (handle == NULL)
		goto no_mem;

	if ((res = spa_handle_factory_init(factory,
					   handle,
					   properties ? &properties->dict : NULL,
					   support,
					   n_support)) < 0) {
		pw_log_error("can't make factory instance: %d", res);
		goto init_failed;
	}

	if (SPA_RESULT_IS_ASYNC(res))
		flags |= PW_SPA_NODE_FLAG_ASYNC;

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node, &iface)) < 0) {
		pw_log_error("can't get node interface %d", res);
		goto interface_failed;
	}
	spa_node = iface;

	if (properties != NULL) {
		if (setup_props(core, spa_node, properties) < 0) {
			pw_log_debug("Unrecognized properties");
		}
	}

	this = pw_spa_node_new(core, owner, parent, name, flags,
			       spa_node, handle, properties, user_data_size);

	impl = this->user_data;
	impl->hnd = hnd;
	impl->handle = handle;
	impl->lib = filename;
	impl->factory_name = strdup(factory_name);

	return this;

      interface_failed:
	spa_handle_clear(handle);
      init_failed:
	free(handle);
      enum_failed:
      no_mem:
      no_symbol:
	dlclose(hnd);
      open_failed:
	free(filename);
	return NULL;
}
