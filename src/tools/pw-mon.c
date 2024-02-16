/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <locale.h>

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/ansi.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>

#include <pipewire/pipewire.h>

struct proxy_data;

typedef void (*print_func_t) (struct proxy_data *data);

static struct pprefix {
	const char *prefix;
	const char *suffix;
} pprefix[2] = {
	{ .prefix = " ", .suffix = "" },
	{ .prefix = "*", .suffix = "" },
};

#define with_prefix(use_prefix_) \
   for (bool once_ = !!printf("%s", (pprefix[!!(use_prefix_)]).prefix); \
	once_; \
	once_ = false, printf("%s", (pprefix[!!(use_prefix_)]).suffix))


struct param {
	struct spa_list link;
	uint32_t id;
	int seq;
	struct spa_pod *param;
	unsigned int changed:1;
};

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct spa_list pending_list;
	struct spa_list global_list;

	bool hide_params;
	bool hide_props;
};

struct proxy_data {
	struct data *data;
	bool first;
	struct pw_proxy *proxy;
	uint32_t id;
	uint32_t permissions;
	uint32_t version;
	char *type;
	void *info;
	pw_destroy_t destroy;
	struct spa_hook proxy_listener;
	struct spa_hook object_listener;
	int pending_seq;
	struct spa_list global_link;
	struct spa_list pending_link;
	print_func_t print_func;
	struct spa_list param_list;
};

static void add_pending(struct proxy_data *pd)
{
	struct data *d = pd->data;

	if (pd->pending_seq == 0) {
		spa_list_append(&d->pending_list, &pd->pending_link);
	}
	pd->pending_seq = pw_core_sync(d->core, 0, pd->pending_seq);
}

static void remove_pending(struct proxy_data *pd)
{
	if (pd->pending_seq != 0) {
		spa_list_remove(&pd->pending_link);
		pd->pending_seq = 0;
	}
}

static void on_core_done(void *data, uint32_t id, int seq)
{
	struct data *d = data;
	struct proxy_data *pd, *t;

	spa_list_for_each_safe(pd, t, &d->pending_list, pending_link) {
		if (pd->pending_seq == seq) {
			remove_pending(pd);
			pd->print_func(pd);
		}
	}
}

static void clear_params(struct proxy_data *data)
{
	struct param *p;
	spa_list_consume(p, &data->param_list, link) {
		spa_list_remove(&p->link);
		free(p);
	}
}

static void remove_params(struct proxy_data *data, uint32_t id, int seq)
{
	struct param *p, *t;

	spa_list_for_each_safe(p, t, &data->param_list, link) {
		if (p->id == id && seq != p->seq) {
			spa_list_remove(&p->link);
			free(p);
		}
	}
}

static void event_param(void *_data, int seq, uint32_t id,
		uint32_t index, uint32_t next, const struct spa_pod *param)
{
        struct proxy_data *data = _data;
	struct param *p;

	/* remove all params with the same id and older seq */
	remove_params(data, id, seq);

	/* add new param */
	p = malloc(sizeof(struct param) + SPA_POD_SIZE(param));
	if (p == NULL) {
		pw_log_error("can't add param: %m");
		return;
	}

	p->id = id;
	p->seq = seq;
	p->param = SPA_PTROFF(p, sizeof(struct param), struct spa_pod);
	p->changed = true;
	memcpy(p->param, param, SPA_POD_SIZE(param));
	spa_list_append(&data->param_list, &p->link);
}

static void print_parameters(struct proxy_data *data, bool use_prefix)
{
	struct param *p;

	with_prefix(use_prefix) {
		printf("\tparams:\n");
	}

	spa_list_for_each(p, &data->param_list, link) {
		with_prefix(p->changed) {
			printf("\t  id:%u (%s)\n",
				p->id,
				spa_debug_type_find_name(spa_type_param, p->id));
			if (spa_pod_is_object_type(p->param, SPA_TYPE_OBJECT_Format))
				spa_debug_format(10, NULL, p->param);
			else
				spa_debug_pod(10, NULL, p->param);
		}
		p->changed = false;
	}
}

