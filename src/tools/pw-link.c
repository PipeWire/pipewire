/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <signal.h>
#include <math.h>
#include <getopt.h>
#include <regex.h>
#include <locale.h>

#include <spa/utils/cleanup.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/defs.h>
#include <spa/debug/file.h>
#include <spa/param/latency-utils.h>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

enum object_type {
	OBJECT_ANY,
	OBJECT_NODE,
	OBJECT_PORT,
	OBJECT_LINK,
};

union object_data {
	struct {
		enum pw_direction direction;
		uint32_t node;
	} port;
	struct {
		uint32_t output_port;
		uint32_t input_port;
	} link;
};

struct object {
	struct spa_list link;
	struct data *d;

	uint32_t id;
	enum object_type type;
	struct pw_properties *props;
	union object_data data;
#define STATE_NONE	0
#define STATE_NEW	1
#define STATE_CHANGED	2
#define STATE_DELETE	3
	int state;

	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	struct spa_hook object_listener;

	struct spa_latency_info latency[2];
	bool latency_changed[2];
};

struct target_link {
	struct spa_list link;
	struct data *data;
	struct pw_proxy *proxy;
	struct spa_hook listener, link_listener;
	enum pw_link_state state;
	int result;
};

enum mode {
	MODE_CONNECT,
	MODE_DISCONNECT,
	MODE_LIST,
};

enum list_target {
	LIST_OUTPUT = 1 << 0,
	LIST_INPUT = 1 << 1,
	LIST_PORTS = LIST_OUTPUT | LIST_INPUT,
	LIST_LINKS = 1 << 2,
	LIST_LATENCY = 1 << 3,
};

struct data {
	struct pw_main_loop *loop;

	const char *opt_remote;
	enum mode opt_mode;
	enum list_target opt_list; /* for `MODE_LIST` */
	bool opt_id;
	bool opt_verbose;
	bool opt_wait;
	bool opt_monitor;
	const char *opt_output;
	const char *opt_input;
	struct pw_properties *props;

	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct spa_list objects;
	struct spa_list target_links;

	int sync;
	int nb_links;
	bool new_object;
	bool monitoring;
	bool list_inputs;
	bool list_outputs;

	regex_t out_port_regex, *out_regex;
	regex_t in_port_regex, *in_regex;
};

static void destroy_object(struct object *obj)
{
	spa_list_remove(&obj->link);
	pw_properties_free(obj->props);
	if (obj->proxy)
		pw_proxy_destroy(obj->proxy);
	free(obj);
}

static void link_event(struct target_link *tl, enum pw_link_state state, int result)
{
	/* Ignore non definitive states (negotiating, allocating, etc). */
	if (state != PW_LINK_STATE_ERROR &&
	    state != PW_LINK_STATE_PAUSED &&
	    state != PW_LINK_STATE_ACTIVE)
		return;

	/* Keep the first definitive state. For example, once a link is marked
	 * as paused, we start ignoring all errors. */
	if (tl->state == PW_LINK_STATE_INIT) {
		tl->state = state;
		tl->result = result;
	}

	/* End if all links have a definitive state. */
	struct target_link *m;
	spa_list_for_each(m, &tl->data->target_links, link) {
		if (m->state == PW_LINK_STATE_INIT)
			return;
	}
	pw_main_loop_quit(tl->data->loop);
}

static void link_proxy_destroy(void *data)
{
	struct target_link *tl = data;

	spa_hook_remove(&tl->listener);
	spa_hook_remove(&tl->link_listener);
	tl->proxy = NULL;

	link_event(tl, PW_LINK_STATE_ERROR, -EINVAL);
}

static void link_proxy_removed(void *data)
{
	struct target_link *tl = data;
	pw_proxy_destroy(tl->proxy);
}

static void link_proxy_error(void *data, int seq, int res, const char *message)
{
	struct target_link *tl = data;
	link_event(tl, PW_LINK_STATE_ERROR, res);
}

static const struct pw_proxy_events link_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = link_proxy_destroy,
	.removed = link_proxy_removed,
	.error = link_proxy_error,
};

