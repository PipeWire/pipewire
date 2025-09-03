/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/impl.h>

/** \page page_module_link_factory Link Factory
 *
 * Allows clients to create links between ports.
 *
 * This module creates a new factory. Clients that can see the factory
 * can use the factory name (`link-factory`) to create new link
 * objects with \ref pw_core_create_object(). It is also possible to create
 * objects in the config file.
 *
 * Object of the \ref PW_TYPE_INTERFACE_Link will be created and a proxy
 * to it will be returned.
 *
 * As an argument to the create_object call, a set of properties will
 * control what ports will be linked.
 *
 * ## Module Name
 *
 * `libpipewire-module-link-factory`
 *
 * ## Module Options
 *
 * - `allow.link.passive`: if the `link.passive` property is allowed. Default false.
 *                        By default, the core will decide when a link is passive
 *                        based on the properties of the node and ports.
 *
 * ## Properties for the create_object call
 *
 * - `link.output.node`: The output node to use. This can be the node object.id, node.name,
 *                     node.nick, node.description or object.path of a node. When the
 *                     property is not given or NULL, the output port should be
 *                     specified.
 * - `link.output.port`: The output port to link. This can be a port object.id, port.name,
 *                     port.alias or object.path. If an output node is specified, the
 *                     port must belong to the node. Finding a port in a node using the
 *                     port.id is deprecated and may lead to unexpected results when the
 *                     port.id also matches an object.id. If no output port is given, an
 *                     output node must be specified and a random (unlinked) port will
 *                     be used from the node.
 * - `link.input.node`: The input node to use. This can be the node object.id, node.name,
 *                     node.nick, node.description or object.path of a node. When the
 *                     property is not given or NULL, the input port should be
 *                     specified.
 * - `link.input.port`: The input port to link. This can be a port object.id, port.name,
 *                     port.alias or object.path. If an input node is specified, the
 *                     port must belong to the node. Finding a port in a node using the
 *                     port.id is deprecated and may lead to unexpected results when the
 *                     port.id also matches an object.id. If no input port is given, an
 *                     input node must be specified and a random (unlinked) port will
 *                     be used from the node.
 * - `object.linger`: Keep the link around even when the client that created it is gone.
 * - `link.passive`: The link is passive, meaning that it will not keep nodes busy.
 *                 By default this property is ignored and the node and port properties
 *                 are used to determine the passive state of the link.
 *
 * ## Example configuration
 *
 * The module is usually added to the config file of the main pipewire daemon.
 *
 *\code{.unparsed}
 * context.modules = [
 * { name = libpipewire-link-factory
 *   args = {
 *       #allow.link.passive = false
 *   }
 * }
 * ]
 *\endcode

 * ## Config override
 *
 * A `module.link-factory.args` config section can be added
 * to override the module arguments.
 *
 *\code{.unparsed}
 * # ~/.config/pipewire/pipewire.conf.d/my-link-factory-args.conf
 *
 * module.link-factory.args = {
 *     #allow.link.passive = false
 * }
 *\endcode
 *
 * ## Config objects
 *
 * To create an object from the factory, one can use the \ref pw_core_create_object()
 * method or make an object in the `context.objects` section like:
 *
 *\code{.unparsed}
 * context.objects = [
 * { factory = link-factory
 *     args = {
 *         link.output.node = system
 *         link.output.port = capture_2
 *         link.input.node  = my-mic
 *         link.input.port  = input_FR
 *     }
 * }
 *\endcode
 *
 * Note that this only works when the ports that need to be linked are available
 * at the time the config file is parsed.
 *
 * ## See also
 *
 * - `pw-link`: a tool to manage port links
 */

#define NAME "link-factory"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define FACTORY_USAGE	"("PW_KEY_LINK_OUTPUT_NODE"=<output-node>) "	\
			"("PW_KEY_LINK_OUTPUT_PORT"=<output-port>) "	\
			"("PW_KEY_LINK_INPUT_NODE"=<input-node>) "	\
			"("PW_KEY_LINK_INPUT_PORT"=<input-port>) "	\
			"("PW_KEY_OBJECT_LINGER"=<bool>) "		\
			"("PW_KEY_LINK_PASSIVE"=<bool>)"

#define MODULE_USAGE	"( allow.link.passive=<bool, default false> ) "

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Allow clients to create links" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct factory_data {
	struct pw_context *context;

	struct pw_properties *props;

	unsigned int allow_passive:1;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_impl_factory *factory;
	struct spa_hook factory_listener;

	struct spa_list link_list;

	struct pw_work_queue *work;
};