static void print_properties(const struct spa_dict *props, bool use_prefix)
{
	const struct spa_dict_item *item;

	with_prefix(use_prefix) {
		printf("\tproperties:\n");
		if (props == NULL || props->n_items == 0) {
			printf("\t\tnone\n");
			return;
		}
	}

	spa_dict_for_each(item, props) {
		with_prefix(use_prefix) {
			if (item->value)
				printf("\t\t%s = \"%s\"\n", item->key, item->value);
			else
				printf("\t\t%s = (null)\n", item->key);
		}
	}
}

#define MARK_CHANGE(f) (!!(print_mark && ((info)->change_mask & (f))))

static void on_core_info(void *_data, const struct pw_core_info *info)
{
	struct proxy_data *data = _data;
	bool hide_props, print_mark = true;

	hide_props = data->data->hide_props;

	printf("\ttype: %s\n", PW_TYPE_INTERFACE_Core);
	printf("\tcookie: %u\n", info->cookie);
	printf("\tuser-name: \"%s\"\n", info->user_name);
	printf("\thost-name: \"%s\"\n", info->host_name);
	printf("\tversion: \"%s\"\n", info->version);
	printf("\tname: \"%s\"\n", info->name);
	if (!hide_props) {
		print_properties(info->props, MARK_CHANGE(PW_CORE_CHANGE_MASK_PROPS));
	}
}

static void module_event_info(void *_data, const struct pw_module_info *info)
{
	struct proxy_data *data = _data;
	bool hide_props,  print_mark;

	hide_props = data->data->hide_props;
	if (data->info == NULL) {
		printf("added:\n");
		print_mark = false;
	}
	else {
		printf("changed:\n");
		print_mark = true;
	}

	info = data->info = pw_module_info_update(data->info, info);

	printf("\tid: %d\n", data->id);
	printf("\tpermissions: "PW_PERMISSION_FORMAT"\n",
			PW_PERMISSION_ARGS(data->permissions));
	printf("\ttype: %s (version %d)\n", data->type, data->version);
	printf("\tname: \"%s\"\n", info->name);
	printf("\tfilename: \"%s\"\n", info->filename);
	printf("\targs: \"%s\"\n", info->args);
	if (!hide_props) {
		print_properties(info->props, MARK_CHANGE(PW_MODULE_CHANGE_MASK_PROPS));
	}
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
        .info = module_event_info,
};

static void print_node(struct proxy_data *data)
{
	struct pw_node_info *info = data->info;
	bool hide_params, hide_props, print_mark;

	hide_params = data->data->hide_params;
	hide_props = data->data->hide_props;

	if (data->first) {
		printf("added:\n");
		print_mark = false;
		data->first = false;
	}
	else {
		printf("changed:\n");
		print_mark = true;
	}

	printf("\tid: %d\n", data->id);
	printf("\tpermissions: "PW_PERMISSION_FORMAT"\n",
			PW_PERMISSION_ARGS(data->permissions));
	printf("\ttype: %s (version %d)\n", data->type, data->version);
	if (!hide_params) {
		print_parameters(data, MARK_CHANGE(PW_NODE_CHANGE_MASK_PARAMS));
		with_prefix(MARK_CHANGE(PW_NODE_CHANGE_MASK_INPUT_PORTS)) {
			printf("\tinput ports: %u/%u\n",
				info->n_input_ports, info->max_input_ports);
		}
		with_prefix(MARK_CHANGE(PW_NODE_CHANGE_MASK_OUTPUT_PORTS)) {
			printf("\toutput ports: %u/%u\n",
				info->n_output_ports, info->max_output_ports);
		}
		with_prefix(MARK_CHANGE(PW_NODE_CHANGE_MASK_STATE)) {
			printf("\tstate: \"%s\"",
				pw_node_state_as_string(info->state));
		}
		if (info->state == PW_NODE_STATE_ERROR && info->error)
			printf(" \"%s\"\n", info->error);
		else
			printf("\n");
	}

	if (!hide_props) {
		print_properties(info->props, MARK_CHANGE(PW_NODE_CHANGE_MASK_PROPS));
	}
}