static void link_event_info(void *data, const struct pw_link_info *info)
{
	struct target_link *tl = data;
	int result = 0;

	/*
	 * Invent an error code to always have one if state == error. That does
	 * not occur currently; on link error a proxy error is raised first.
	 */
	if (tl->state == PW_LINK_STATE_ERROR)
		result = -EINVAL;

	link_event(tl, info->state, result);
}

static const struct pw_link_events link_events = {
	PW_VERSION_LINK_EVENTS,
	.info = link_event_info,
};

static void core_sync(struct data *data)
{
	data->sync = pw_core_sync(data->core, PW_ID_CORE, data->sync);
}

static const char *state_name(struct data *d, struct object *o)
{
	if (!d->opt_monitor)
		return "";
	switch (o->state) {
	case STATE_NONE:
		return " ";
	case STATE_NEW:
		return "+";
	case STATE_CHANGED:
		return "*";
	case STATE_DELETE:
		return "-";
	}
	return " ";
}

static struct object *find_object(struct data *data, enum object_type type, uint32_t id)
{
	struct object *o;
	spa_list_for_each(o, &data->objects, link)
		if ((type == OBJECT_ANY || o->type == type) && o->id == id)
			return o;
	return NULL;
}

static struct object *find_node_port(struct data *data, struct object *node, enum pw_direction direction, const char *port_id)
{
	struct object *o;

	spa_list_for_each(o, &data->objects, link) {
		const char *o_port_id;
		if (o->type != OBJECT_PORT)
			continue;
		if (o->data.port.node != node->id)
			continue;
		if (o->data.port.direction != direction)
			continue;
		if ((o_port_id = pw_properties_get(o->props, PW_KEY_PORT_ID)) == NULL)
			continue;
		if (spa_streq(o_port_id, port_id))
			return o;
	}

	return NULL;
}

static char *node_name(char *buffer, int size, struct object *n)
{
	const char *name;
	buffer[0] = '\0';
	if ((name = pw_properties_get(n->props, PW_KEY_NODE_NAME)) == NULL)
		snprintf(buffer, size, "node.id.%d", n->id);
	else
		snprintf(buffer, size, "%s", name);
	return buffer;
}

static char *node_path(char *buffer, int size, struct object *n)
{
	const char *name;
	buffer[0] = '\0';
	if ((name = pw_properties_get(n->props, PW_KEY_OBJECT_PATH)) == NULL)
		snprintf(buffer, size, "node.path.%d", n->id);
	else
		snprintf(buffer, size, "%s", name);
	return buffer;
}

static char *port_name(char *buffer, int size, struct object *n, struct object *p)
{
	const char *name1, *name2;
	buffer[0] = '\0';
	name1 = pw_properties_get(n->props, PW_KEY_NODE_NAME);
	name2 = pw_properties_get(p->props, PW_KEY_PORT_NAME);
	if (name1 && name2)
		snprintf(buffer, size, "%s:%s", name1, name2);
	else if (name1)
		snprintf(buffer, size, "%s:port.id.%d", name1, p->id);
	else if (name2)
		snprintf(buffer, size, "node.id.%d:%s", n->id, name2);
	else
		snprintf(buffer, size, "node.id.%d:port.id.%d", n->id, p->id);
	return buffer;
}

static char *port_path(char *buffer, int size, struct object *n, struct object *p)
{
	const char *name;
	buffer[0] = '\0';
	if ((name = pw_properties_get(p->props, PW_KEY_OBJECT_PATH)) == NULL)
		snprintf(buffer, size, "port.path.%d", p->id);
	else
		snprintf(buffer, size, "%s", name);
	return buffer;
}

static char *port_alias(char *buffer, int size, struct object *n, struct object *p)
{
	const char *name;
	buffer[0] = '\0';
	if ((name = pw_properties_get(p->props, PW_KEY_PORT_ALIAS)) == NULL)
		snprintf(buffer, size, "port_alias.%d", p->id);
	else
		snprintf(buffer, size, "%s", name);
	return buffer;
}

static void print_port_latency(struct data *data, const char *prefix,
		struct object *p, enum spa_direction direction)
{
	const char *state;
	struct spa_latency_info *info = &p->latency[direction];

	if (p->state == STATE_NONE || p->state == STATE_CHANGED)
		state = p->latency_changed[direction] ? "*" : "=";
	else
		state = state_name(data, p);

