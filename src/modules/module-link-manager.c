/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Asymptotic Inc. */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <spa/debug/dict.h>
#include <spa/param/props.h>
#include <spa/pod/parser.h>
#include <spa/utils/defs.h>
#include <spa/utils/hook.h>
#include <spa/utils/result.h>

#include <pipewire/core.h>
#include <pipewire/impl.h>
#include <pipewire/keys.h>
#include <pipewire/log.h>
#include <pipewire/properties.h>
#include <pipewire/proxy.h>

#define NAME "link-manager"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define KEY_API_ALSA_USB_GADGET "api.alsa.usb-gadget"

struct node {
	struct spa_list list;
	struct impl *impl;

	uint32_t id;
	struct pw_properties *props;

	struct pw_node *proxy;
	struct spa_hook proxy_listener;

	bool is_gadget;
	// -1 for non-USB nodes, 0 or rate for USB nodes to track link status
	int capture_rate;
};

struct port {
	struct spa_list list;

	uint32_t id;
	// FIXME: maybe just replace with a `struct node *`
	uint32_t node_id;
	enum spa_direction direction;
	struct pw_properties *props;

	struct pw_port *proxy;
	struct spa_hook proxy_listener;

	struct port *linked;
	uint32_t link_id;
};

struct impl {
	struct pw_impl_module *module;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;

	bool own_core;

	struct spa_hook module_listener;
	struct spa_hook registry_listener;

	struct spa_list nodes;
	struct spa_list ports;
};

static void node_free(struct node *node)
{
	if (node->props)
		pw_properties_free(node->props);
	pw_proxy_destroy((struct pw_proxy*)node->proxy);
}

static void port_free(struct port *port)
{
	if (port->props)
		pw_properties_free(port->props);
	pw_proxy_destroy((struct pw_proxy*)port->proxy);
}

static void create_link(struct impl *impl, struct port *p1, struct port *p2)
{
	struct pw_properties *link_props;
	struct pw_proxy *proxy;

	if (p1->direction == SPA_DIRECTION_OUTPUT) {
		struct port *tmp = p1;
		p1 = p2;
		p2 = tmp;
	}

	pw_log_debug("link %s -> %s",
			pw_properties_get(p1->props, PW_KEY_PORT_NAME),
			pw_properties_get(p2->props, PW_KEY_PORT_NAME));

	// p1 is now input, p2 is output
	link_props = pw_properties_new(NULL, NULL);
	pw_properties_setf(link_props, PW_KEY_LINK_INPUT_PORT, "%u", p1->id);
	pw_properties_setf(link_props, PW_KEY_LINK_OUTPUT_PORT, "%u", p2->id);

	proxy = pw_core_create_object(impl->core,
			"link-factory",
			PW_TYPE_INTERFACE_Link,
			PW_VERSION_LINK,
			&link_props->dict, 0);

	if (proxy == NULL)
		pw_log_error("Could not create link: %s", spa_strerror(errno));

	p1->linked = p2;
	p2->linked = p1;
}

static bool node_is_source(struct node *node)
{
	return spa_streq(pw_properties_get(node->props, PW_KEY_MEDIA_CLASS), "Audio/Source");
}

static bool node_has_link_group(struct node *node)
{
	return pw_properties_get(node->props, PW_KEY_NODE_LINK_GROUP) != NULL;
}

static bool node_is_ready(struct node *node)
{
	// We want to know if we have all the node information (i.e. has the
	// node_info callback been called). We use the existence of properties
	// as a proxy for this, but could potentially just use an explicit
	// boolean in the future
	if (node->props == NULL)
		return false;

	// USB device, isn't deemed ready until the capture side is linked
	if (node->is_gadget && node->capture_rate <= 0)
		return false;

	return true;
}

static struct node *find_node_by_id(struct impl *impl, uint32_t id)
{
	struct node *node;

	spa_list_for_each(node, &impl->nodes, list) {
		if (node->id == id)
			return node;
	}

	return NULL;
}

static struct node *find_target_node(struct impl *impl, struct node *node)
{
	struct node *n;
	const char *target;
	uint32_t target_id = 0;
	bool target_is_id;

	if (!node->props) {
		return NULL;
	}

