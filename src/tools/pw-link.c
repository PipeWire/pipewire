/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
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

#include <stdio.h>
#include <signal.h>
#include <math.h>
#include <getopt.h>
#include <regex.h>

#include <spa/utils/result.h>
#include <spa/utils/defs.h>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

struct object {
	struct spa_list link;

	uint32_t id;
#define OBJECT_NODE	0
#define OBJECT_PORT	1
#define OBJECT_LINK	2
	uint32_t type;
	struct pw_properties *props;
	uint32_t extra[2];
};

struct data {
	struct pw_main_loop *loop;

	const char *opt_remote;
#define MODE_LIST_OUTPUT	(1<<0)
#define MODE_LIST_INPUT		(1<<1)
#define MODE_LIST_PORTS		(MODE_LIST_OUTPUT|MODE_LIST_INPUT)
#define MODE_LIST_LINKS		(1<<2)
#define MODE_LIST		(MODE_LIST_PORTS|MODE_LIST_LINKS)
#define MODE_MONITOR		(1<<3)
#define MODE_DISCONNECT		(1<<4)
	uint32_t opt_mode;
	bool opt_verbose;
	const char *opt_output;
	const char *opt_input;
	struct pw_properties *props;

	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct spa_list objects;

	int sync;
	int link_res;
};

static void link_proxy_error(void *data, int seq, int res, const char *message)
{
	struct data *d = data;
	d->link_res = res;
}

static const struct pw_proxy_events link_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.error = link_proxy_error,
};

static void core_sync(struct data *data)
{
	data->sync = pw_core_sync(data->core, PW_ID_CORE, data->sync);
}

static int create_link(struct data *data)
{
	struct pw_proxy *proxy;
	struct spa_hook listener;

	proxy = pw_core_create_object(data->core,
			"link-factory",
			PW_TYPE_INTERFACE_Link,
			PW_VERSION_LINK,
			&data->props->dict, 0);
	if (proxy == NULL)
		return -errno;

	spa_zero(listener);
	pw_proxy_add_listener(proxy, &listener, &link_proxy_events, data);

	core_sync(data);
	pw_main_loop_run(data->loop);

        spa_hook_remove(&listener);

        pw_proxy_destroy(proxy);

	return data->link_res;
}

static struct object *find_object(struct data *data, uint32_t type, uint32_t id)
{
	struct object *o;
	spa_list_for_each(o, &data->objects, link)
		if (o->type == type && o->id == id)
			return o;
	return NULL;
}

static char *port_name(char *buffer, int size, struct object *n, struct object *p)
{
	const char *name1, *name2;
	buffer[0] = '\0';
	if ((name1 = pw_properties_get(n->props, PW_KEY_NODE_NAME)) == NULL)
		return buffer;
	if ((name2 = pw_properties_get(p->props, PW_KEY_PORT_NAME)) == NULL)
		return buffer;
	snprintf(buffer, size, "%s:%s", name1, name2);
	return buffer;
}

static char *port_path(char *buffer, int size, struct object *n, struct object *p)
{
	const char *name;
	buffer[0] = '\0';
	if ((name = pw_properties_get(p->props, PW_KEY_OBJECT_PATH)) == NULL)
		return buffer;
	snprintf(buffer, size, "%s", name);
	return buffer;
}

static char *port_alias(char *buffer, int size, struct object *n, struct object *p)
{
	const char *name;
	buffer[0] = '\0';
	if ((name = pw_properties_get(p->props, PW_KEY_PORT_ALIAS)) == NULL)
		return buffer;
	snprintf(buffer, size, "%s", name);
	return buffer;
}

static void print_port(struct data *data, const char *prefix, struct object *n,
		struct object *p, bool verbose)
{
	char buffer[1024];
	fprintf(stdout, "%s%s\n", prefix, port_name(buffer, sizeof(buffer), n, p));
	if (verbose) {
		port_path(buffer, sizeof(buffer), n, p);
		if (buffer[0] != '\0')
			fprintf(stdout, "  %s%s\n", prefix, buffer);
		port_alias(buffer, sizeof(buffer), n, p);
		if (buffer[0] != '\0')
			fprintf(stdout, "  %s%s\n", prefix, buffer);
	}
}