	printf("%s%s    %s latency:  { quantum=[ %f %f ], rate=[ %d %d ], ns=[ %"PRIi64" %"PRIi64" ] }\n",
			state, prefix, direction == SPA_DIRECTION_INPUT ? "input ": "output",
			info->min_quantum, info->max_quantum,
			info->min_rate, info->max_rate, info->min_ns, info->max_ns);
	p->latency_changed[direction] = false;
}

static void print_port(struct data *data, const char *prefix, const char *state,
		struct object *n, struct object *p, bool verbose)
{
	char buffer[1024], id[64] = "";
	const char *prefix2 = "";

	if (state == NULL)
		state = state_name(data, p);

	if (data->opt_id) {
		snprintf(id, sizeof(id), "%4d ", p->id);
		prefix2 = "     ";
	}

	printf("%s%s%s%s\n", state, prefix,
			id, port_name(buffer, sizeof(buffer), n, p));
	if (verbose) {
		port_path(buffer, sizeof(buffer), n, p);
		if (buffer[0] != '\0')
			printf("%s  %s%s%s\n", state, prefix2, prefix, buffer);
		port_alias(buffer, sizeof(buffer), n, p);
		if (buffer[0] != '\0')
			printf("%s  %s%s%s\n", state, prefix2, prefix, buffer);
	}
	if (data->opt_list & LIST_LATENCY) {
		print_port_latency(data, "", p, SPA_DIRECTION_INPUT);
		print_port_latency(data, "", p, SPA_DIRECTION_OUTPUT);
	}
}

static void print_port_id(struct data *data, const char *prefix, uint32_t peer, struct object *l)
{
	struct object *n, *p;
	if ((p = find_object(data, OBJECT_PORT, peer)) == NULL)
		return;
	if ((n = find_object(data, OBJECT_NODE, p->data.port.node)) == NULL)
		return;
	print_port(data, prefix, state_name(data, l), n, p, false);
}

static void do_list_port_links(struct data *data, struct object *node, struct object *port)
{
	struct object *o;
	bool first = false;

	if (!(data->opt_list & LIST_PORTS))
		first = true;

	spa_list_for_each(o, &data->objects, link) {
		uint32_t peer;
		char prefix[64], id[16] = "";

		if (data->opt_id)
			snprintf(id, sizeof(id), "%4d ", o->id);

		if (o->type != OBJECT_LINK)
			continue;

		if (port->data.port.direction == PW_DIRECTION_OUTPUT &&
		    o->data.link.output_port == port->id) {
			peer = o->data.link.input_port;
			snprintf(prefix, sizeof(prefix), "%s  |-> ", id);
		}
		else if (port->data.port.direction == PW_DIRECTION_INPUT &&
		    o->data.link.input_port == port->id) {
			peer = o->data.link.output_port;
			snprintf(prefix, sizeof(prefix), "%s  |<- ", id);
		}
		else
			continue;

		if (first) {
			print_port(data, "", NULL, node, port, data->opt_verbose);
			first = false;
		}
		print_port_id(data, prefix, peer, o);
	}
}

static int node_matches(struct data *data, struct object *n, const char *name)
{
	char buffer[1024];
	uint32_t id = atoi(name);
	if (n->id == id)
		return 1;
	if (spa_streq(node_name(buffer, sizeof(buffer), n), name))
		return 1;
	if (spa_streq(node_path(buffer, sizeof(buffer), n), name))
		return 1;
	return 0;
}

static int port_matches(struct data *data, struct object *n, struct object *p, const char *name)
{
	char buffer[1024];
	uint32_t id = atoi(name);
	if (p->id == id)
		return 1;
	if (spa_streq(port_name(buffer, sizeof(buffer), n, p), name))
		return 1;
	if (spa_streq(port_path(buffer, sizeof(buffer), n, p), name))
		return 1;
	if (spa_streq(port_alias(buffer, sizeof(buffer), n, p), name))
		return 1;
	return 0;
}

static int port_regex(struct data *data, struct object *n, struct object *p, regex_t *regex)
{
	char buffer[1024];
	if (regexec(regex, port_name(buffer, sizeof(buffer), n, p), 0, NULL, 0) == 0)
		return 1;
	return 0;
}