struct link_data {
	struct factory_data *data;
	struct spa_list l;
	struct pw_impl_link *link;
	struct spa_hook link_listener;

	struct pw_resource *resource;
	struct spa_hook resource_listener;

	struct pw_global *global;
	struct spa_hook global_listener;

	struct pw_resource *factory_resource;
	uint32_t new_id;
	bool linger;
};

static void resource_destroy(void *data)
{
	struct link_data *ld = data;
	spa_hook_remove(&ld->resource_listener);
	ld->resource = NULL;
	if (ld->global)
		pw_global_destroy(ld->global);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = resource_destroy
};

static void global_destroy(void *data)
{
	struct link_data *ld = data;

	spa_hook_remove(&ld->global_listener);
	ld->global = NULL;
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy
};

static void link_destroy(void *data)
{
	struct link_data *ld = data;
	spa_list_remove(&ld->l);
	spa_hook_remove(&ld->link_listener);
	if (ld->global)
		spa_hook_remove(&ld->global_listener);
	if (ld->resource)
		spa_hook_remove(&ld->resource_listener);
	pw_work_queue_cancel(ld->data->work, ld, SPA_ID_INVALID);
}

static void link_initialized(void *data)
{
	struct link_data *ld = data;
	struct pw_impl_client *client;
	int res;

	if (ld->factory_resource == NULL)
		return;

	client = pw_resource_get_client(ld->factory_resource);
	ld->global = pw_impl_link_get_global(ld->link);
	pw_global_add_listener(ld->global, &ld->global_listener, &global_events, ld);

	res = pw_global_bind(ld->global, client, PW_PERM_ALL, PW_VERSION_LINK, ld->new_id);
	if (res < 0)
		goto error_bind;

	if (!ld->linger) {
		ld->resource = pw_impl_client_find_resource(client, ld->new_id);
		if (ld->resource == NULL) {
			res = -ENOENT;
			goto error_bind;
		}
		pw_resource_add_listener(ld->resource, &ld->resource_listener, &resource_events, ld);
	}
	return;

error_bind:
	pw_resource_errorf_id(ld->factory_resource, ld->new_id, res,
			"can't bind link: %s", spa_strerror(res));
}

static void destroy_link(void *obj, void *data, int res, uint32_t id)
{
	struct link_data *ld = data;
	if (ld->global)
		pw_global_destroy(ld->global);
}

static void link_state_changed(void *data, enum pw_link_state old,
		enum pw_link_state state, const char *error)
{
	struct link_data *ld = data;
	struct factory_data *d = ld->data;

	switch (state) {
	case PW_LINK_STATE_ERROR:
		pw_work_queue_add(d->work, ld, 0, destroy_link, ld);
		break;
	default:
		break;
	}
}

static const struct pw_impl_link_events link_events = {
	PW_VERSION_IMPL_LINK_EVENTS,
	.destroy = link_destroy,
	.initialized = link_initialized,
	.state_changed = link_state_changed
};

static struct pw_impl_port *get_port(struct pw_impl_node *node, enum spa_direction direction)
{
	struct pw_impl_port *p;

	p = pw_impl_node_find_port(node, direction, PW_ID_ANY);

	if (p == NULL || pw_impl_port_is_linked(p))
		p = pw_impl_node_get_free_port(node, direction);

	return p;
}

struct find_port {
	uint32_t id;
	const char *name;
	enum spa_direction direction;
	struct pw_impl_node *node;
	struct pw_impl_port *port;
};

static int find_port_func(void *data, struct pw_global *global)
{
	struct find_port *find = data;
	const char *str;
	const struct pw_properties *props;

	if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Port))
		return 0;
	if (pw_global_get_id(global) == find->id)
		goto found;

	props = pw_global_get_properties(global);
	if ((str = pw_properties_get(props, PW_KEY_OBJECT_PATH)) != NULL &&
	    spa_streq(str, find->name))
		goto found;
	return 0;
found:
	find->port = pw_global_get_object(global);
	return 1;
}

