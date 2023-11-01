/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Asymptotic Inc. */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <spa/utils/defs.h>
#include <spa/utils/hook.h>
#include <spa/utils/result.h>
#include <spa/debug/dict.h>

#include <pipewire/core.h>
#include <pipewire/impl.h>
#include <pipewire/keys.h>
#include <pipewire/log.h>
#include <pipewire/properties.h>
#include <pipewire/proxy.h>

#define NAME "link-manager"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct node {
	struct spa_list list;

	uint32_t id;
	struct pw_properties *props;

	struct pw_node *proxy;
	struct spa_hook proxy_listener;
};

struct port {
	struct spa_list list;

	uint32_t id;
	uint32_t node_id;
	enum spa_direction direction;
	struct pw_properties *props;

	struct pw_port *proxy;
	struct spa_hook proxy_listener;

	struct port *linked;
};

struct impl {
	struct pw_impl_module *module;
	struct pw_core *core;
	struct pw_registry *registry;

	bool own_core;

	struct spa_hook module_listener;
	struct spa_hook registry_listener;

	struct spa_list nodes;
	struct spa_list ports;
};

void node_free(struct node *node)
{
	if (node->props)
		pw_properties_free(node->props);
	pw_proxy_destroy((struct pw_proxy*)node->proxy);
}

void port_free(struct port *port)
{
	if (port->props)
		pw_properties_free(port->props);
	pw_proxy_destroy((struct pw_proxy*)port->proxy);
}

void create_link(struct impl *impl, struct port *p1, struct port *p2)
{
	struct pw_properties *link_props;
	struct pw_proxy *proxy;

	if (p1->direction == SPA_DIRECTION_OUTPUT) {
		struct port *tmp = p1;
		p1 = p2;
		p2 = tmp;
	}

	// p1 is now input, p2 is output
	link_props = pw_properties_new(NULL, NULL);
	pw_properties_set(link_props, PW_KEY_OBJECT_LINGER, "true");
	pw_properties_setf(link_props, PW_KEY_LINK_INPUT_PORT, "%u", p1->id);
	pw_properties_setf(link_props, PW_KEY_LINK_OUTPUT_PORT, "%u", p2->id);

	proxy = pw_core_create_object(impl->core,
			"link-factory",
			PW_TYPE_INTERFACE_Link,
			PW_VERSION_LINK,
			&link_props->dict, 0);

	if (proxy == NULL)
		pw_log_error("Could not create link: %s", spa_strerror(errno));
	else
		pw_proxy_destroy(proxy);

	p1->linked = p2;
	p2->linked = p1;
}

struct node *find_node_for_port(struct impl *impl, struct port *port)
{
	struct node *node;

	spa_list_for_each(node, &impl->nodes, list) {
		if (node->id == port->node_id)
			return node;
	}

	return NULL;
}

struct node *find_target_node(struct impl *impl, struct node *node)
{
	struct node *n;
	const char *target;
	uint32_t target_id = 0;
	bool target_is_id;

	if (!node->props) {
		pw_log_debug("Don't yet have node props for %u", node->id);
		return NULL;
	}

	if ((target = pw_properties_get(node->props, PW_KEY_TARGET_OBJECT)) == NULL)
		return NULL;
	else {
		pw_log_debug("node %u has target %s", node->id, target);
	}

	target_is_id = spa_atou32(target, &target_id, 10);

	// Find the node given by `target` in `impl->nodes`
	spa_list_for_each(n, &impl->nodes, list) {
		if (!target_is_id && !n->props) {
			pw_log_debug("Can't yet match node %u", n->id);
			continue;
		}

		if (target_is_id && n->id == target_id)
			return n;
		else if (spa_streq(target, pw_properties_get(n->props, PW_KEY_NODE_NAME)))
			return n;
	}

	return NULL;
}

void check_port(struct impl *impl, struct port *port)
{
	struct node *node, *target_node = NULL;
	struct port *p;

	if (port->linked)
		return;

	node = find_node_for_port(impl, port);
	if (!node) {
		pw_log_error("Could not find node for port %u", port->id);
		return;
	}

	target_node = find_target_node(impl, node);
	if (!target_node)
		return;

	pw_log_info("Trying to link port of %s and %s/%u",
			pw_properties_get(node->props, PW_KEY_NODE_NAME),
			pw_properties_get(target_node->props, PW_KEY_NODE_NAME), target_node->id);

	// FIXME: do some validation here
	spa_list_for_each(p, &impl->ports, list) {
		// Link all ports of `node` with `target_node`, but port.id
		if (p->node_id != target_node->id)
			continue;

		if (p->linked)
			continue;

		if (!p->props) {
			pw_log_debug("Can't yet match port %u", p->id);
			continue;
		}

		pw_log_debug("%s/%s & %s/%s",
				pw_properties_get(p->props, PW_KEY_PORT_NAME), p->direction == SPA_DIRECTION_INPUT ? "in" : "out",
				pw_properties_get(port->props, PW_KEY_PORT_NAME), port->direction == SPA_DIRECTION_INPUT ? "in" : "out");

		if (spa_streq(pw_properties_get(p->props, PW_KEY_PORT_ID),
					pw_properties_get(port->props, PW_KEY_PORT_ID)) &&
				p->direction != port->direction) {
			create_link(impl, p, port);
		}
	}
}