static void do_list_ports(struct data *data, struct object *node,
		enum pw_direction direction, regex_t *regex)
{
	struct object *o;
	spa_list_for_each(o, &data->objects, link) {
		if (o->type != OBJECT_PORT)
			continue;
		if (o->state == STATE_NONE)
			continue;
		if (o->data.port.node != node->id)
			continue;
		if (o->data.port.direction != direction)
			continue;

		if (regex && !port_regex(data, node, o, regex))
			continue;

		if (data->opt_list & LIST_PORTS)
			print_port(data, "", NULL, node, o, data->opt_verbose);
		if (data->opt_list & LIST_LINKS)
			do_list_port_links(data, node, o);
	}
}

static void do_list(struct data *data)
{
	struct object *n, *t;

	spa_list_for_each(n, &data->objects, link) {
		if (n->type != OBJECT_NODE)
			continue;
		if (data->list_outputs)
			do_list_ports(data, n, PW_DIRECTION_OUTPUT, data->out_regex);
		if (data->list_inputs)
			do_list_ports(data, n, PW_DIRECTION_INPUT, data->in_regex);

	}
	spa_list_for_each_safe(n, t, &data->objects, link) {
		if (n->state == STATE_DELETE)
			destroy_object(n);
		else
			n->state = STATE_NONE;
	}
}

static int create_link_target(struct data *data)
{
	struct target_link *tl = calloc(1, sizeof(*tl));
	if (!tl)
		return -ENOMEM;

	tl->proxy = pw_core_create_object(data->core,
			"link-factory", PW_TYPE_INTERFACE_Link,
			PW_VERSION_LINK, &data->props->dict, 0);
	if (tl->proxy == NULL) {
		free(tl);
		return -errno;
	}

	tl->data = data;
	tl->state = PW_LINK_STATE_INIT;
	pw_proxy_add_listener(tl->proxy, &tl->listener, &link_proxy_events, tl);
	pw_proxy_add_object_listener(tl->proxy, &tl->link_listener, &link_events, tl);
	spa_list_append(&data->target_links, &tl->link);
	return 0;
}

/*
 * create_link_proxies() looks at the current objects and tries to find the
 * matching output and input nodes (multiple links) or the matching output and
 * input ports.
 *
 * If successful, it fills data->target_links with proxies for all links and
 * returns the number of links. This can be zero (two nodes with no ports). It
 * might return (negative) errors. -ENOENT means no matching nodes or ports
 * were found.
 */
static int create_link_proxies(struct data *data)
{
	uint32_t in_port = 0, out_port = 0;
	struct object *n, *p;
	struct object *in_node = NULL, *out_node = NULL;

	spa_assert(data->opt_output);
	spa_assert(data->opt_input);

	spa_list_for_each(n, &data->objects, link) {
		if (n->type != OBJECT_NODE)
			continue;

		if (out_node == NULL && node_matches(data, n, data->opt_output)) {
			out_node = n;
			continue;
		} else if (in_node == NULL && node_matches(data, n, data->opt_input)) {
			in_node = n;
			continue;
		}

		spa_list_for_each(p, &data->objects, link) {
			if (p->type != OBJECT_PORT)
				continue;
			if (p->data.port.node != n->id)
				continue;

			if (out_port == 0 && p->data.port.direction == PW_DIRECTION_OUTPUT &&
			    port_matches(data, n, p, data->opt_output))
				out_port = p->id;
			else if (in_port == 0 && p->data.port.direction == PW_DIRECTION_INPUT &&
			    port_matches(data, n, p, data->opt_input))
				in_port = p->id;
		}
	}

	if (in_node && out_node) {
		int i;
		char port_id[32];

		for (i=0;; i++) {
			snprintf(port_id, sizeof(port_id), "%d", i);

			struct object *port_out = find_node_port(data, out_node, PW_DIRECTION_OUTPUT, port_id);
			struct object *port_in = find_node_port(data, in_node, PW_DIRECTION_INPUT, port_id);

			if (!port_out || !port_in)
				return i;

			pw_properties_setf(data->props, PW_KEY_LINK_OUTPUT_PORT, "%u", port_out->id);
			pw_properties_setf(data->props, PW_KEY_LINK_INPUT_PORT, "%u", port_in->id);

			int ret = create_link_target(data);
			if (ret)
				return ret;
		}
		return i;
	}

	if (in_port == 0 || out_port == 0)
		return -ENOENT;

	pw_properties_setf(data->props, PW_KEY_LINK_OUTPUT_PORT, "%u", out_port);
	pw_properties_setf(data->props, PW_KEY_LINK_INPUT_PORT, "%u", in_port);

	/*
	 * create_link_target() returns zero on success. We return the number of
	 * links on success, meaning 1.
	 */
	int ret = create_link_target(data);
	return !ret ? 1 : ret;
}

