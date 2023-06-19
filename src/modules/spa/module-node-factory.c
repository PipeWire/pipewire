/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include <spa/utils/result.h>

#include "config.h"

#include "pipewire/impl.h"

#include "spa-node.h"

#define NAME "spa-node-factory"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define FACTORY_USAGE	SPA_KEY_FACTORY_NAME"=<factory-name> " \
			"["SPA_KEY_LIBRARY_NAME"=<library-name>]"

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Provide a factory to make SPA nodes" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct factory_data {
	struct pw_context *context;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_impl_factory *factory;
	struct spa_hook factory_listener;

	struct spa_list node_list;
};

struct node_data {
	struct factory_data *data;
	struct spa_list link;
	struct pw_impl_node *node;
	struct spa_hook node_listener;
	struct pw_resource *resource;
	struct spa_hook resource_listener;
	unsigned int linger:1;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;
	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
};

static void proxy_removed(void *_data)
{
	struct node_data *nd = _data;
	pw_log_debug("%p: removed", nd);
	pw_proxy_destroy(nd->proxy);
}

static void proxy_destroy(void *_data)
{
	struct node_data *nd = _data;
	pw_log_debug("%p: destroy", nd);
	spa_hook_remove(&nd->proxy_listener);
	nd->proxy = NULL;
	if (nd->node)
		pw_impl_node_destroy(nd->node);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = proxy_removed,
	.destroy = proxy_destroy,
};

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct node_data *nd = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_node_destroy(nd->node);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static void core_removed(void *d)
{
	struct node_data *nd = d;
	pw_log_debug("%p: removed", nd);
	spa_hook_remove(&nd->core_proxy_listener);
	spa_hook_remove(&nd->core_listener);
	nd->core = NULL;
	if (nd->node)
		pw_impl_node_destroy(nd->node);
}

static const struct pw_proxy_events core_proxy_events = {
	.removed = core_removed,
};

static int export_node(struct node_data *nd, struct pw_properties *props)
{
	const char *str;

	str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
	nd->core = pw_context_connect(nd->data->context,
			pw_properties_new(
				PW_KEY_REMOTE_NAME, str,
				NULL),
			0);
	if (nd->core == NULL) {
		pw_log_error("can't connect: %m");
		return -errno;
	}
	pw_proxy_add_listener((struct pw_proxy*)nd->core,
			&nd->core_proxy_listener,
			&core_proxy_events, nd);
	pw_core_add_listener(nd->core,
			&nd->core_listener,
			&core_events, nd);

	pw_log_debug("%p: export node %p", nd, nd->node);
	nd->proxy = pw_core_export(nd->core,
			PW_TYPE_INTERFACE_Node, NULL, nd->node, 0);
	if (nd->proxy == NULL)
		return -errno;

	pw_proxy_add_listener(nd->proxy, &nd->proxy_listener, &proxy_events, nd);

	return 0;
}

static void resource_destroy(void *data)
{
	struct node_data *nd = data;
	pw_log_debug("node %p", nd);
	spa_hook_remove(&nd->resource_listener);
	nd->resource = NULL;
	if (nd->node && !nd->linger)
		pw_impl_node_destroy(nd->node);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = resource_destroy
};

static void node_destroy(void *data)
{
	struct node_data *nd = data;
	pw_log_debug("node %p", nd);
	spa_list_remove(&nd->link);
	spa_hook_remove(&nd->node_listener);
	nd->node = NULL;

	if (nd->resource) {
		spa_hook_remove(&nd->resource_listener);
		nd->resource = NULL;
	}
	if (nd->core) {
		pw_core_disconnect(nd->core);
		nd->core = NULL;
	}
}

static const struct pw_impl_node_events node_events = {
	PW_VERSION_IMPL_NODE_EVENTS,
	.destroy = node_destroy,
};