void node_info(void *data, const struct pw_node_info *info)
{
	struct node *node = data;

	pw_log_debug("Got node info for %u", node->id);
	node->props = pw_properties_new_dict(info->props);
	// Just gather props, we'll use this while linking
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_info,
};

void port_info(void *data, const struct pw_port_info *info)
{
	struct impl *impl = data;
	struct port *port = NULL;

	// We don't expect this to fail
	spa_list_for_each(port, &impl->ports, list) {
		if (port->id == info->id)
			break;
	}

	pw_log_debug("Got port info for %u", port->id);
	port->direction = info->direction;
	port->props = pw_properties_new_dict(info->props);

	check_port(impl, port);
}

static const struct pw_port_events port_events = {
	PW_VERSION_NODE_EVENTS,
	.info = port_info,
};

void registry_global_added(void *data, uint32_t id, uint32_t permissions,
		const char *type, uint32_t version,
		const struct spa_dict *props)
{
	struct impl *impl = data;
	const char *str;

	pw_log_debug("Got object %u of type %s", id, type);

	if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
		struct pw_node *proxy;
		struct node *node;

		proxy = pw_registry_bind(impl->registry, id, type, PW_VERSION_NODE, sizeof(struct node));

		node = pw_proxy_get_user_data((struct pw_proxy*)proxy);
		node->id = id;
		node->proxy = proxy;
		node->props = NULL;

		spa_list_insert(&impl->nodes, &node->list);

		pw_node_add_listener(proxy, &node->proxy_listener, &node_events, node);
		pw_core_sync(impl->core, 0, 0);
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Port)) {
		struct pw_port *proxy;
		struct port *port;

		if ((str = spa_dict_lookup(props, PW_KEY_NODE_ID)) == NULL) {
			pw_log_info("Got port %u with no node id", id);
			return;
		}

		proxy = pw_registry_bind(impl->registry, id, type, PW_VERSION_PORT, sizeof(struct port));

		port = pw_proxy_get_user_data((struct pw_proxy*)proxy);
		port->id = id;
		port->node_id = atoi(str);
		port->props = NULL;
		port->proxy = proxy;

		spa_list_insert(&impl->ports, &port->list);

		pw_port_add_listener(proxy, &port->proxy_listener, &port_events, impl);
	} else {
		pw_log_debug("Ignoring object of type %s", type);
	}
}

void registry_global_removed(void *data, uint32_t id)
{
	struct impl *impl = data;
	struct node *node;
	struct port *port;

	spa_list_for_each(node, &impl->nodes, list) {
		if (node->id != id)
			continue;

		pw_log_debug("Removing node %u", id);

		spa_list_remove(&node->list);
		node_free(node);
		return;
	}

	spa_list_for_each(port, &impl->ports, list) {
		if (port->id != id)
			continue;

		pw_log_debug("Removing port %u", id);

		spa_list_remove(&port->list);
		port_free(port);
		return;
	}
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_global_added,
	.global_remove = registry_global_removed,
};

void module_destroy(void *data)
{
	struct impl *impl = data;
	struct node *node;
	struct port *port;

	spa_hook_remove(&impl->module_listener);
	spa_hook_remove(&impl->registry_listener);

	spa_list_for_each(node, &impl->nodes, list) {
		node_free(node);
	}

	spa_list_for_each(port, &impl->ports, list) {
		port_free(port);
	}

	pw_proxy_destroy((struct pw_proxy*)impl->registry);

	if (impl->own_core)
		pw_core_disconnect(impl->core);

	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct impl *impl;
	struct pw_context *context = pw_impl_module_get_context(module);

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;
	impl->module = module;
	impl->core = pw_context_get_object(context, PW_TYPE_INTERFACE_Core);

	if (impl->core == NULL) {
		// FIXME: allow non-default remotes
		impl->core = pw_context_connect(context, NULL, 0);
		impl->own_core = true;
	}

	impl->registry = pw_core_get_registry(impl->core, PW_VERSION_REGISTRY, 0);

	spa_list_init(&impl->nodes);
	spa_list_init(&impl->ports);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);
	pw_registry_add_listener(impl->registry, &impl->registry_listener, &registry_events, impl);

	return 0;
}