static int do_unlink_ports(struct data *data)
{
	struct object *l, *n, *p;
	bool found_any = false;
	struct object *in_node = NULL, *out_node = NULL;

	spa_assert(data->opt_output);

	if (data->opt_input != NULL) {
		/* 2 args, check if they are node names */
		spa_list_for_each(n, &data->objects, link) {
			if (n->type != OBJECT_NODE)
				continue;

			if (out_node == NULL && node_matches(data, n, data->opt_output)) {
				out_node = n;
				continue;
			} else if (in_node == NULL && node_matches(data, n, data->opt_input)) {
				in_node = n;
				continue;
			}
		}
	}

	spa_list_for_each(l, &data->objects, link) {
		if (l->type != OBJECT_LINK)
			continue;

		if (data->opt_input == NULL) {
			/* 1 arg, check link id */
			if (l->id != (uint32_t)atoi(data->opt_output))
				continue;
		} else if (out_node && in_node) {
			/* 2 args, check nodes */
			if ((p = find_object(data, OBJECT_PORT, l->data.link.output_port)) == NULL)
				continue;
			if ((n = find_object(data, OBJECT_NODE, p->data.port.node)) == NULL)
				continue;
			if (n->id != out_node->id)
				continue;

			if ((p = find_object(data, OBJECT_PORT, l->data.link.input_port)) == NULL)
				continue;
			if ((n = find_object(data, OBJECT_NODE, p->data.port.node)) == NULL)
				continue;
			if (n->id != in_node->id)
				continue;
		} else {
			/* 2 args, check port names */
			if ((p = find_object(data, OBJECT_PORT, l->data.link.output_port)) == NULL)
				continue;
			if ((n = find_object(data, OBJECT_NODE, p->data.port.node)) == NULL)
				continue;
			if (!port_matches(data, n, p, data->opt_output))
				continue;

			if ((p = find_object(data, OBJECT_PORT, l->data.link.input_port)) == NULL)
				continue;
			if ((n = find_object(data, OBJECT_NODE, p->data.port.node)) == NULL)
				continue;
			if (!port_matches(data, n, p, data->opt_input))
				continue;
		}
		pw_registry_destroy(data->registry, l->id);
		found_any = true;
	}
	if (!found_any)
		return -ENOENT;

	core_sync(data);
	pw_main_loop_run(data->loop);

	return 0;
}

static void
removed_proxy (void *data)
{
	struct object *obj = data;
	pw_proxy_destroy(obj->proxy);
}

static void
destroy_proxy (void *data)
{
	struct object *obj = data;
	spa_hook_remove(&obj->proxy_listener);
	spa_hook_remove(&obj->object_listener);
	obj->proxy = NULL;
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = removed_proxy,
	.destroy = destroy_proxy,
};

static void port_event_param(void *_data, int seq, uint32_t id,
		uint32_t index, uint32_t next, const struct spa_pod *param)
{
	struct object *obj = _data;
	struct spa_latency_info info;

	if (id != SPA_PARAM_Latency ||
	    spa_latency_parse(param, &info) < 0)
		return;
	obj->latency[info.direction] = info;
	if (obj->state == STATE_NONE)
		obj->state = STATE_CHANGED;
	obj->latency_changed[info.direction] = true;
	core_sync(obj->d);
}

static const struct pw_port_events port_events = {
	PW_VERSION_PORT_EVENTS,
	.param = port_event_param
};

