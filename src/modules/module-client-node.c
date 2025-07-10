/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include <spa/utils/result.h>

#include <pipewire/impl.h>

#define PW_API_CLIENT_NODE_IMPL	SPA_EXPORT
#include "module-client-node/client-node.h"

/** \page page_module_client_node Client Node
 *
 * Allow clients to export processing nodes to the PipeWire daemon.
 *
 * This module creates 2 export types, one for the \ref PW_TYPE_INTERFACE_Node and
 * another for the \ref SPA_TYPE_INTERFACE_Node interfaces.
 *
 * With \ref pw_core_export(), objects of these types can be exported to the
 * PipeWire server. All actions performed on the node locally will be visible
 * to connecteced clients and scheduling of the Node will be performed.
 *
 * Objects of the \ref PW_TYPE_INTERFACE_Node interface can be made with
 * \ref pw_context_create_node(), for example. You would manually need to create
 * and add an object of the \ref SPA_TYPE_INTERFACE_Node interface. Exporting a
 * \ref SPA_TYPE_INTERFACE_Node directly will first wrap it in a
 * \ref PW_TYPE_INTERFACE_Node interface.
 *
 * Usually this module is not used directly but through the \ref pw_stream and
 * \ref pw_filter APIs, which provides API to implement the \ref SPA_TYPE_INTERFACE_Node
 * interface.
 *
 * In some cases, it is possible to use this factory directly (the PipeWire JACK
 * implementation does this). With \ref pw_core_create_object() on the `client-node`
 * factory will result in a \ref PW_TYPE_INTERFACE_ClientNode proxy that can be
 * used to control the server side created \ref pw_impl_node.
 *
 * Schematically, the client side \ref pw_impl_node is wrapped in the ClientNode
 * proxy and unwrapped by the server side resource so that all actions on the client
 * side node are reflected on the server side node and server side actions are
 * reflected in the client.
 *
 *\code{.unparsed}
 *
 *   client side proxy                            server side resource
 * .------------------------------.            .----------------------------------.
 * | PW_TYPE_INTERFACE_ClientNode |            |  PW_TYPE_INTERFACE_Node          |
 * |.----------------------------.|  IPC       |.--------------------------------.|
 * || PW_TYPE_INTERFACE_Node     || ----->     || SPA_TYPE_INTERFACE_Node        ||
 * ||.--------------------------.||            ||.------------------------------.||
 * ||| SPA_TYPE_INTERFACE_Node  |||            ||| PW_TYPE_INTERFACE_ClientNode |||
 * |||                          |||            |||                              |||
 * ||'--------------------------'||            ||'------------------------------'||
 * |'----------------------------'|            |'--------------------------------'|
 * '------------------------------'            '----------------------------------'
 *\endcode
 *
 * ## Module Name
 *
 * `libpipewire-module-client-node`
 *
 * ## Module Options
 *
 * This module has no options.
 *
 * ## Properties for the create_object call
 *
 * All properties are passed directly to the \ref pw_context_create_node() call.
 *
 * ## Example configuration
 *
 * The module is usually added to the config file of the main PipeWire daemon and the
 * clients.
 *
 *\code{.unparsed}
 * context.modules = [
 * { name = libpipewire-module-client-node }
 * ]
 *\endcode
 *
 * ## See also
 *
 * - `module-spa-node-factory`: make nodes from a factory
 */

#define NAME "client-node"

PW_LOG_TOPIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Allow clients to create and control remote nodes" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct pw_proxy *pw_core_node_export(struct pw_core *core,
		const char *type, const struct spa_dict *props, void *object, size_t user_data_size);
struct pw_proxy *pw_core_spa_node_export(struct pw_core *core,
		const char *type, const struct spa_dict *props, void *object, size_t user_data_size);

struct pw_protocol *pw_protocol_native_ext_client_node_init(struct pw_context *context);

struct factory_data {
	struct pw_impl_factory *factory;
	struct spa_hook factory_listener;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_export_type export_node;
	struct pw_export_type export_spanode;
};

static void *create_object(void *_data,
			   struct pw_resource *resource,
			   const char *type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	void *result;
	struct pw_resource *node_resource;
	struct pw_impl_client *client;
	int res;

	if (resource == NULL) {
		res = -EINVAL;
		goto error_exit;
	}

	client = pw_resource_get_client(resource);
	node_resource = pw_resource_new(client, new_id, PW_PERM_ALL, type, version, 0);
	if (node_resource == NULL) {
		res = -errno;
		goto error_resource;
	}

	if (version == 0) {
		result = NULL;
		errno = ENOTSUP;
	} else {
		result = pw_impl_client_node_new(node_resource, properties, true);
	}
	if (result == NULL) {
		res = -errno;
		goto error_node;
	}
	return result;

error_resource:
	pw_log_error("can't create resource: %s", spa_strerror(res));
	pw_resource_errorf_id(resource, new_id, res, "can't create resource: %s", spa_strerror(res));
	goto error_exit;
error_node:
	pw_log_error("can't create node: %s", spa_strerror(res));
	pw_resource_errorf_id(resource, new_id, res, "can't create node: %s", spa_strerror(res));
	goto error_exit;
error_exit:
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
	spa_hook_remove(&d->factory_listener);
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
	spa_list_remove(&d->export_node.link);
	spa_list_remove(&d->export_spanode.link);

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
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	factory = pw_context_create_factory(context,
				 "client-node",
				 PW_TYPE_INTERFACE_ClientNode,
				 PW_VERSION_CLIENT_NODE,
				 NULL,
				 sizeof(*data));
	if (factory == NULL)
		return -errno;

	data = pw_impl_factory_get_user_data(factory);
	data->factory = factory;
	data->module = module;

	pw_log_debug("module %p: new", module);

	pw_impl_factory_set_implementation(factory,
				      &impl_factory,
				      data);

	data->export_node.type = PW_TYPE_INTERFACE_Node;
	data->export_node.func = pw_core_node_export;
	if ((res = pw_context_register_export_type(context, &data->export_node)) < 0)
		goto error;

	data->export_spanode.type = SPA_TYPE_INTERFACE_Node;
	data->export_spanode.func = pw_core_spa_node_export;
	if ((res = pw_context_register_export_type(context, &data->export_spanode)) < 0)
		goto error_remove;

	pw_protocol_native_ext_client_node_init(context);

	pw_impl_factory_add_listener(factory, &data->factory_listener, &factory_events, data);
	pw_impl_module_add_listener(module, &data->module_listener, &module_events, data);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
error_remove:
	spa_list_remove(&data->export_node.link);
error:
	pw_impl_factory_destroy(data->factory);
	return res;
}
