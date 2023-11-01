/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Asymptotic Inc. */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <alsa/asoundlib.h>
#include <alsa/version.h>

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

#define USB_CAPTURE_DEV "usb-out-ch"
#define USB_PLAYBACK_DEV "usb-in-ch"

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

	/* For the ALSA watcher */
	snd_ctl_t *ctl;
	snd_ctl_elem_value_t *capture_rate_elem;
	int capture_rate;

	struct spa_source sources[16];
	struct pollfd pfds[16];
	int n_fds;
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

static struct node *find_node_by_id(struct impl *impl, uint32_t id)
{
	struct node *node;

	spa_list_for_each(node, &impl->nodes, list) {
		if (node->id == id)
			return node;
	}

	return NULL;
}

static struct node *find_node_by_name(struct impl *impl, const char *name)
{
	struct node *n, *node = NULL;

	spa_list_for_each(n, &impl->nodes, list) {
		if (spa_streq(pw_properties_get(n->props, PW_KEY_NODE_NAME), name)) {
			node = n;
			break;
		}
	}

	return node;
}

static struct node *find_target_node(struct impl *impl, struct node *node)
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

	// FIXME: do some validation here
	spa_list_for_each(p, &impl->ports, list) {
		if (p->node_id != target_node->id)
			continue;

		if (p->linked)
			continue;

		if (!p->props) {
			pw_log_debug("Can't yet match port %u", p->id);
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

	target_node = find_target_node(impl, node);
	if (!target_node)
		return;

	if (spa_streq(pw_properties_get(target_node->props, PW_KEY_NODE_NAME), USB_CAPTURE_DEV) ||
			spa_streq(pw_properties_get(target_node->props, PW_KEY_NODE_NAME), USB_PLAYBACK_DEV)) {
		if (impl->capture_rate == 0) {
			pw_log_debug("Skipping USB device until host starts playback");
			return;
		}
	}

	link_port_to_target(impl, port, target_node);
}

static void node_info(void *data, const struct pw_node_info *info)
{
	struct node *node = data;

	if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS)
		node->props = pw_properties_new_dict(info->props);
	// Just gather props, we'll use this while linking
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_info,
};