	if ((target = pw_properties_get(node->props, PW_KEY_TARGET_OBJECT)) == NULL)
		return NULL;

	target_is_id = spa_atou32(target, &target_id, 10);

	// Find the node given by `target` in `impl->nodes`
	spa_list_for_each(n, &impl->nodes, list) {
		if (!target_is_id && !n->props)
			continue;

		if (target_is_id && n->id == target_id)
			return n;
		else if (spa_streq(target, pw_properties_get(n->props, PW_KEY_NODE_NAME)))
			return n;
	}

	return NULL;
}

static struct port *find_port_by_id(struct impl *impl, uint32_t id)
{
	struct port *port;

	spa_list_for_each(port, &impl->ports, list) {
		if (port->id == id)
			return port;
	}

	return NULL;
}

static void link_port_to_target(struct impl *impl, struct port *port, struct node *target_node) {
	struct port *p;

	if (!port->props) {
		// Not ready to compare yet
		return;
	}

	// FIXME: do some validation here
	spa_list_for_each(p, &impl->ports, list) {
		if (p->node_id != target_node->id)
			continue;

		if (p->linked)
			continue;

		if (!p->props) {
			continue;
		}

		if (spa_streq(pw_properties_get(p->props, PW_KEY_PORT_ID),
					pw_properties_get(port->props, PW_KEY_PORT_ID)) &&
				p->direction != port->direction) {
			create_link(impl, p, port);
		}
	}
}

static void check_port(struct impl *impl, struct port *port)
{
	struct node *node, *target_node = NULL;

	if (port->linked)
		return;

	node = find_node_by_id(impl, port->node_id);
	if (!node) {
		pw_log_error("Could not find node for port %u", port->id);
		return;
	}

	if (!node_is_ready(node)) {
		pw_log_debug("Waiting for node to be ready for port %u", port->id);
		return;
	}

	target_node = find_target_node(impl, node);
	if (!target_node)
		return;

	if (!node_is_ready(target_node)) {
		pw_log_debug("Waiting for target node to be ready for port %u", port->id);
		return;
	}

	link_port_to_target(impl, port, target_node);
}

static void link_node_group(struct impl *impl, struct node *node);

static void link_node_ports(struct impl *impl, struct node *this_node, bool cascade)
{
	struct node *node, *other_node = NULL;
	struct port *port;

	// We can get here, for example, when traversing a link group where one
	// node props have been read but another have not
	if (!node_is_ready(this_node))
		return;

	// First see if this node has a target
	other_node = find_target_node(impl, this_node);

	if (other_node) {
		pw_log_debug("Node %u has a target", this_node->id);
	} else {
		// If not, see if this is the target of another node
		spa_list_for_each(node, &impl->nodes, list) {
			struct node *target;

			target = find_target_node(impl, node);
			if (!target)
				continue;

			// This node's target is the USB device
			if (target->id == this_node->id) {
				other_node = node;
				break;
			}
		}

		pw_log_debug("Node %u is%s a target", this_node->id, other_node ? "" : " not");
	}

	if (!other_node)
		return;

	if (!node_is_ready(other_node)) {
		pw_log_debug("... but is not ready");
		return;
	}

	spa_list_for_each(port, &impl->ports, list) {
		if (port->node_id != this_node->id)
			continue;

		link_port_to_target(impl, port, other_node);
	}

	if (cascade) {
		// Cascade to link groups
		if (node_has_link_group(this_node))
			link_node_group(impl, this_node);
		if (node_has_link_group(other_node))
			link_node_group(impl, other_node);
	}
}

static void link_node_group(struct impl *impl, struct node *node)
{
	struct node *n;
	const char *group = pw_properties_get(node->props, PW_KEY_NODE_LINK_GROUP);

	if (!group) {
		pw_log_warn("Could not find link group for node '%u'", node->id);
		return;
	}

	pw_log_debug("Linking nodes in group '%s'", group);

	spa_list_for_each(n, &impl->nodes, list) {
		if (!node_is_ready(n))
			continue;

		if (!spa_streq(group, pw_properties_get(n->props, PW_KEY_NODE_LINK_GROUP)))
			continue;

		// Don't cascade to avoid infinite recursion
		link_node_ports(impl, n, false);
	}
}

