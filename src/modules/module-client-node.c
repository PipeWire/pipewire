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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include "config.h"

#include <pipewire/pipewire.h>

#include "module-client-node/client-node.h"

#define NAME "client-node"

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Allow clients to create and control remote nodes" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct pw_proxy *pw_remote_node_export(struct pw_remote *remote,
		uint32_t type, struct pw_properties *props, void *object, size_t user_data_size);
struct pw_proxy *pw_remote_spa_node_export(struct pw_remote *remote,
		uint32_t type, struct pw_properties *props, void *object, size_t user_data_size);

struct pw_protocol *pw_protocol_native_ext_client_node_init(struct pw_core *core);

struct factory_data {
	struct pw_factory *this;

	struct pw_module *module;
	struct spa_hook module_listener;

	struct pw_export_type export_node;
	struct pw_export_type export_spanode;
};

static void *create_object(void *_data,
			   struct pw_resource *resource,
			   uint32_t type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	void *result;
	struct pw_resource *node_resource;
	struct pw_client *client = pw_resource_get_client(resource);
	int res;

	node_resource = pw_resource_new(client, new_id, PW_PERM_RWX, type, version, 0);
	if (node_resource == NULL) {
		res = -errno;
		goto error_resource;
	}

	result = pw_client_node_new(node_resource, properties, true);
	if (result == NULL) {
		res = -errno;
		goto error_node;
	}
	return result;

error_resource:
	pw_log_error("can't create resource: %s", spa_strerror(res));
	pw_resource_error(resource, res, "can't create resource: %s", spa_strerror(res));
	goto error_exit;
error_node:
	pw_log_error("can't create node: %s", spa_strerror(res));
	pw_resource_error(resource, res, "can't create node: %s", spa_strerror(res));
	goto error_exit_free;

error_exit_free:
	pw_resource_destroy(node_resource);
error_exit:
	errno = -res;
	return NULL;
}

static const struct pw_factory_implementation impl_factory = {
	PW_VERSION_FACTORY_IMPLEMENTATION,
	.create_object = create_object,
};

static void module_destroy(void *data)
{
	struct factory_data *d = data;

	spa_hook_remove(&d->module_listener);

	spa_list_remove(&d->export_node.link);
	spa_list_remove(&d->export_spanode.link);

	pw_factory_destroy(d->this);
}

static void module_registered(void *data)
{
	struct factory_data *d = data;
	struct pw_module *module = d->module;
	struct pw_factory *factory = d->this;
	struct spa_dict_item items[1];
	char id[16];
	int res;

	snprintf(id, sizeof(id), "%d", pw_global_get_id(pw_module_get_global(module)));
	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_MODULE_ID, id);
	pw_factory_update_properties(factory, &SPA_DICT_INIT(items, 1));

	if ((res = pw_factory_register(factory, NULL)) < 0) {
		pw_log_error(NAME" %p: can't register factory: %s", factory, spa_strerror(res));
	}
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
	.registered = module_registered,
};

SPA_EXPORT
int pipewire__module_init(struct pw_module *module, const char *args)
{
	struct pw_core *core = pw_module_get_core(module);
	struct pw_factory *factory;
	struct factory_data *data;

	factory = pw_factory_new(core,
				 "client-node",
				 PW_TYPE_INTERFACE_ClientNode,
				 PW_VERSION_CLIENT_NODE,
				 NULL,
				 sizeof(*data));
	if (factory == NULL)
		return -errno;

	data = pw_factory_get_user_data(factory);
	data->this = factory;
	data->module = module;

	pw_log_debug("module %p: new", module);

	pw_factory_set_implementation(factory,
				      &impl_factory,
				      data);

	pw_protocol_native_ext_client_node_init(core);

	data->export_node.type = PW_TYPE_INTERFACE_Node;
	data->export_node.func = pw_remote_node_export;
	pw_core_register_export_type(core, &data->export_node);

	data->export_spanode.type = SPA_TYPE_INTERFACE_Node;
	data->export_spanode.func = pw_remote_spa_node_export;
	pw_core_register_export_type(core, &data->export_spanode);

	pw_module_add_listener(module, &data->module_listener, &module_events, data);

	pw_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
}