static int find_node_port_func(void *data, struct pw_impl_port *port)
{
	struct find_port *find = data;
	const char *str;
	const struct pw_properties *props;

	if (pw_impl_port_get_id(port) == find->id)
		goto found;

	props = pw_impl_port_get_properties(port);
	if ((str = pw_properties_get(props, PW_KEY_PORT_NAME)) != NULL &&
	    spa_streq(str, find->name))
		goto found;
	if ((str = pw_properties_get(props, PW_KEY_PORT_ALIAS)) != NULL &&
	    spa_streq(str, find->name))
		goto found;
	if ((str = pw_properties_get(props, PW_KEY_OBJECT_PATH)) != NULL &&
	    spa_streq(str, find->name))
		goto found;
	return 0;
found:
	find->port = port;
	return 1;
}

static struct pw_impl_port *find_port(struct pw_context *context,
		struct pw_impl_node *node, enum spa_direction direction, const char *name)
{
	struct find_port find = {
		.id = SPA_ID_INVALID,
		.name = name,
		.direction = direction,
		.node = node
	};
	spa_atou32(name, &find.id, 0);

	if (find.id != SPA_ID_INVALID) {
		struct pw_global *global = pw_context_find_global(context, find.id);
		/* find port by global id */
		if (global != NULL && pw_global_is_type(global, PW_TYPE_INTERFACE_Port)) {
			find.port = pw_global_get_object(global);
			if (find.port != NULL &&
			    (node == NULL || pw_impl_port_get_node(find.port) == node))
				return find.port;
		}
	}
	if (node != NULL) {
		/* find port by local id (deprecated) */
		if (find.id != SPA_ID_INVALID) {
			find.port = pw_impl_node_find_port(node, find.direction, find.id);
			if (find.port != NULL)
				return find.port;
		}
		/* find port by local name */
		if (pw_impl_node_for_each_port(find.node, find.direction,
					find_node_port_func, &find) == 1)
			return find.port;

	} else {
		/* find port by name */
		if (pw_context_for_each_global(context, find_port_func, &find) == 1)
			return find.port;
	}
	return NULL;
}

struct find_node {
	uint32_t id;
	const char *name;
	struct pw_impl_node *node;
};

static int find_node_func(void *data, struct pw_global *global)
{
	struct find_node *find = data;
	const char *str;
	const struct pw_properties *props;

	if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Node))
		return 0;
	if (pw_global_get_id(global) == find->id)
		goto found;

	props = pw_global_get_properties(global);
	if ((str = pw_properties_get(props, PW_KEY_NODE_NAME)) != NULL &&
	    spa_streq(str, find->name))
		goto found;
	if ((str = pw_properties_get(props, PW_KEY_NODE_NICK)) != NULL &&
	    spa_streq(str, find->name))
		goto found;
	if ((str = pw_properties_get(props, PW_KEY_NODE_DESCRIPTION)) != NULL &&
	    spa_streq(str, find->name))
		goto found;
	if ((str = pw_properties_get(props, PW_KEY_OBJECT_PATH)) != NULL &&
	    spa_streq(str, find->name))
		goto found;
	return 0;
found:
	find->node = pw_global_get_object(global);
	return 1;
}

static struct pw_impl_node *find_node(struct pw_context *context, const char *name)
{
	struct find_node find = {
		.id = SPA_ID_INVALID,
		.name = name,
	};
	spa_atou32(name, &find.id, 0);

	if (find.id != SPA_ID_INVALID) {
		struct pw_global *global = pw_context_find_global(context, find.id);
		if (global != NULL && pw_global_is_type(global, PW_TYPE_INTERFACE_Node))
			return pw_global_get_object(global);
	}
	if (pw_context_for_each_global(context, find_node_func, &find) == 1)
		return find.node;
	return NULL;
}