static void print_port_id(struct data *data, const char *prefix, uint32_t peer)
{
	struct object *n, *p;
	if ((p = find_object(data, OBJECT_PORT, peer)) == NULL)
		return;
	if ((n = find_object(data, OBJECT_NODE, p->extra[1])) == NULL)
		return;
	print_port(data, prefix, n, p, false);
}

static void do_list_port_links(struct data *data, struct object *node, struct object *port)
{
	struct object *o;
	bool first = false;
	if ((data->opt_mode & MODE_LIST_PORTS) == 0)
		first = true;
	spa_list_for_each(o, &data->objects, link) {
		uint32_t peer;
		const char *prefix;

		if (o->type != OBJECT_LINK)
			continue;
		if (port->extra[0] == PW_DIRECTION_OUTPUT &&
		    o->extra[0] == port->id) {
			peer = o->extra[1];
			prefix = "  |-> ";
		}
		else if (port->extra[0] == PW_DIRECTION_INPUT &&
		    o->extra[1] == port->id) {
			peer = o->extra[0];
			prefix = "  <-| ";
		}
		else
			continue;

		if (first) {
			print_port(data, "", node, port, data->opt_verbose);
			first = false;
		}
		print_port_id(data, prefix, peer);
	}
}

static int port_matches(struct data *data, struct object *n, struct object *p, const char *name)
{
	char buffer[1024];
	if (strcmp(port_name(buffer, sizeof(buffer), n, p), name) == 0)
		return 1;
	if (strcmp(port_path(buffer, sizeof(buffer), n, p), name) == 0)
		return 1;
	if (strcmp(port_alias(buffer, sizeof(buffer), n, p), name) == 0)
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
		if (o->extra[1] != node->id)
			continue;
		if (o->extra[0] != direction)
			continue;

		if (regex && !port_regex(data, node, o, regex))
			continue;

		if (data->opt_mode & MODE_LIST_PORTS)
			print_port(data, "", node, o, data->opt_verbose);
		if (data->opt_mode & MODE_LIST_LINKS)
			do_list_port_links(data, node, o);
	}
}

static void do_list(struct data *data)
{
	struct object *n;
	regex_t out_port_regex, *out_regex = NULL;
	regex_t in_port_regex, *in_regex = NULL;
	bool list_inputs = false, list_outputs = false;

	if ((data->opt_mode & (MODE_LIST_PORTS|MODE_LIST_LINKS)) == MODE_LIST_LINKS)
		list_inputs = list_outputs = true;
	if ((data->opt_mode & MODE_LIST_INPUT) == MODE_LIST_INPUT)
		list_inputs = true;
	if ((data->opt_mode & MODE_LIST_OUTPUT) == MODE_LIST_OUTPUT)
		list_outputs = true;

	if (data->opt_output) {
		if (regcomp(&out_port_regex, data->opt_output, REG_EXTENDED | REG_NOSUB) == 0)
			out_regex = &out_port_regex;
	}
	if (data->opt_input) {
		if (regcomp(&in_port_regex, data->opt_input, REG_EXTENDED | REG_NOSUB) == 0)
			in_regex = &in_port_regex;
	}

	spa_list_for_each(n, &data->objects, link) {
		if (n->type != OBJECT_NODE)
			continue;
		if (list_outputs)
			do_list_ports(data, n, PW_DIRECTION_OUTPUT, out_regex);
		if (list_inputs)
			do_list_ports(data, n, PW_DIRECTION_INPUT, in_regex);
	}
	if (out_regex)
		regfree(out_regex);
	if (in_regex)
		regfree(in_regex);
}

