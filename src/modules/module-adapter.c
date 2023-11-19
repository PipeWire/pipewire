/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include "config.h"

#include <spa/node/node.h>
#include <spa/utils/hook.h>
#include <spa/utils/result.h>

#include <pipewire/impl.h>

#include "modules/spa/spa-node.h"
#include "module-adapter/adapter.h"

/** \page page_module_adapter Adapter
 *
 * ## Module Name
 *
 * `libpipewire-module-adapter`
 */
#define NAME "adapter"

PW_LOG_TOPIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define FACTORY_USAGE	SPA_KEY_FACTORY_NAME"=<factory-name> " \
			"("SPA_KEY_LIBRARY_NAME"=<library-name>) " \
			ADAPTER_USAGE

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Manage adapter nodes" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct factory_data {
	struct pw_impl_factory *factory;
	struct spa_hook factory_listener;

	struct spa_list node_list;

	struct pw_context *context;
	struct pw_impl_module *module;
	struct spa_hook module_listener;
};

struct node_data {
	struct factory_data *data;
	struct spa_list link;
	struct pw_impl_node *adapter;
	struct pw_impl_node *follower;
	struct spa_handle *handle;
	struct spa_hook adapter_listener;
	struct pw_resource *resource;
	struct pw_resource *bound_resource;
	struct spa_hook resource_listener;
	uint32_t new_id;
	unsigned int linger;
};

static void resource_destroy(void *data)
{
	struct node_data *nd = data;

	pw_log_debug("%p: destroy %p", nd, nd->adapter);
	spa_hook_remove(&nd->resource_listener);
	nd->bound_resource = NULL;
	if (nd->adapter && !nd->linger)
		pw_impl_node_destroy(nd->adapter);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = resource_destroy
};

static void node_destroy(void *data)
{
	struct node_data *nd = data;
	pw_log_debug("%p: destroy %p", nd, nd->adapter);
	spa_list_remove(&nd->link);
	nd->adapter = NULL;
}

static void node_free(void *data)
{
	struct node_data *nd = data;

	pw_log_debug("%p: free %p", nd, nd->follower);

	if (nd->bound_resource != NULL)
		spa_hook_remove(&nd->resource_listener);

	spa_hook_remove(&nd->adapter_listener);

	if (nd->follower)
		pw_impl_node_destroy(nd->follower);
	if (nd->handle)
		pw_unload_spa_handle(nd->handle);
}

static void node_initialized(void *data)
{
	struct node_data *nd = data;
	struct pw_impl_client *client;
	struct pw_resource *bound_resource;
	struct pw_global *global;
	int res;

	if (nd->resource == NULL)
		return;

	client = pw_resource_get_client(nd->resource);
	global = pw_impl_node_get_global(nd->adapter);

	res = pw_global_bind(global, client,
			PW_PERM_ALL, PW_VERSION_NODE, nd->new_id);
	if (res < 0)
		goto error_bind;

	if ((bound_resource = pw_impl_client_find_resource(client, nd->new_id)) == NULL) {
		res = -EIO;
		goto error_bind;
	}

	nd->bound_resource = bound_resource;
	pw_resource_add_listener(bound_resource, &nd->resource_listener, &resource_events, nd);
	return;

error_bind:
	pw_resource_errorf_id(nd->resource, nd->new_id, res,
			"can't bind adapter node: %s", spa_strerror(res));
	return;
}


static const struct pw_impl_node_events node_events = {
	PW_VERSION_IMPL_NODE_EVENTS,
	.destroy = node_destroy,
	.free = node_free,
	.initialized = node_initialized,
};