static void node_event_info(void *_data, const struct pw_node_info *info)
{
        struct proxy_data *data = _data;
	uint32_t i;

	info = data->info = pw_node_info_update(data->info, info);

	if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			if (info->params[i].user == 0)
				continue;
			remove_params(data, info->params[i].id, 0);
			if (!SPA_FLAG_IS_SET(info->params[i].flags, SPA_PARAM_INFO_READ))
				continue;
			pw_node_enum_params((struct pw_node*)data->proxy,
					0, info->params[i].id, 0, 0, NULL);
			info->params[i].user = 0;
		}
		add_pending(data);
	}

	if (data->pending_seq == 0)
		data->print_func(data);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
        .info = node_event_info,
        .param = event_param
};

static void print_port(struct proxy_data *data)
{
	struct pw_port_info *info = data->info;
	bool hide_params, hide_props, print_mark;

	hide_params = data->data->hide_params;
	hide_props = data->data->hide_props;

	if (data->first) {
		printf("added:\n");
		print_mark = false;
		data->first = false;
	}
	else {
		printf("changed:\n");
		print_mark = true;
	}

	printf("\tid: %d\n", data->id);
	printf("\tpermissions: "PW_PERMISSION_FORMAT"\n",
			PW_PERMISSION_ARGS(data->permissions));
	printf("\ttype: %s (version %d)\n", data->type, data->version);

	printf("\tdirection: \"%s\"\n", pw_direction_as_string(info->direction));

	if (!hide_params) {
		print_parameters(data, MARK_CHANGE(PW_PORT_CHANGE_MASK_PARAMS));
	}

	if (!hide_props) {
		print_properties(info->props, MARK_CHANGE(PW_PORT_CHANGE_MASK_PROPS));
	}
}

static void port_event_info(void *_data, const struct pw_port_info *info)
{
        struct proxy_data *data = _data;
	uint32_t i;

	info = data->info = pw_port_info_update(data->info, info);

	if (info->change_mask & PW_PORT_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			if (info->params[i].user == 0)
				continue;
			remove_params(data, info->params[i].id, 0);
			if (!SPA_FLAG_IS_SET(info->params[i].flags, SPA_PARAM_INFO_READ))
				continue;
			pw_port_enum_params((struct pw_port*)data->proxy,
					0, info->params[i].id, 0, 0, NULL);
			info->params[i].user = 0;
		}
		add_pending(data);
	}

	if (data->pending_seq == 0)
		data->print_func(data);
}

static const struct pw_port_events port_events = {
	PW_VERSION_PORT_EVENTS,
        .info = port_event_info,
        .param = event_param
};

static void factory_event_info(void *_data, const struct pw_factory_info *info)
{
        struct proxy_data *data = _data;
	bool hide_props, print_mark;

	hide_props = data->data->hide_props;

	if (data->info == NULL) {
		printf("added:\n");
		print_mark = false;
	}
	else {
		printf("changed:\n");
		print_mark = true;
	}

        info = data->info = pw_factory_info_update(data->info, info);

	printf("\tid: %d\n", data->id);
	printf("\tpermissions: "PW_PERMISSION_FORMAT"\n",
			PW_PERMISSION_ARGS(data->permissions));
	printf("\ttype: %s (version %d)\n", data->type, data->version);

	printf("\tname: \"%s\"\n", info->name);
	printf("\tobject-type: %s/%d\n", info->type, info->version);
	if (!hide_props) {
		print_properties(info->props, MARK_CHANGE(PW_FACTORY_CHANGE_MASK_PROPS));
	}
}