static void unlink_node_group(struct impl *impl, struct node *node);

static void unlink_node_ports(struct impl *impl, struct node *this_node, bool cascade)
{
	struct node *other_node = NULL;
	struct port *port;
	uint32_t other_node_id = 0;

	if (!this_node)
		return;

	spa_list_for_each(port, &impl->ports, list) {
		if (port->node_id != this_node->id)
			continue;

		if (!port->linked)
			continue;

		pw_log_debug("Destroying link %u (%u <-> %u)", port->link_id, port->id, port->linked->id);
		pw_registry_destroy(impl->registry, port->link_id);

		other_node_id = port->linked->node_id;

		// Clean up the other port
		port->linked->link_id = 0;
		port->linked->linked = NULL;
		// And this one
		port->link_id = 0;
		port->linked = NULL;
	}

	if (other_node_id != 0)
		other_node = find_node_by_id(impl, other_node_id);

	if (cascade) {
		// Cascade to link groups
		if (node_has_link_group(this_node))
			unlink_node_group(impl, this_node);
		if (other_node && node_has_link_group(other_node))
			unlink_node_group(impl, other_node);
	}
}

static void unlink_node_group(struct impl *impl, struct node *node)
{
	struct node *n;
	const char *group = pw_properties_get(node->props, PW_KEY_NODE_LINK_GROUP);

	if (!group) {
		pw_log_warn("Could not find link group for node '%u'", node->id);
		return;
	}


	pw_log_debug("Unlinking nodes in group '%s'", group);

	spa_list_for_each(n, &impl->nodes, list) {
		const char *g = pw_properties_get(n->props, PW_KEY_NODE_LINK_GROUP);

		if (!spa_streq(group, g))
			continue;

		// Don't cascade to avoid infinite recursion
		pw_log_debug("Destroying links of node %u (%s)", n->id, g);
		unlink_node_ports(impl, n, false);
	}
}

static void usb_capture_rate_changed(struct node *node, int capture_rate)
{
	struct impl *impl = node->impl;
	const char *device = pw_properties_get(node->props, "api.alsa.path");

	node->capture_rate = capture_rate;

	if (device) {
		// Update rate for all nodes of the same device track the USB status
		struct node *n;

		spa_list_for_each(n, &impl->nodes, list) {
			const char *d = pw_properties_get(node->props, "api.alsa.path");

			if (!spa_streq(device, d))
				continue;

			n->capture_rate = capture_rate;
		}
	}

	if (capture_rate > 0) {
		pw_log_debug("Linking USB ports");
		// ... and cascading to linked groups
		link_node_ports(impl, node, true);
	} else {
		pw_log_debug("Unlinking USB ports");
		// ... and cascading to linked groups
		unlink_node_ports(impl, node, true);
	}
}

static void node_info(void *data, const struct pw_node_info *info)
{
	struct node *node = data;

	if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) {
		bool wasnt_ready = false;

		pw_log_debug("node props updated: %u", node->id);

		if (node->props)
			pw_properties_free(node->props);
		else
			wasnt_ready = true;

		node->props = pw_properties_new_dict(info->props);
		node->is_gadget = spa_atob(spa_dict_lookup(info->props, KEY_API_ALSA_USB_GADGET));

		// We now know whether this is a gadget or not, so we can check whether it can be linked
		if (wasnt_ready) {
			// Listen for rate changes on USB gadget capture nodes
			if (node->is_gadget && node_is_source(node)) {
				uint32_t ids[] = { SPA_PARAM_Props };
				pw_node_subscribe_params(node->proxy, ids, SPA_N_ELEMENTS(ids));
			}

			link_node_ports(node->impl, node, true);
		}
	}
	// Just gather props, we'll use this while linking
}