static void registry_event_global(void *data, uint32_t id, uint32_t permissions,
				  const char *type, uint32_t version,
				  const struct spa_dict *props)
{
	struct data *d = data;
	enum object_type t;
	union object_data extra = {0};
	struct object *obj, *p;
	const char *str;

	if (props == NULL)
		return;

	spa_zero(extra);
	if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
		t = OBJECT_NODE;
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Port)) {
		if (!d->new_object && d->opt_wait && spa_list_is_empty(&d->target_links)) {
			d->new_object = true;
			core_sync(d);
		}

		t = OBJECT_PORT;
		if ((str = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION)) == NULL)
			return;
		if (spa_streq(str, "in"))
			extra.port.direction = PW_DIRECTION_INPUT;
		else if (spa_streq(str, "out"))
			extra.port.direction = PW_DIRECTION_OUTPUT;
		else
			return;
		if ((str = spa_dict_lookup(props, PW_KEY_NODE_ID)) == NULL)
			return;
		extra.port.node = atoi(str);
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Link)) {
		t = OBJECT_LINK;
		if ((str = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_PORT)) == NULL)
			return;
		extra.link.output_port = atoi(str);
		if ((str = spa_dict_lookup(props, PW_KEY_LINK_INPUT_PORT)) == NULL)
			return;
		extra.link.input_port = atoi(str);
		if ((p = find_object(d, OBJECT_PORT, extra.link.output_port)) != NULL)
			if (p->state == STATE_NONE)
				p->state = STATE_CHANGED;
		if ((p = find_object(d, OBJECT_PORT, extra.link.input_port)) != NULL)
			if (p->state == STATE_NONE)
				p->state = STATE_CHANGED;
	} else
		return;

	obj = calloc(1, sizeof(*obj));
	obj->type = t;
	obj->id = id;
	obj->props = pw_properties_new_dict(props);
	obj->data = extra;
	obj->state = STATE_NEW;
	obj->d = d;
	spa_list_append(&d->objects, &obj->link);

	switch (obj->type) {
	case OBJECT_PORT:
	{
		uint32_t subs[] = { SPA_PARAM_Latency };
		obj->proxy = pw_registry_bind(d->registry, id, type, PW_VERSION_PORT, 0);
		pw_proxy_add_object_listener(obj->proxy, &obj->object_listener, &port_events, obj);
		pw_proxy_add_listener(obj->proxy, &obj->proxy_listener, &proxy_events, obj);
		pw_port_subscribe_params((struct pw_port*)obj->proxy, subs, SPA_N_ELEMENTS(subs));
		break;
	}
	default:
		break;
	}
	core_sync(d);
}

static void registry_event_global_remove(void *data, uint32_t id)
{
	struct data *d = data;
	struct object *obj, *p;

	if ((obj = find_object(d, OBJECT_ANY, id)) == NULL)
		return;

	if (obj->type == OBJECT_LINK) {
		if ((p = find_object(d, OBJECT_PORT, obj->data.link.output_port)) != NULL)
			p->state = STATE_CHANGED;
		if ((p = find_object(d, OBJECT_PORT, obj->data.link.input_port)) != NULL)
			p->state = STATE_CHANGED;
	}
	obj->state = STATE_DELETE;
	core_sync(d);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void on_core_done(void *data, uint32_t id, int seq)
{
	struct data *d = data;

	if (d->sync != seq)
		return;

	/* Connect mode, look for our targets. */
	if (d->opt_mode == MODE_CONNECT) {
		d->nb_links = create_link_proxies(d);
		/* In wait mode, if none exist, keep running. */
		if (d->opt_wait && d->nb_links == -ENOENT) {
			d->new_object = false;
			return;
		}
	}

	pw_main_loop_quit(d->loop);

}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct data *d = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_main_loop_quit(d->loop);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
	.error = on_core_error,
};

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	data->opt_monitor = false;
	pw_main_loop_quit(data->loop);
}

static void show_help(struct data *data, const char *name, bool error)
{
	fprintf(error ? stderr : stdout, "%1$s : PipeWire port and link manager.\n"
		"Generic: %1$s [options]\n"
		"  -h, --help                            Show this help\n"
		"      --version                         Show version\n"
		"  -r, --remote=NAME                     Remote daemon name\n"
		"List: %1$s [options] [out-pattern] [in-pattern]\n"
		"  -o, --output                          List output ports\n"
		"  -i, --input                           List input ports\n"
		"  -l, --links                           List links\n"
		"  -t, --latency                         List port latencies\n"
		"  -m, --monitor                         Monitor links and ports\n"
		"  -I, --id                              List IDs\n"
		"  -v, --verbose                         Verbose port properties\n"
		"Connect: %1$s [options] output input\n"
		"  -L, --linger                          Linger (default, unless -m is used)\n"
		"  -P, --passive                         Passive link\n"
		"  -p, --props=PROPS                     Properties as JSON object\n"
		"  -w, --wait                            Wait until link creation attempt\n"
		"Disconnect: %1$s -d [options] output input\n"
		"            %1$s -d [options] link-id\n"
		"  -d, --disconnect                      Disconnect ports\n",
		name);
}