static void *create_object(void *_data,
			   struct pw_resource *resource,
			   const char *type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	struct factory_data *d = _data;
	struct pw_impl_client *client;
	struct pw_impl_node *adapter, *follower;
	struct spa_node *spa_follower;
	const char *str;
	int res;
	struct node_data *nd;
	bool linger, do_register;
	struct spa_handle *handle = NULL;
	const struct pw_properties *p;

	if (properties == NULL)
		goto error_properties;

	pw_properties_setf(properties, PW_KEY_FACTORY_ID, "%d",
			pw_impl_factory_get_info(d->factory)->id);

	linger = pw_properties_get_bool(properties, PW_KEY_OBJECT_LINGER, false);
	do_register = pw_properties_get_bool(properties, PW_KEY_OBJECT_REGISTER, true);

	p = pw_context_get_properties(d->context);
	pw_properties_set(properties, "clock.quantum-limit",
			pw_properties_get(p, "default.clock.quantum-limit"));

	client = resource ? pw_resource_get_client(resource): NULL;
	if (client && !linger) {
		pw_properties_setf(properties, PW_KEY_CLIENT_ID, "%d",
				pw_impl_client_get_info(client)->id);
	}

	follower = NULL;
	spa_follower = NULL;
	str = pw_properties_get(properties, "adapt.follower.node");
	if (str != NULL) {
		if (sscanf(str, "pointer:%p", &follower) != 1)
			goto error_properties;
		spa_follower = pw_impl_node_get_implementation(follower);
	}
	str = pw_properties_get(properties, "adapt.follower.spa-node");
	if (str != NULL) {
		if (sscanf(str, "pointer:%p", &spa_follower) != 1)
			goto error_properties;
	}
	if (spa_follower == NULL) {
		void *iface;
		const char *factory_name;

		factory_name = pw_properties_get(properties, SPA_KEY_FACTORY_NAME);
		if (factory_name == NULL)
			goto error_properties;

		handle = pw_context_load_spa_handle(d->context,
				factory_name,
				&properties->dict);
		if (handle == NULL)
			goto error_errno;

		if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node, &iface)) < 0)
			goto error_res;

		spa_follower = iface;
	}
	if (spa_follower == NULL) {
		res = -EINVAL;
		goto error_res;
	}

	adapter = pw_adapter_new(pw_impl_module_get_context(d->module),
			spa_follower,
			properties,
			sizeof(struct node_data));
	properties = NULL;

	if (adapter == NULL) {
		if (errno == ENOMEM || errno == EBUSY)
			goto error_errno;
		else
			goto error_usage;
	}

	nd = pw_adapter_get_user_data(adapter);
	nd->data = d;
	nd->adapter = adapter;
	nd->follower = follower;
	nd->handle = handle;
	nd->resource = resource;
	nd->new_id = new_id;
	nd->linger = linger;
	spa_list_append(&d->node_list, &nd->link);

	pw_impl_node_add_listener(adapter, &nd->adapter_listener, &node_events, nd);

	if (do_register)
		pw_impl_node_register(adapter, NULL);
	else
		pw_impl_node_initialized(adapter);

	return adapter;

error_properties:
	res = -EINVAL;
	pw_resource_errorf_id(resource, new_id, res, "usage: " FACTORY_USAGE);
	goto error_cleanup;
error_errno:
	res = -errno;
error_res:
	pw_resource_errorf_id(resource, new_id, res, "can't create node: %s", spa_strerror(res));
	goto error_cleanup;
error_usage:
	res = -EINVAL;
	pw_log_error("usage: "ADAPTER_USAGE);
	pw_resource_errorf_id(resource, new_id, res, "usage: "ADAPTER_USAGE);
	goto error_cleanup;
error_cleanup:
	pw_properties_free(properties);
	if (handle)
		pw_unload_spa_handle(handle);
	errno = -res;
	return NULL;
}

static const struct pw_impl_factory_implementation impl_factory = {
	PW_VERSION_IMPL_FACTORY_IMPLEMENTATION,
	.create_object = create_object,
};

static void factory_destroy(void *data)
{
	struct factory_data *d = data;
	struct node_data *nd;

	spa_hook_remove(&d->factory_listener);

	spa_list_consume(nd, &d->node_list, link)
		pw_impl_node_destroy(nd->adapter);

	d->factory = NULL;
	if (d->module)
		pw_impl_module_destroy(d->module);
}

static const struct pw_impl_factory_events factory_events = {
	PW_VERSION_IMPL_FACTORY_EVENTS,
	.destroy = factory_destroy,
};

static void module_destroy(void *data)
{
	struct factory_data *d = data;

	pw_log_debug("%p: destroy", d);
	spa_hook_remove(&d->module_listener);
	d->module = NULL;

	if (d->factory)
		pw_impl_factory_destroy(d->factory);
}

static void module_registered(void *data)
{
	struct factory_data *d = data;
	struct pw_impl_module *module = d->module;
	struct pw_impl_factory *factory = d->factory;
	struct spa_dict_item items[1];
	char id[16];
	int res;

	snprintf(id, sizeof(id), "%d", pw_impl_module_get_info(module)->id);
	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_MODULE_ID, id);
	pw_impl_factory_update_properties(factory, &SPA_DICT_INIT(items, 1));

	if ((res = pw_impl_factory_register(factory, NULL)) < 0) {
		pw_log_error("%p: can't register factory: %s", factory, spa_strerror(res));
	}
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
	.registered = module_registered,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_impl_factory *factory;
	struct factory_data *data;

	PW_LOG_TOPIC_INIT(mod_topic);

	factory = pw_context_create_factory(context,
				 "adapter",
				 PW_TYPE_INTERFACE_Node,
				 PW_VERSION_NODE,
				 pw_properties_new(
					 PW_KEY_FACTORY_USAGE, FACTORY_USAGE,
					 NULL),
				 sizeof(*data));
	if (factory == NULL)
		return -errno;

	data = pw_impl_factory_get_user_data(factory);
	data->factory = factory;
	data->context = context;
	data->module = module;
	spa_list_init(&data->node_list);

	pw_log_debug("module %p: new", module);

	pw_impl_factory_add_listener(factory, &data->factory_listener,
			&factory_events, data);
	pw_impl_factory_set_implementation(factory,
				      &impl_factory,
				      data);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	pw_impl_module_add_listener(module, &data->module_listener, &module_events, data);

	return 0;
}