static const struct pw_factory_events factory_events = {
	PW_VERSION_FACTORY_EVENTS,
        .info = factory_event_info
};

static void client_event_info(void *_data, const struct pw_client_info *info)
{
        struct proxy_data *data = _data;
	bool hide_props, print_mark;

	hide_props = data->data->hide_props;

	if (data->info == NULL) {
		printf("added:\n");
		print_mark = false;
	}
	else {
		printf("changed:\n");
		print_mark = true;
	}

        info = data->info = pw_client_info_update(data->info, info);

	printf("\tid: %d\n", data->id);
	printf("\tpermissions: "PW_PERMISSION_FORMAT"\n",
			PW_PERMISSION_ARGS(data->permissions));
	printf("\ttype: %s (version %d)\n", data->type, data->version);

	if (!hide_props) {
		print_properties(info->props, MARK_CHANGE(PW_CLIENT_CHANGE_MASK_PROPS));
	}
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
        .info = client_event_info
};

static void link_event_info(void *_data, const struct pw_link_info *info)
{
        struct proxy_data *data = _data;
	bool hide_props, print_mark;

	hide_props = data->data->hide_props;

	if (data->info == NULL) {
		printf("added:\n");
		print_mark = false;
	}
	else {
		printf("changed:\n");
		print_mark = true;
	}

        info = data->info = pw_link_info_update(data->info, info);

	printf("\tid: %d\n", data->id);
	printf("\tpermissions: "PW_PERMISSION_FORMAT"\n",
			PW_PERMISSION_ARGS(data->permissions));
	printf("\ttype: %s (version %d)\n", data->type, data->version);

	printf("\toutput-node-id: %u\n", info->output_node_id);
	printf("\toutput-port-id: %u\n", info->output_port_id);
	printf("\tinput-node-id: %u\n", info->input_node_id);
	printf("\tinput-port-id: %u\n", info->input_port_id);
	if (!hide_props) {
		with_prefix(MARK_CHANGE(PW_LINK_CHANGE_MASK_STATE)) {
			printf("\tstate: \"%s\"",
				pw_link_state_as_string(info->state));
		}
		if (info->state == PW_LINK_STATE_ERROR && info->error)
			printf(" \"%s\"\n", info->error);
		else
			printf("\n");
		with_prefix(MARK_CHANGE(PW_LINK_CHANGE_MASK_FORMAT)) {
			printf("\tformat:\n");
			if (info->format)
				spa_debug_format(2, NULL, info->format);
			else
				printf("\t\tnone\n");
		}
		print_properties(info->props, MARK_CHANGE(PW_LINK_CHANGE_MASK_PROPS));
	}
}

static const struct pw_link_events link_events = {
	PW_VERSION_LINK_EVENTS,
	.info = link_event_info
};

static void print_device(struct proxy_data *data)
{
	struct pw_device_info *info = data->info;
	bool hide_params, hide_props, print_mark;

	hide_params = data->data->hide_params;
	hide_props = data->data->hide_props;

	if (data->first) {
		printf("added:\n");
		print_mark = false;
		data->first = false;
	}
	else {
		printf("changed:\n");
		print_mark = true;
	}

	printf("\tid: %d\n", data->id);
	printf("\tpermissions: "PW_PERMISSION_FORMAT"\n",
			PW_PERMISSION_ARGS(data->permissions));
	printf("\ttype: %s (version %d)\n", data->type, data->version);

	if (!hide_params) {
		print_parameters(data, MARK_CHANGE(PW_DEVICE_CHANGE_MASK_PARAMS));
	}

	if (!hide_props) {
		print_properties(info->props, MARK_CHANGE(PW_DEVICE_CHANGE_MASK_PROPS));
	}
}