static void *create_object(void *_data,
			   struct pw_resource *resource,
			   const char *type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	struct factory_data *d = _data;
	struct pw_impl_client *client = NULL;
	struct pw_impl_node *output_node, *input_node;
	struct pw_impl_port *outport = NULL, *inport = NULL;
	struct pw_context *context = d->context;
	struct pw_impl_link *link;
	const char *output_node_str, *input_node_str;
	const char *output_port_str, *input_port_str;
	struct link_data *ld;
	int res;
	bool linger;

	if (properties == NULL)
		goto error_properties;

	if ((output_node_str = pw_properties_get(properties, PW_KEY_LINK_OUTPUT_NODE)) != NULL)
		output_node = find_node(context, output_node_str);
	else
		output_node = NULL;

	if ((output_port_str = pw_properties_get(properties, PW_KEY_LINK_OUTPUT_PORT)) != NULL)
		outport = find_port(context, output_node, SPA_DIRECTION_OUTPUT, output_port_str);
	else if (output_node != NULL)
		outport = get_port(output_node, SPA_DIRECTION_OUTPUT);
	if (outport == NULL)
		goto error_output_port;

	if ((input_node_str = pw_properties_get(properties, PW_KEY_LINK_INPUT_NODE)) != NULL)
		input_node = find_node(context, input_node_str);
	else
		input_node = NULL;

	if ((input_port_str = pw_properties_get(properties, PW_KEY_LINK_INPUT_PORT)) != NULL)
		inport = find_port(context, input_node, SPA_DIRECTION_INPUT, input_port_str);
	else if (input_node != NULL)
		inport = get_port(input_node, SPA_DIRECTION_INPUT);
	if (inport == NULL)
		goto error_input_port;

	linger = pw_properties_get_bool(properties, PW_KEY_OBJECT_LINGER, false);

	pw_properties_setf(properties, PW_KEY_FACTORY_ID, "%d",
			pw_impl_factory_get_info(d->factory)->id);

	client = resource ? pw_resource_get_client(resource) : NULL;
	if (client && !linger)
		pw_properties_setf(properties, PW_KEY_CLIENT_ID, "%d",
				pw_impl_client_get_info(client)->id);

	if (!d->allow_passive)
		pw_properties_set(properties, PW_KEY_LINK_PASSIVE, NULL);

	link = pw_context_create_link(context, outport, inport, NULL, properties, sizeof(struct link_data));
	properties = NULL;
	if (link == NULL) {
		res = -errno;
		goto error_create_link;
	}

	ld = pw_impl_link_get_user_data(link);
	ld->data = d;
	ld->factory_resource = resource;
	ld->link = link;
	ld->new_id = new_id;
	ld->linger = linger;
	spa_list_append(&d->link_list, &ld->l);

	pw_impl_link_add_listener(link, &ld->link_listener, &link_events, ld);
	if ((res = pw_impl_link_register(link, NULL)) < 0)
		goto error_link_register;

	return link;

error_properties:
	res = -EINVAL;
	pw_resource_errorf_id(resource, new_id, res, NAME": no properties. usage:"FACTORY_USAGE);
	goto error_exit;
error_output_port:
	res = -EINVAL;
	pw_resource_errorf_id(resource, new_id, res, NAME": unknown output port %s", output_port_str);
	goto error_exit;
error_input_port:
	res = -EINVAL;
	pw_resource_errorf_id(resource, new_id, res, NAME": unknown input port %s", input_port_str);
	goto error_exit;
error_create_link:
	pw_resource_errorf_id(resource, new_id, res, NAME": can't link ports %d and %d: %s",
			pw_impl_port_get_info(outport)->id, pw_impl_port_get_info(inport)->id,
			spa_strerror(res));
	goto error_exit;
error_link_register:
	pw_resource_errorf_id(resource, new_id, res, NAME": can't register link: %s", spa_strerror(res));
	goto error_exit;
error_exit:
	pw_properties_free(properties);
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
	struct link_data *ld, *t;

	spa_hook_remove(&d->factory_listener);

	spa_list_for_each_safe(ld, t, &d->link_list, l)
		pw_impl_link_destroy(ld->link);

	d->factory = NULL;
	if (d->module)
		pw_impl_module_destroy(d->module);
	pw_properties_free(d->props);
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
				 "link-factory",
				 PW_TYPE_INTERFACE_Link,
				 PW_VERSION_LINK,
				 pw_properties_new(
					 PW_KEY_FACTORY_USAGE, FACTORY_USAGE,
					 NULL),
				 sizeof(*data));
	if (factory == NULL)
		return -errno;

	data = pw_impl_factory_get_user_data(factory);
	data->factory = factory;
	data->module = module;
	data->context = context;
	data->work = pw_context_get_work_queue(context);
	data->props = args ? pw_properties_new_string(args) : NULL;

	if (data->props) {
		pw_context_conf_update_props(context, "module."NAME".args", data->props);

		data->allow_passive = pw_properties_get_bool(data->props,
				"allow.link.passive", false);
	}

	spa_list_init(&data->link_list);

	pw_log_debug("module %p: new", module);

	pw_impl_factory_set_implementation(factory,
				      &impl_factory,
				      data);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	pw_impl_factory_add_listener(factory, &data->factory_listener, &factory_events, data);
	pw_impl_module_add_listener(module, &data->module_listener, &module_events, data);

	return 0;
}