static void *create_object(void *_data,
			   struct pw_resource *resource,
			   const char *type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	struct factory_data *data = _data;
	struct pw_context *context = data->context;
	struct pw_impl_node *node;
	const char *factory_name;
	struct node_data *nd;
	int res;
	struct pw_impl_client *client;
	bool linger;

	if (properties == NULL)
		goto error_properties;

	factory_name = pw_properties_get(properties, SPA_KEY_FACTORY_NAME);
	if (factory_name == NULL)
		goto error_properties;

	pw_properties_setf(properties, PW_KEY_FACTORY_ID, "%d",
			pw_global_get_id(pw_impl_factory_get_global(data->factory)));

	linger = pw_properties_get_bool(properties, PW_KEY_OBJECT_LINGER, false);

	client = resource ? pw_resource_get_client(resource) : NULL;
	if (client && !linger) {
		pw_properties_setf(properties, PW_KEY_CLIENT_ID, "%d",
			pw_global_get_id(pw_impl_client_get_global(client)));
	}
	node = pw_spa_node_load(context,
				factory_name,
				PW_SPA_NODE_FLAG_ACTIVATE,
				properties,
				sizeof(struct node_data));
	if (node == NULL)
		goto error_create_node;

	nd = pw_spa_node_get_user_data(node);
	nd->data = data;
	nd->node = node;
	nd->linger = linger;
	spa_list_append(&data->node_list, &nd->link);

	pw_impl_node_add_listener(node, &nd->node_listener, &node_events, nd);

	if (client) {
		res = pw_global_bind(pw_impl_node_get_global(node),
			       client, PW_PERM_ALL, version, new_id);
		if (res < 0)
			goto error_bind;

		if ((nd->resource = pw_impl_client_find_resource(client, new_id)) == NULL)
			goto error_bind;

		pw_resource_add_listener(nd->resource, &nd->resource_listener, &resource_events, nd);
	}
	if (pw_properties_get_bool(properties, PW_KEY_OBJECT_EXPORT, false)) {
		res = export_node(nd, properties);
		if (res < 0)
			goto error_export;
	}
	return node;

error_properties:
	res = -EINVAL;
	pw_resource_errorf_id(resource, new_id, res, "usage: "FACTORY_USAGE);
	goto error_exit_cleanup;
error_create_node:
	res = -errno;
	pw_resource_errorf_id(resource, new_id, res,
				"can't create node: %s", spa_strerror(res));
	goto error_exit;
error_bind:
	pw_resource_errorf_id(resource, new_id, res, "can't bind node");
	pw_impl_node_destroy(node);
	goto error_exit;
error_export:
	pw_resource_errorf_id(resource, new_id, res, "can't export node");
	pw_impl_node_destroy(node);
	goto error_exit;

error_exit_cleanup:
	pw_properties_free(properties);
error_exit:
	errno = -res;
	return NULL;
}

static const struct pw_impl_factory_implementation factory_impl = {
	PW_VERSION_IMPL_FACTORY_IMPLEMENTATION,
	.create_object = create_object,
};

static void factory_destroy(void *data)
{
	struct factory_data *d = data;
	struct node_data *nd;

	spa_hook_remove(&d->factory_listener);
	spa_list_consume(nd, &d->node_list, link)
		pw_impl_node_destroy(nd->node);
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

	snprintf(id, sizeof(id), "%d", pw_global_get_id(pw_impl_module_get_global(module)));
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
				 "spa-node-factory",
				 PW_TYPE_INTERFACE_Node,
				 PW_VERSION_NODE,
				 NULL,
				 sizeof(*data));
	if (factory == NULL)
		return -errno;

	data = pw_impl_factory_get_user_data(factory);
	data->factory = factory;
	data->context = context;
	data->module = module;
	spa_list_init(&data->node_list);

	pw_impl_factory_add_listener(factory, &data->factory_listener, &factory_events, data);
	pw_impl_factory_set_implementation(factory, &factory_impl, data);

	pw_log_debug("module %p: new", module);
	pw_impl_module_add_listener(module, &data->module_listener, &module_events, data);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
}