static void data_clear(struct data *data)
{
	struct object *o;
	spa_list_consume(o, &data->objects, link)
		destroy_object(o);

	struct target_link *tl;
	spa_list_consume(tl, &data->target_links, link) {
		if (tl->proxy != NULL) {
			spa_hook_remove(&tl->listener);
			spa_hook_remove(&tl->link_listener);
			pw_proxy_destroy(tl->proxy);
		}
		spa_list_remove(&tl->link);
		free(tl);
	}

	if (data->out_regex)
		regfree(data->out_regex);
	if (data->in_regex)
		regfree(data->in_regex);

	if (data->registry) {
		spa_hook_remove(&data->registry_listener);
		pw_proxy_destroy((struct pw_proxy *) data->registry);
	}

	if (data->core) {
		spa_hook_remove(&data->core_listener);
		pw_core_disconnect(data->core);
	}

	spa_clear_ptr(data->context, pw_context_destroy);
	spa_clear_ptr(data->loop, pw_main_loop_destroy);

	pw_properties_free(data->props);
}

static int run(int argc, char *argv[])
{
	spa_cleanup(data_clear) struct data data = {
		.opt_mode = MODE_CONNECT,
		.objects = SPA_LIST_INIT(&data.objects),
		.target_links = SPA_LIST_INIT(&data.target_links),
	};
	int res = 0, c;
	struct spa_error_location loc;
	static const struct option long_options[] = {
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'V' },
		{ "remote",	required_argument,	NULL, 'r' },
		{ "output",	no_argument,		NULL, 'o' },
		{ "input",	no_argument,		NULL, 'i' },
		{ "links",	no_argument,		NULL, 'l' },
		{ "monitor",	no_argument,		NULL, 'm' },
		{ "id",		no_argument,		NULL, 'I' },
		{ "verbose",	no_argument,		NULL, 'v' },
		{ "linger",	no_argument,		NULL, 'L' },
		{ "passive",	no_argument,		NULL, 'P' },
		{ "props",	required_argument,	NULL, 'p' },
		{ "wait",	no_argument,		NULL, 'w' },
		{ "disconnect",	no_argument,		NULL, 'd' },
		{ "latency",	no_argument,		NULL, 't' },
		{ NULL,	0, NULL, 0}
	};

	data.props = pw_properties_new(NULL, NULL);
	if (data.props == NULL) {
		fprintf(stderr, "can't create properties: %m\n");
		return -1;
	}

	while ((c = getopt_long(argc, argv, "hVr:oilmIvLPp:wdt", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(&data, argv[0], false);
			return 0;
		case 'V':
			printf("%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				argv[0],
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;
		case 'r':
			data.opt_remote = optarg;
			break;
		case 'o':
			data.opt_mode = MODE_LIST;
			data.opt_list |= LIST_OUTPUT;
			break;
		case 'i':
			data.opt_mode = MODE_LIST;
			data.opt_list |= LIST_INPUT;
			break;
		case 'l':
			data.opt_mode = MODE_LIST;
			data.opt_list |= LIST_LINKS;
			break;
		case 't':
			data.opt_mode = MODE_LIST;
			data.opt_list |= LIST_LATENCY;
			break;
		case 'm':
			data.opt_monitor = true;
			break;
		case 'I':
			data.opt_id = true;
			break;
		case 'v':
			data.opt_verbose = true;
			break;
		case 'L':
			pw_properties_set(data.props, PW_KEY_OBJECT_LINGER, "true");
			break;
		case 'P':
			pw_properties_set(data.props, PW_KEY_LINK_PASSIVE, "true");
			break;
		case 'p':
			if (pw_properties_update_string_checked(data.props, optarg, strlen(optarg), &loc) < 0) {
				spa_debug_file_error_location(stderr, &loc,
						"error: syntax error in --props: %s", loc.reason);
				return -1;
			}
			break;
		case 'd':
			data.opt_mode = MODE_DISCONNECT;
			break;
		case 'w':
			data.opt_wait = true;
			break;
		default:
			show_help(&data, argv[0], true);
			return -1;
		}
	}
	if (argc == 1)
		show_help(&data, argv[0], true);

	if (data.opt_id && data.opt_mode != MODE_LIST) {
		fprintf(stderr, "-I option needs one or more of -l, -i or -o\n");
		return -1;
	}

	if (!data.opt_monitor)
		pw_properties_set(data.props, PW_KEY_OBJECT_LINGER, "true");

	if (optind < argc)
		data.opt_output = argv[optind++];
	if (optind < argc)
		data.opt_input = argv[optind++];

	switch (data.opt_mode) {
	case MODE_LIST:
		break;
	case MODE_DISCONNECT:
		if (data.opt_output == NULL) {
			fprintf(stderr, "missing link-id or output and input port names to disconnect\n");
			return -1;
		}
		break;
	case MODE_CONNECT:
		if (data.opt_output == NULL || data.opt_input == NULL) {
			fprintf(stderr, "missing output and input port names to connect\n");
			return -1;
		}
		break;
	}

	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL) {
		fprintf(stderr, "can't create mainloop: %m\n");
		return -1;
	}
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);

	data.context = pw_context_new(pw_main_loop_get_loop(data.loop), NULL, 0);
	if (data.context == NULL) {
		fprintf(stderr, "can't create context: %m\n");
		return -1;
	}

	data.core = pw_context_connect(data.context,
			pw_properties_new(
				PW_KEY_REMOTE_NAME, data.opt_remote,
				NULL),
			0);
	if (data.core == NULL) {
		fprintf(stderr, "can't connect: %m\n");
		return -1;
	}

	pw_core_add_listener(data.core,
			&data.core_listener,
			&core_events, &data);

	data.registry = pw_core_get_registry(data.core,
			PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(data.registry,
			&data.registry_listener,
			&registry_events, &data);

	core_sync(&data);
	pw_main_loop_run(data.loop);

	if ((data.opt_list & (LIST_PORTS|LIST_LINKS)) == LIST_LINKS)
		data.list_inputs = data.list_outputs = true;
	if ((data.opt_list & LIST_INPUT) == LIST_INPUT)
		data.list_inputs = true;
	if ((data.opt_list & LIST_OUTPUT) == LIST_OUTPUT)
		data.list_outputs = true;

	if (data.opt_output) {
		if (regcomp(&data.out_port_regex, data.opt_output, REG_EXTENDED | REG_NOSUB) == 0)
			data.out_regex = &data.out_port_regex;
	}
	if (data.opt_input) {
		if (regcomp(&data.in_port_regex, data.opt_input, REG_EXTENDED | REG_NOSUB) == 0)
			data.in_regex = &data.in_port_regex;
	}

	switch (data.opt_mode) {
	case MODE_LIST:
		do_list(&data);
		break;
	case MODE_DISCONNECT:
		if ((res = do_unlink_ports(&data)) < 0) {
			fprintf(stderr, "failed to unlink ports: %s\n", spa_strerror(res));
			return -1;
		}
		break;
	case MODE_CONNECT:
		if (data.nb_links < 0) {
			fprintf(stderr, "failed to link ports: %s\n", spa_strerror(data.nb_links));
			return -1;
		}

		if (data.nb_links > 0) {
			core_sync(&data);
			pw_main_loop_run(data.loop);

			struct target_link *tl;
			spa_list_for_each(tl, &data.target_links, link) {
				if (tl->state == PW_LINK_STATE_ERROR) {
					fprintf(stderr, "failed to link ports: %s\n",
						spa_strerror(tl->result));
					return -1;
				}
			}
		}
		break;
	}

	while (data.opt_monitor) {
		pw_main_loop_run(data.loop);
		if (data.opt_monitor)
			do_list(&data);
	}

	return 0;
}

int main(int argc, char *argv[])
{
	setlocale(LC_ALL, "");
	setlinebuf(stdout);

	pw_init(&argc, &argv);
	int res = run(argc, argv);
	pw_deinit();

	return res;
}