static void device_event_info(void *_data, const struct pw_device_info *info)
{
        struct proxy_data *data = _data;
	uint32_t i;

	info = data->info = pw_device_info_update(data->info, info);

	if (info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			if (info->params[i].user == 0)
				continue;
			remove_params(data, info->params[i].id, 0);
			if (!SPA_FLAG_IS_SET(info->params[i].flags, SPA_PARAM_INFO_READ))
				continue;
			pw_device_enum_params((struct pw_device*)data->proxy,
					0, info->params[i].id, 0, 0, NULL);
			info->params[i].user = 0;
		}
		add_pending(data);
	}
	if (data->pending_seq == 0)
		data->print_func(data);
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.info = device_event_info,
        .param = event_param
};

static void
removed_proxy (void *data)
{
        struct proxy_data *pd = data;
	pw_proxy_destroy(pd->proxy);
}

static void
destroy_proxy (void *data)
{
	struct proxy_data *pd = data;

	spa_list_remove(&pd->global_link);

	spa_hook_remove(&pd->object_listener);
	spa_hook_remove(&pd->proxy_listener);

	clear_params(pd);
	remove_pending(pd);
	free(pd->type);

	if (pd->info == NULL)
		return;
	if (pd->destroy)
		pd->destroy(pd->info);
	pd->info = NULL;
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = removed_proxy,
	.destroy = destroy_proxy,
};

static void registry_event_global(void *data, uint32_t id,
				  uint32_t permissions, const char *type, uint32_t version,
				  const struct spa_dict *props)
{
	struct data *d = data;
	struct pw_proxy *proxy;
	uint32_t client_version;
	const void *events;
	struct proxy_data *pd;
	pw_destroy_t destroy;
	print_func_t print_func = NULL;

	if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
		events = &node_events;
		client_version = PW_VERSION_NODE;
		destroy = (pw_destroy_t) pw_node_info_free;
		print_func = print_node;
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Port)) {
		events = &port_events;
		client_version = PW_VERSION_PORT;
		destroy = (pw_destroy_t) pw_port_info_free;
		print_func = print_port;
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Module)) {
		events = &module_events;
		client_version = PW_VERSION_MODULE;
		destroy = (pw_destroy_t) pw_module_info_free;
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Device)) {
		events = &device_events;
		client_version = PW_VERSION_DEVICE;
		destroy = (pw_destroy_t) pw_device_info_free;
		print_func = print_device;
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Factory)) {
		events = &factory_events;
		client_version = PW_VERSION_FACTORY;
		destroy = (pw_destroy_t) pw_factory_info_free;
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Client)) {
		events = &client_events;
		client_version = PW_VERSION_CLIENT;
		destroy = (pw_destroy_t) pw_client_info_free;
	} else if (spa_streq(type, PW_TYPE_INTERFACE_Link)) {
		events = &link_events;
		client_version = PW_VERSION_LINK;
		destroy = (pw_destroy_t) pw_link_info_free;
	} else {
		printf("added:\n");
		printf("\tid: %u\n", id);
		printf("\tpermissions: "PW_PERMISSION_FORMAT"\n",
				PW_PERMISSION_ARGS(permissions));
		printf("\ttype: %s (version %d)\n", type, version);
		print_properties(props, false);
		return;
	}

	proxy = pw_registry_bind(d->registry, id, type,
				       client_version,
				       sizeof(struct proxy_data));
	if (proxy == NULL)
		goto no_mem;

	pd = pw_proxy_get_user_data(proxy);
	pd->data = d;
	pd->first = true;
	pd->proxy = proxy;
	pd->id = id;
	pd->permissions = permissions;
	pd->version = version;
	pd->type = strdup(type);
	pd->destroy = destroy;
	pd->pending_seq = 0;
	pd->print_func = print_func;
	spa_list_init(&pd->param_list);
	pw_proxy_add_object_listener(proxy, &pd->object_listener, events, pd);
	pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);
	spa_list_append(&d->global_list, &pd->global_link);

	return;

no_mem:
	fprintf(stderr, "failed to create proxy");
	return;
}

static struct proxy_data *find_proxy(struct data *d, uint32_t id)
{
	struct proxy_data *pd;
	spa_list_for_each(pd, &d->global_list, global_link) {
		if (pd->id == id)
			return pd;
	}
	return NULL;
}