static int do_link_ports(struct data *data)
{
	uint32_t in_port = 0, out_port = 0;
	struct object *n, *p;

	spa_list_for_each(n, &data->objects, link) {
		if (n->type != OBJECT_NODE)
			continue;

		spa_list_for_each(p, &data->objects, link) {
			if (p->type != OBJECT_PORT)
				continue;
			if (p->extra[1] != n->id)
				continue;

			if (out_port == 0 && p->extra[0] == PW_DIRECTION_OUTPUT &&
			    port_matches(data, n, p, data->opt_output))
				out_port = p->id;
			else if (in_port == 0 && p->extra[0] == PW_DIRECTION_INPUT &&
			    port_matches(data, n, p, data->opt_input))
				in_port = p->id;
		}
	}
	if (in_port == 0 || out_port == 0)
		return -ENOENT;

	pw_properties_setf(data->props, PW_KEY_LINK_OUTPUT_PORT, "%u", out_port);
	pw_properties_setf(data->props, PW_KEY_LINK_INPUT_PORT, "%u", in_port);

	return create_link(data);
}

static int do_unlink_ports(struct data *data)
{
	struct object *l, *n, *p;
	uint32_t link_id = 0;

	spa_list_for_each(l, &data->objects, link) {
		if (l->type != OBJECT_LINK)
			continue;

		if ((p = find_object(data, OBJECT_PORT, l->extra[0])) == NULL)
			continue;
		if ((n = find_object(data, OBJECT_NODE, p->extra[1])) == NULL)
			continue;
		if (!port_matches(data, n, p, data->opt_output))
			continue;

		if ((p = find_object(data, OBJECT_PORT, l->extra[1])) == NULL)
			continue;
		if ((n = find_object(data, OBJECT_NODE, p->extra[1])) == NULL)
			continue;
		if (!port_matches(data, n, p, data->opt_input))
			continue;

		link_id = l->id;
		break;
	}
	if (link_id == 0)
		return -ENOENT;

	pw_registry_destroy(data->registry, link_id);

	core_sync(data);
	pw_main_loop_run(data->loop);

	return 0;
}

static void registry_event_global(void *data, uint32_t id, uint32_t permissions,
				  const char *type, uint32_t version,
				  const struct spa_dict *props)
{
	struct data *d = data;
	uint32_t t, extra[2];
	struct object *obj;
	const char *str;

	if (props == NULL)
		return;

	spa_zero(extra);
	if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
		t = OBJECT_NODE;
	} else if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
		t = OBJECT_PORT;
		if ((str = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION)) == NULL)
			return;
		if (strcmp(str, "in") == 0)
			extra[0] = PW_DIRECTION_INPUT;
		else if (strcmp(str, "out") == 0)
			extra[0] = PW_DIRECTION_OUTPUT;
		else
			return;
		if ((str = spa_dict_lookup(props, PW_KEY_NODE_ID)) == NULL)
			return;
		extra[1] = atoi(str);
	} else if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
		t = OBJECT_LINK;
		if ((str = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_PORT)) == NULL)
			return;
		extra[0] = atoi(str);
		if ((str = spa_dict_lookup(props, PW_KEY_LINK_INPUT_PORT)) == NULL)
			return;
		extra[1] = atoi(str);
	} else
		return;

	obj = calloc(1, sizeof(*obj));
	obj->type = t;
	obj->id = id;
	obj->props = pw_properties_new_dict(props);
	memcpy(obj->extra, extra, sizeof(extra));
	spa_list_append(&d->objects, &obj->link);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
};

static void on_core_done(void *data, uint32_t id, int seq)
{
	struct data *d = data;
	if (d->sync == seq)
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
	pw_main_loop_quit(data->loop);
}