static void node_param(void *data, int seq, uint32_t id, uint32_t index,
		uint32_t next, const struct spa_pod *param)
{
	struct node *node = data;
	struct spa_pod_parser param_props;
	struct spa_pod_frame f;
	struct spa_pod *pod = NULL;
	int32_t capture_rate = 0;

	if (id != SPA_PARAM_Props)
		return;

	spa_pod_parse_object(param,
			SPA_TYPE_OBJECT_Props, NULL,
			SPA_PROP_params, SPA_POD_OPT_Pod(&pod));

	if (!pod)
		return;

	pw_log_debug("node params updated");

	spa_pod_parser_pod(&param_props, pod);
	if (spa_pod_parser_push_struct(&param_props, &f) < 0) {
		pw_log_warn("got props param that is not a struct");
		return;
	}

	while (true) {
		const char *name;

		// prop name
		if (spa_pod_parser_get_string(&param_props, &name) < 0)
			break;

		// prop value
		if (spa_pod_parser_get_pod(&param_props, &pod) < 0)
			break;

		if (!spa_streq(name, "api.alsa.bind-ctl.Capture Rate"))
			continue;

		if (!spa_pod_is_int(pod)) {
			pw_log_warn("Did not find expected integer rate in props param");
			break;
		}

		capture_rate = SPA_POD_VALUE(struct spa_pod_int, pod);

		if (capture_rate != node->capture_rate)
			usb_capture_rate_changed(node, capture_rate);
	}
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_info,
	.param = node_param,
};

static void port_info(void *data, const struct pw_port_info *info)
{
	struct impl *impl = data;
	struct port *port = NULL;

	// We don't expect this to fail
	port = find_port_by_id(impl, info->id);

	port->direction = info->direction;
	if (info->change_mask & PW_PORT_CHANGE_MASK_PROPS) {
		if (port->props)
			pw_properties_free(port->props);
		port->props = pw_properties_new_dict(info->props);
	}

	check_port(impl, port);
}

static const struct pw_port_events port_events = {
	PW_VERSION_NODE_EVENTS,
	.info = port_info,
};

static void registry_global(void *data, uint32_t id, uint32_t permissions,
		const char *type, uint32_t version,
		const struct spa_dict *props)
{
	struct impl *impl = data;
	const char *str;

	pw_log_trace("Got type %s: %u", type, id);

	if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
		struct pw_node *proxy;
		struct node *node;

		proxy = pw_registry_bind(impl->registry, id, type, PW_VERSION_NODE, sizeof(struct node));

		node = pw_proxy_get_user_data((struct pw_proxy*)proxy);
		node->impl = impl;
		node->id = id;
		node->proxy = proxy;
		node->props = NULL;
		node->capture_rate = -1;

		spa_list_insert(&impl->nodes, &node->list);

		pw_node_add_listener(proxy, &node->proxy_listener, &node_events, node);
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
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Link)) {
		struct port *port;
		const char *id_str;
		uint32_t port_id;

		id_str = spa_dict_lookup(props, PW_KEY_LINK_INPUT_PORT);
		if (spa_atou32(id_str, &port_id, 10) && (port = find_port_by_id(impl, port_id)))
			port->link_id = id;

		id_str = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_PORT);
		if (spa_atou32(id_str, &port_id, 10) && (port = find_port_by_id(impl, port_id)))
			port->link_id = id;

		pw_log_debug("Stored link %u (%s -> %s)", id,
				spa_dict_lookup(props, PW_KEY_LINK_INPUT_PORT),
				spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_PORT));
	}
}

static void registry_global_removed(void *data, uint32_t id)
{
	struct impl *impl = data;
	struct node *node;
	struct port *port;

	pw_log_trace("Removed %u", id);

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
	.global = registry_global,
	.global_remove = registry_global_removed,
};

static void module_destroy(void *data)
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

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;
	impl->module = module;
	impl->context = pw_impl_module_get_context(module);
	impl->core = pw_context_get_object(impl->context, PW_TYPE_INTERFACE_Core);

	if (impl->core == NULL) {
		// FIXME: allow non-default remotes
		impl->core = pw_context_connect(impl->context, NULL, 0);
		impl->own_core = true;
	}

	impl->registry = pw_core_get_registry(impl->core, PW_VERSION_REGISTRY, 0);

	spa_list_init(&impl->nodes);
	spa_list_init(&impl->ports);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);
	pw_registry_add_listener(impl->registry, &impl->registry_listener, &registry_events, impl);

	pw_core_sync(impl->core, 0, 0);

	return 0;
}