static void port_info(void *data, const struct pw_port_info *info)
{
	struct impl *impl = data;
	struct port *port = NULL;

	// We don't expect this to fail
	port = find_port_by_id(impl, info->id);

	port->direction = info->direction;
	if (info->change_mask & PW_PORT_CHANGE_MASK_PROPS)
		port->props = pw_properties_new_dict(info->props);

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
		node->id = id;
		node->proxy = proxy;
		node->props = NULL;

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

static void link_node_ports(struct impl *impl, const char *node_name)
{
	struct node *node, *this_node = NULL, *other_node = NULL;
	struct port *port;

	this_node = find_node_by_name(impl, node_name);

	if (!this_node)
		return;

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

	if (!other_node)
		return;

	spa_list_for_each(port, &impl->ports, list) {
		if (port->node_id != this_node->id)
			continue;

		link_port_to_target(impl, port, other_node);
	}
}

static void unlink_node_ports(struct impl *impl, const char *node_name)
{
	struct node *this_node;
	struct port *port;

	this_node = find_node_by_name(impl, node_name);

	if (!this_node)
		return;

	spa_list_for_each(port, &impl->ports, list) {
		if (port->node_id != this_node->id)
			continue;

		if (!port->linked)
			continue;

		pw_log_debug("Destroying link %u (%u <-> %u)", port->link_id, port->id, port->linked->id);
		pw_registry_destroy(impl->registry, port->link_id);

		// Clean up the other port
		port->linked->link_id = 0;
		port->linked->linked = NULL;
		// And this one
		port->link_id = 0;
		port->linked = NULL;
	}
}

static void usb_capture_rate_changed(struct impl *impl, int capture_rate)
{
	impl->capture_rate = capture_rate;

	if (capture_rate > 0) {
		pw_log_debug("Linking USB ports");
		link_node_ports(impl, USB_PLAYBACK_DEV);
		link_node_ports(impl, USB_CAPTURE_DEV);
	} else {
		pw_log_debug("Unlinking USB ports");
		unlink_node_ports(impl, USB_CAPTURE_DEV);
		unlink_node_ports(impl, USB_PLAYBACK_DEV);
	}
}

static void ctl_event(struct spa_source *source)
{
	struct impl *impl = source->data;
	snd_ctl_event_t *event;
	int capture_rate, err;

	if (!(source->rmask & SPA_IO_IN)) {
		pw_log_debug("Woken up without work");
		return;
	}

	snd_ctl_event_alloca(&event);
	err = snd_ctl_read(impl->ctl, event);
	if (err < 0) {
		pw_log_warn("Error reading ctl event: %s", snd_strerror(err));
		return;
	}

	if (snd_ctl_event_get_type(event) != SND_CTL_EVENT_ELEM ||
			snd_ctl_event_elem_get_numid(event) != snd_ctl_elem_value_get_numid(impl->capture_rate_elem))
		return;

	err = snd_ctl_elem_read(impl->ctl, impl->capture_rate_elem);
	if (err < 0) {
		pw_log_warn("Could not read 'Capture Rate': %s", snd_strerror(err));
		return;
	}

	capture_rate = snd_ctl_elem_value_get_integer(impl->capture_rate_elem, 0);
	pw_log_debug("New capture rate: %d", capture_rate);

	if (capture_rate != impl->capture_rate) {
		// TODO: debounce
		usb_capture_rate_changed(impl, capture_rate);
	}
}

static void start_alsa_watcher(struct impl *impl, const char *device_name)
{
	struct pw_loop *loop;
	snd_ctl_elem_id_t *id;
	int err;

	err = snd_ctl_open(&impl->ctl, device_name, SND_CTL_READONLY | SND_CTL_NONBLOCK);
	if (err < 0) {
		pw_log_warn("Could not find ctl device for %s: %s", device_name, snd_strerror(err));
		impl->ctl = NULL;
		return;
	}

	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_id_set_name(id, "Capture Rate");
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_PCM);

	snd_ctl_elem_value_malloc(&impl->capture_rate_elem);
	snd_ctl_elem_value_set_id(impl->capture_rate_elem, id);

	err = snd_ctl_elem_read(impl->ctl, impl->capture_rate_elem);
	if (err < 0) {
		pw_log_warn("Could not read 'Capture Rate': %s", snd_strerror(err));

		snd_ctl_elem_value_free(impl->capture_rate_elem);
		impl->capture_rate_elem = NULL;

		snd_ctl_close(impl->ctl);
		impl->ctl = NULL;
		return;
	}

	impl->capture_rate = snd_ctl_elem_value_get_integer(impl->capture_rate_elem, 0);

	impl->n_fds = snd_ctl_poll_descriptors_count(impl->ctl);
	if (impl->n_fds > (int)SPA_N_ELEMENTS(impl->sources)) {
		pw_log_warn("Too many poll descriptors (%d), listening to a subset", impl->n_fds);
		impl->n_fds = SPA_N_ELEMENTS(impl->sources);
	}

	if ((err = snd_ctl_poll_descriptors(impl->ctl, impl->pfds, impl->n_fds)) < 0) {
		pw_log_warn("Could not get poll descriptors: %s", snd_strerror(err));
		return;
	}

	snd_ctl_subscribe_events(impl->ctl, 1);

	loop = pw_context_get_main_loop(impl->context);

	for (int i = 0; i < impl->n_fds; i++) {
		impl->sources[i].func = ctl_event;
		impl->sources[i].data = impl;
		impl->sources[i].fd = impl->pfds[i].fd;
		impl->sources[i].mask = SPA_IO_IN;
		impl->sources[i].rmask = 0;
		pw_loop_add_source(loop, &impl->sources[i]);
	}
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	struct node *node;
	struct port *port;
	struct pw_loop *loop;

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

	loop = pw_context_get_main_loop(impl->context);

	for (int i = 0; i < impl->n_fds; i++)
		pw_loop_remove_source(loop, &impl->sources[i]);

	if (impl->capture_rate_elem)
		snd_ctl_elem_value_free(impl->capture_rate_elem);

	if (impl->ctl)
		snd_ctl_close(impl->ctl);

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

	start_alsa_watcher(impl, "hw:1");

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);
	pw_registry_add_listener(impl->registry, &impl->registry_listener, &registry_events, impl);

	return 0;
}