static void show_help(struct data *data, const char *name)
{
        fprintf(stdout, "%1$s : PipeWire port and link manager.\n"
		"Generic: %1$s [options]\n"
		"  -h, --help                            Show this help\n"
		"      --version                         Show version\n"
		"  -r, --remote=NAME                     Remote daemon name\n"
		"List: %1$s [options] [out-pattern] [in-pattern]\n"
		"  -o, --output                          List output ports\n"
		"  -i, --input                           List input ports\n"
		"  -l, --links                           List links\n"
		"  -m, --monitor                         Monitor links\n"
		"  -v, --verbose                         Verbose port properties\n"
		"Connect: %1$s [options] output input\n"
		"  -L, --linger                          Linger (for use with -m)\n"
		"  -P, --passive                         Passive link\n"
		"  -p, --props=PROPS                     Properties as JSON object\n"
		"Disconnect: %1$s -d [options] output input\n"
		"  -d, --disconnect                      Disconnect ports\n",
		name);
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	int res = 0, c;
	static const struct option long_options[] = {
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'V' },
		{ "remote",	required_argument,	NULL, 'r' },
		{ "output",	no_argument,		NULL, 'o' },
		{ "input",	no_argument,		NULL, 'i' },
		{ "links",	no_argument,		NULL, 'l' },
		{ "monitor",	no_argument,		NULL, 'm' },
		{ "verbose",	no_argument,		NULL, 'v' },
		{ "linger",	no_argument,		NULL, 'L' },
		{ "passive",	no_argument,		NULL, 'P' },
		{ "props",	required_argument,	NULL, 'p' },
		{ "disconnect",	no_argument,		NULL, 'd' },
		{ NULL,	0, NULL, 0}
	};

	pw_init(&argc, &argv);
	spa_list_init(&data.objects);

	data.props = pw_properties_new(NULL, NULL);
	if (data.props == NULL) {
		fprintf(stderr, "can't create properties: %m\n");
		return -1;
	}

	while ((c = getopt_long(argc, argv, "hVr:oilmvLPp:d", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(&data, argv[0]);
			return 0;
		case 'V':
			fprintf(stdout, "%s\n"
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
			data.opt_mode |= MODE_LIST_OUTPUT;
			break;
		case 'i':
			data.opt_mode |= MODE_LIST_INPUT;
			break;
		case 'l':
			data.opt_mode |= MODE_LIST_LINKS;
			break;
		case 'm':
			data.opt_mode |= MODE_MONITOR;
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
			pw_properties_update_string(data.props, optarg, strlen(optarg));
			break;
		case 'd':
			data.opt_mode |= MODE_DISCONNECT;
			break;
		default:
			show_help(&data, argv[0]);
			return -1;
		}
	}
	if (argc == 1)
		show_help(&data, argv[0]);

	if ((data.opt_mode & MODE_MONITOR) == 0)
		pw_properties_set(data.props, PW_KEY_OBJECT_LINGER, "true");

	if (optind < argc)
		data.opt_output = argv[optind++];
	if (optind < argc)
		data.opt_input = argv[optind++];

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

	if (data.opt_mode & (MODE_LIST)) {
		do_list(&data);
	} else if (data.opt_mode & MODE_DISCONNECT) {
		if (data.opt_output == NULL ||
		    data.opt_input == NULL) {
			fprintf(stderr, "missing output and input port names\n");
			return -1;
		}
		if ((res = do_unlink_ports(&data)) < 0) {
			fprintf(stderr, "failed to unlink ports: %s\n", spa_strerror(res));
		}
	} else {
		if (data.opt_output == NULL ||
		    data.opt_input == NULL) {
			fprintf(stderr, "missing output and input port names\n");
			return -1;
		}
		if ((res = do_link_ports(&data)) < 0) {
			fprintf(stderr, "failed to link ports: %s\n", spa_strerror(res));
		}
	}

	if (data.opt_mode & MODE_MONITOR)
		pw_main_loop_run(data.loop);

	pw_proxy_destroy((struct pw_proxy*)data.registry);
	pw_core_disconnect(data.core);
	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);
	pw_deinit();

	return res;
}