static void registry_event_global_remove(void *data, uint32_t id)
{
	struct data *d = data;
	struct proxy_data *pd;

	printf("removed:\n");
	printf("\tid: %u\n", id);

	pd = find_proxy(d, id);
	if (pd == NULL)
		return;
	if (pd->proxy)
		pw_proxy_destroy(pd->proxy);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void on_core_error(void *_data, uint32_t id, int seq, int res, const char *message)
{
	struct data *data = _data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_main_loop_quit(data->loop);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.info = on_core_info,
	.done = on_core_done,
	.error = on_core_error,
};

static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

static void show_help(const char *name, bool error)
{
        fprintf(error ? stderr : stdout, "%s [options]\n"
		"  -h, --help                            Show this help\n"
		"      --version                         Show version\n"
		"  -r, --remote                          Remote daemon name\n"
		"  -N, --no-colors                       disable color output\n"
		"  -C, --color[=WHEN]                    whether to enable color support. WHEN is `never`, `always`, or `auto`\n"
		"  -o, --hide-props                      hide node properties\n"
		"  -a, --hide-params                     hide node properties\n",
		name);
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct pw_loop *l;
	const char *opt_remote = NULL;
	static const struct option long_options[] = {
		{ "help",		no_argument,		NULL, 'h' },
		{ "version",		no_argument,		NULL, 'V' },
		{ "remote",		required_argument,	NULL, 'r' },
		{ "no-colors",		no_argument,		NULL, 'N' },
		{ "color",		optional_argument,	NULL, 'C' },
		{ "hide-props",		no_argument,		NULL, 'o' },
		{ "hide-params",	no_argument,		NULL, 'a' },
		{ NULL,	0, NULL, 0}
	};
	int c;
	bool colors = false;

	setlocale(LC_ALL, "");
	pw_init(&argc, &argv);

	setlinebuf(stdout);

	if (getenv("NO_COLOR") == NULL && isatty(STDOUT_FILENO))
		colors = true;

	while ((c = getopt_long(argc, argv, "hVr:NCoa", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(argv[0], false);
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
			opt_remote = optarg;
			break;
		case 'N' :
			colors = false;
			break;
		case 'C' :
			if (optarg == NULL || !strcmp(optarg, "auto"))
				break; /* nothing to do, tty detection was done
					  before parsing options */
			else if (!strcmp(optarg, "never"))
				colors = false;
			else if (!strcmp(optarg, "always"))
				colors = true;
			else {
				fprintf(stderr, "Invalid color: %s\n", optarg);
				show_help(argv[0], true);
				return -1;
			}
			break;
		case 'o':
			data.hide_props = true;
			break;
		case 'a':
			data.hide_params = true;
			break;
		default:
			show_help(argv[0], true);
			return -1;
		}
	}

	if (colors) {
		pprefix[1].prefix = SPA_ANSI_RED "*";
		pprefix[1].suffix = SPA_ANSI_RESET;
	}

	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL) {
		fprintf(stderr, "can't create main loop: %m\n");
		return -1;
	}

	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	data.context = pw_context_new(l, NULL, 0);
	if (data.context == NULL) {
		fprintf(stderr, "can't create context: %m\n");
		return -1;
	}

	spa_list_init(&data.pending_list);
	spa_list_init(&data.global_list);

	data.core = pw_context_connect(data.context,
			pw_properties_new(
				PW_KEY_REMOTE_NAME, opt_remote ? opt_remote :
					("[" PW_DEFAULT_REMOTE "-manager," PW_DEFAULT_REMOTE "]"),
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

	pw_main_loop_run(data.loop);

	spa_hook_remove(&data.registry_listener);
	pw_proxy_destroy((struct pw_proxy*)data.registry);
	spa_hook_remove(&data.core_listener);
	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);
	pw_deinit();

	return 0;
}
