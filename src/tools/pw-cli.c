/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#if !defined(__FreeBSD__) && !defined(__MidnightBSD__)
#include <alloca.h>
#endif
#include <getopt.h>
#include <fnmatch.h>
#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif
#include <locale.h>

#if !defined(FNM_EXTMATCH)
#define FNM_EXTMATCH 0
#endif

#define spa_debug(fmt,...) printf(fmt"\n", ## __VA_ARGS__)

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/debug/pod.h>
#include <spa/utils/keys.h>
#include <spa/utils/json-pod.h>
#include <spa/pod/builder.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>

#include <pipewire/extensions/session-manager.h>

static const char WHITESPACE[] = " \t";
static char prompt[64];

struct remote_data;
struct proxy_data;

typedef void (*info_func_t) (struct proxy_data *pd);

struct class {
	const char *type;
	uint32_t version;
	const void *events;
	pw_destroy_t destroy;
	info_func_t info;
	const char *name_key;
};

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;

	struct spa_list remotes;
	struct remote_data *current;

	struct pw_map vars;
	unsigned int interactive:1;
	unsigned int monitoring:1;
	unsigned int quit:1;
};

struct global {
	struct remote_data *rd;
	uint32_t id;
	uint32_t permissions;
	uint32_t version;
	char *type;
	const struct class *class;
	struct pw_proxy *proxy;
	bool info_pending;
	struct pw_properties *properties;
};

struct remote_data {
	struct spa_list link;
	struct data *data;

	char *name;
	uint32_t id;

	int prompt_pending;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct spa_hook proxy_core_listener;
	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct pw_map globals;
};


struct proxy_data {
	struct remote_data *rd;
	struct global *global;
	struct pw_proxy *proxy;
	void *info;
	const struct class *class;
	struct spa_hook proxy_listener;
	struct spa_hook object_listener;
};

struct command {
	const char *name;
	const char *alias;
	const char *description;
	bool (*func) (struct data *data, const char *cmd, char *args, char **error);
};

static struct spa_dict * global_props(struct global *global);
static struct global * obj_global(struct remote_data *rd, uint32_t id);
static int children_of(struct remote_data *rd, uint32_t parent_id,
	const char *child_type, uint32_t **children);

static void print_properties(struct spa_dict *props, char mark, bool header)
{
	const struct spa_dict_item *item;

	if (header)
		printf("%c\tproperties:\n", mark);
	if (props == NULL || props->n_items == 0) {
		if (header)
			printf("\t\tnone\n");
		return;
	}

	spa_dict_for_each(item, props) {
		printf("%c\t\t%s = \"%s\"\n", mark, item->key, item->value);
	}
}

static void print_params(struct spa_param_info *params, uint32_t n_params, char mark, bool header)
{
	uint32_t i;

	if (header)
		printf("%c\tparams: (%u)\n", mark, n_params);
	if (params == NULL || n_params == 0) {
		if (header)
			printf("\t\tnone\n");
		return;
	}
	for (i = 0; i < n_params; i++) {
		const struct spa_type_info *type_info = spa_type_param;

		printf("%c\t  %d (%s) %c%c\n",
				params[i].user > 0 ? mark : ' ', params[i].id,
				spa_debug_type_find_name(type_info, params[i].id),
				params[i].flags & SPA_PARAM_INFO_READ ? 'r' : '-',
				params[i].flags & SPA_PARAM_INFO_WRITE ? 'w' : '-');
		params[i].user = 0;
	}
}

static bool do_not_implemented(struct data *data, const char *cmd, char *args, char **error)
{
        *error = spa_aprintf("Command \"%s\" not yet implemented", cmd);
	return false;
}

static bool do_help(struct data *data, const char *cmd, char *args, char **error);
static bool do_load_module(struct data *data, const char *cmd, char *args, char **error);
static bool do_list_objects(struct data *data, const char *cmd, char *args, char **error);
static bool do_connect(struct data *data, const char *cmd, char *args, char **error);
static bool do_disconnect(struct data *data, const char *cmd, char *args, char **error);
static bool do_list_remotes(struct data *data, const char *cmd, char *args, char **error);
static bool do_switch_remote(struct data *data, const char *cmd, char *args, char **error);
static bool do_info(struct data *data, const char *cmd, char *args, char **error);
static bool do_create_device(struct data *data, const char *cmd, char *args, char **error);
static bool do_create_node(struct data *data, const char *cmd, char *args, char **error);
static bool do_destroy(struct data *data, const char *cmd, char *args, char **error);
static bool do_create_link(struct data *data, const char *cmd, char *args, char **error);
static bool do_export_node(struct data *data, const char *cmd, char *args, char **error);
static bool do_enum_params(struct data *data, const char *cmd, char *args, char **error);
static bool do_set_param(struct data *data, const char *cmd, char *args, char **error);
static bool do_permissions(struct data *data, const char *cmd, char *args, char **error);
static bool do_get_permissions(struct data *data, const char *cmd, char *args, char **error);
static bool do_send_command(struct data *data, const char *cmd, char *args, char **error);
static bool do_quit(struct data *data, const char *cmd, char *args, char **error);

#define DUMP_NAMES "Core|Module|Device|Node|Port|Factory|Client|Link|Session|Endpoint|EndpointStream"

static const struct command command_list[] = {
	{ "help", "h", "Show this help", do_help },
	{ "load-module", "lm", "Load a module. <module-name> [<module-arguments>]", do_load_module },
	{ "unload-module", "um", "Unload a module. <module-var>", do_not_implemented },
	{ "connect", "con", "Connect to a remote. [<remote-name>]", do_connect },
	{ "disconnect", "dis", "Disconnect from a remote. [<remote-var>]", do_disconnect },
	{ "list-remotes", "lr", "List connected remotes.", do_list_remotes },
	{ "switch-remote", "sr", "Switch between current remotes. [<remote-var>]", do_switch_remote },
	{ "list-objects", "ls", "List objects or current remote. [<interface>]", do_list_objects },
	{ "info", "i", "Get info about an object. <object-id>|all", do_info },
	{ "create-device", "cd", "Create a device from a factory. <factory-name> [<properties>]", do_create_device },
	{ "create-node", "cn", "Create a node from a factory. <factory-name> [<properties>]", do_create_node },
	{ "destroy", "d", "Destroy a global object. <object-id>", do_destroy },
	{ "create-link", "cl", "Create a link between nodes. <node-id> <port-id> <node-id> <port-id> [<properties>]", do_create_link },
	{ "export-node", "en", "Export a local node to the current remote. <node-id> [remote-var]", do_export_node },
	{ "enum-params", "e", "Enumerate params of an object <object-id> <param-id>", do_enum_params },
	{ "set-param", "s", "Set param of an object <object-id> <param-id> <param-json>", do_set_param },
	{ "permissions", "sp", "Set permissions for a client <client-id> <object> <permission>", do_permissions },
	{ "get-permissions", "gp", "Get permissions of a client <client-id>", do_get_permissions },
	{ "send-command", "c", "Send a command <object-id>", do_send_command },
	{ "quit", "q", "Quit", do_quit },
};

static void program_quit(struct data *data)
{
	data->quit = true;
	pw_main_loop_quit(data->loop);
}

static bool do_quit(struct data *data, const char *cmd, char *args, char **error)
{
	program_quit(data);
	return true;
}

static bool do_help(struct data *data, const char *cmd, char *args, char **error)
{
	printf("Available commands:\n");
	SPA_FOR_EACH_ELEMENT_VAR(command_list, c) {
		char cmd[256];
		snprintf(cmd, sizeof(cmd), "%s | %s", c->name, c->alias);
		printf("\t%-20.20s\t%s\n", cmd, c->description);
	}
	return true;
}

static bool do_load_module(struct data *data, const char *cmd, char *args, char **error)
{
	struct pw_impl_module *module;
	char *a[2];
	int n;
	uint32_t id;

	n = pw_split_ip(args, WHITESPACE, 2, a);
	if (n < 1) {
		*error = spa_aprintf("%s <module-name> [<module-arguments>]", cmd);
		return false;
	}

	module = pw_context_load_module(data->context, a[0], n == 2 ? a[1] : NULL, NULL);
	if (module == NULL) {
		*error = spa_aprintf("Could not load module");
		return false;
	}

	id = pw_map_insert_new(&data->vars, module);
	if (data->interactive)
		printf("%d = @module:%d\n", id, pw_global_get_id(pw_impl_module_get_global(module)));

	return true;
}

static void on_core_info(void *_data, const struct pw_core_info *info)
{
	struct remote_data *rd = _data;
	free(rd->name);
	rd->name = info->name ? strdup(info->name) : NULL;
	if (rd->data->interactive)
		printf("remote %d is named '%s'\n", rd->id, rd->name);
}

static void set_prompt(struct remote_data *rd)
{
	snprintf(prompt, sizeof(prompt), "%s>> ", rd->name);
#ifdef HAVE_READLINE
	rl_set_prompt(prompt);
#else
	printf("%s", prompt);
	fflush(stdout);
#endif
}

static void on_core_done(void *_data, uint32_t id, int seq)
{
	struct remote_data *rd = _data;
	struct data *d = rd->data;

	if (seq == rd->prompt_pending) {
		if (d->interactive) {
			set_prompt(rd);
			rd->data->monitoring = true;
		} else {
			pw_main_loop_quit(d->loop);
		}
	}
}

static bool global_matches(struct global *g, const char *pattern)
{
	const char *str;

	if (g->properties == NULL)
		return false;

	if (strstr(g->type, pattern) != NULL)
		return true;
	if ((str = pw_properties_get(g->properties, PW_KEY_OBJECT_PATH)) != NULL &&
	    fnmatch(pattern, str, FNM_EXTMATCH) == 0)
		return true;
	if ((str = pw_properties_get(g->properties, PW_KEY_OBJECT_SERIAL)) != NULL &&
	    spa_streq(pattern, str))
		return true;
	if (g->class != NULL && g->class->name_key != NULL &&
	    (str = pw_properties_get(g->properties, g->class->name_key)) != NULL &&
	    fnmatch(pattern, str, FNM_EXTMATCH) == 0)
		return true;

	return false;
}

static int print_global(void *obj, void *data)
{
	struct global *global = obj;
	const char *filter = data;

	if (global == NULL)
		return 0;

	if (filter && !global_matches(global, filter))
		return 0;

	printf("\tid %d, type %s/%d\n", global->id,
					global->type, global->version);
	if (global->properties)
		print_properties(&global->properties->dict, ' ', false);

	return 0;
}


static bool bind_global(struct remote_data *rd, struct global *global, char **error);

static void registry_event_global(void *data, uint32_t id,
		uint32_t permissions, const char *type, uint32_t version,
		const struct spa_dict *props)
{
	struct remote_data *rd = data;
	struct global *global;
	size_t size;
	char *error;
	bool ret;

	global = calloc(1, sizeof(struct global));
	if (global == NULL) {
		fprintf(stderr, "Allocation failed: %m");
		return;
	}
	global->rd = rd;
	global->id = id;
	global->permissions = permissions;
	global->type = strdup(type);
	global->version = version;
	global->properties = props ? pw_properties_new_dict(props) : NULL;

	if (rd->data->monitoring) {
		printf("remote %d added global: ", rd->id);
		print_global(global, NULL);
	}

	size = pw_map_get_size(&rd->globals);
	while (id > size)
		pw_map_insert_at(&rd->globals, size++, NULL);
	pw_map_insert_at(&rd->globals, id, global);

	/* immediately bind the object always */
	ret = bind_global(rd, global, &error);
	if (!ret) {
		if (rd->data->interactive)
			fprintf(stderr, "Error: \"%s\"\n", error);
		free(error);
	}
}

static int destroy_global(void *obj, void *data)
{
	struct global *global = obj;

	if (global == NULL)
		return 0;

	if (global->proxy)
		pw_proxy_destroy(global->proxy);
	pw_map_insert_at(&global->rd->globals, global->id, NULL);
	pw_properties_free(global->properties);
	free(global->type);
	free(global);
	return 0;
}

static void registry_event_global_remove(void *data, uint32_t id)
{
	struct remote_data *rd = data;
	struct global *global;

	global = pw_map_lookup(&rd->globals, id);
	if (global == NULL) {
		fprintf(stderr, "remote %d removed unknown global %d\n", rd->id, id);
		return;
	}

	if (rd->data->monitoring) {
		printf("remote %d removed global: ", rd->id);
		print_global(global, NULL);
	}

	destroy_global(global, rd);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static struct global *find_global(struct remote_data *rd, const char *pattern)
{
	uint32_t id;
	union pw_map_item *item;

	if (spa_atou32(pattern, &id, 0))
		return pw_map_lookup(&rd->globals, id);

	pw_array_for_each(item, &rd->globals.items) {
		struct global *g = item->data;
		if (pw_map_item_is_free(item) || g == NULL)
			continue;
		if (global_matches(g, pattern))
			return g;
        }
	return NULL;
}

static void on_core_error(void *_data, uint32_t id, int seq, int res, const char *message)
{
	struct remote_data *rd = _data;
	struct data *data = rd->data;

	pw_log_error("remote %p: error id:%u seq:%d res:%d (%s): %s", rd,
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		program_quit(data);
}

static const struct pw_core_events remote_core_events = {
	PW_VERSION_CORE_EVENTS,
	.info = on_core_info,
	.done = on_core_done,
	.error = on_core_error,
};

static void on_core_destroy(void *_data)
{
	struct remote_data *rd = _data;
	struct data *data = rd->data;

	spa_list_remove(&rd->link);

	spa_hook_remove(&rd->core_listener);
	spa_hook_remove(&rd->proxy_core_listener);

	pw_map_remove(&data->vars, rd->id);
	pw_map_for_each(&rd->globals, destroy_global, rd);
	pw_map_clear(&rd->globals);

	if (data->current == rd)
		data->current = NULL;
	free(rd->name);
}

static const struct pw_proxy_events proxy_core_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = on_core_destroy,
};

static void remote_data_free(struct remote_data *rd)
{
	spa_hook_remove(&rd->registry_listener);
	pw_proxy_destroy((struct pw_proxy*)rd->registry);
	pw_core_disconnect(rd->core);
}

static bool do_connect(struct data *data, const char *cmd, char *args, char **error)
{
	char *a[1];
        int n;
	struct pw_properties *props = NULL;
	struct pw_core *core;
	struct remote_data *rd;

	n = args ? pw_split_ip(args, WHITESPACE, 1, a) : 0;
	if (n == 1) {
		props = pw_properties_new(PW_KEY_REMOTE_NAME, a[0], NULL);
	}
	core = pw_context_connect(data->context, props, sizeof(struct remote_data));
	if (core == NULL) {
		*error = spa_aprintf("failed to connect: %m");
		return false;
	}

	rd = pw_proxy_get_user_data((struct pw_proxy*)core);
	rd->core = core;
	rd->data = data;
	pw_map_init(&rd->globals, 64, 16);
	rd->id = pw_map_insert_new(&data->vars, rd);
	spa_list_append(&data->remotes, &rd->link);

	if (rd->data->interactive)
		printf("%d = @remote:%p\n", rd->id, rd->core);

	data->current = rd;

	pw_core_add_listener(rd->core,
				   &rd->core_listener,
				   &remote_core_events, rd);
	pw_proxy_add_listener((struct pw_proxy*)rd->core,
			&rd->proxy_core_listener,
			&proxy_core_events, rd);
	rd->registry = pw_core_get_registry(rd->core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(rd->registry,
				       &rd->registry_listener,
				       &registry_events, rd);
	rd->prompt_pending = pw_core_sync(rd->core, 0, 0);

	return true;
}

static bool do_disconnect(struct data *data, const char *cmd, char *args, char **error)
{
	char *a[1];
        int n;
	uint32_t idx;
	struct remote_data *rd = data->current;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n >= 1) {
		idx = atoi(a[0]);
		rd = pw_map_lookup(&data->vars, idx);
		if (rd == NULL)
			goto no_remote;

	}
	if (rd)
		remote_data_free(rd);

	if (data->current == NULL) {
		if (spa_list_is_empty(&data->remotes)) {
			return true;
		}
		data->current = spa_list_last(&data->remotes, struct remote_data, link);
	}

	return true;

      no_remote:
        *error = spa_aprintf("Remote %d does not exist", idx);
	return false;
}

static bool do_list_remotes(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd;

	spa_list_for_each(rd, &data->remotes, link)
		printf("\t%d = @remote:%p '%s'\n", rd->id, rd->core, rd->name);

	return true;
}

static bool do_switch_remote(struct data *data, const char *cmd, char *args, char **error)
{
	char *a[1];
        int n, idx = 0;
	struct remote_data *rd;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n == 1)
		idx = atoi(a[0]);

	rd = pw_map_lookup(&data->vars, idx);
	if (rd == NULL)
		goto no_remote;

	spa_list_remove(&rd->link);
	spa_list_append(&data->remotes, &rd->link);
	data->current = rd;

	return true;

      no_remote:
        *error = spa_aprintf("Remote %d does not exist", idx);
	return false;
}

#define MARK_CHANGE(f) ((((info)->change_mask & (f))) ? '*' : ' ')

static void info_global(struct proxy_data *pd)
{
	struct global *global = pd->global;

	if (global == NULL)
		return;

	printf("\tid: %d\n", global->id);
	printf("\tpermissions: "PW_PERMISSION_FORMAT"\n",
			PW_PERMISSION_ARGS(global->permissions));
	printf("\ttype: %s/%d\n", global->type, global->version);
}

static void info_core(struct proxy_data *pd)
{
	struct pw_core_info *info = pd->info;

	info_global(pd);
	if (info == NULL)
		return;
	printf("\tcookie: %u\n", info->cookie);
	printf("\tuser-name: \"%s\"\n", info->user_name);
	printf("\thost-name: \"%s\"\n", info->host_name);
	printf("\tversion: \"%s\"\n", info->version);
	printf("\tname: \"%s\"\n", info->name);
	print_properties(info->props, MARK_CHANGE(PW_CORE_CHANGE_MASK_PROPS), true);
	info->change_mask = 0;
}

static void info_module(struct proxy_data *pd)
{
	struct pw_module_info *info = pd->info;

	info_global(pd);
	if (info == NULL)
		return;
	printf("\tname: \"%s\"\n", info->name);
	printf("\tfilename: \"%s\"\n", info->filename);
	printf("\targs: \"%s\"\n", info->args);
	print_properties(info->props, MARK_CHANGE(PW_MODULE_CHANGE_MASK_PROPS), true);
	info->change_mask = 0;
}

static void info_node(struct proxy_data *pd)
{
	struct pw_node_info *info = pd->info;

	info_global(pd);
	if (info == NULL)
		return;
	printf("%c\tinput ports: %u/%u\n", MARK_CHANGE(PW_NODE_CHANGE_MASK_INPUT_PORTS),
			info->n_input_ports, info->max_input_ports);
	printf("%c\toutput ports: %u/%u\n", MARK_CHANGE(PW_NODE_CHANGE_MASK_OUTPUT_PORTS),
			info->n_output_ports, info->max_output_ports);
	printf("%c\tstate: \"%s\"", MARK_CHANGE(PW_NODE_CHANGE_MASK_STATE),
			pw_node_state_as_string(info->state));
	if (info->state == PW_NODE_STATE_ERROR && info->error)
		printf(" \"%s\"\n", info->error);
	else
		printf("\n");
	print_properties(info->props, MARK_CHANGE(PW_NODE_CHANGE_MASK_PROPS), true);
	print_params(info->params, info->n_params, MARK_CHANGE(PW_NODE_CHANGE_MASK_PARAMS), true);
	info->change_mask = 0;
}

static void info_port(struct proxy_data *pd)
{
	struct pw_port_info *info = pd->info;

	info_global(pd);
	if (info == NULL)
		return;
	printf("\tdirection: \"%s\"\n", pw_direction_as_string(info->direction));
	print_properties(info->props, MARK_CHANGE(PW_PORT_CHANGE_MASK_PROPS), true);
	print_params(info->params, info->n_params, MARK_CHANGE(PW_PORT_CHANGE_MASK_PARAMS), true);
	info->change_mask = 0;
}

static void info_factory(struct proxy_data *pd)
{
	struct pw_factory_info *info = pd->info;

	info_global(pd);
	if (info == NULL)
		return;
	printf("\tname: \"%s\"\n", info->name);
	printf("\tobject-type: %s/%d\n", info->type, info->version);
	print_properties(info->props, MARK_CHANGE(PW_FACTORY_CHANGE_MASK_PROPS), true);
	info->change_mask = 0;
}

static void info_client(struct proxy_data *pd)
{
	struct pw_client_info *info = pd->info;

	info_global(pd);
	if (info == NULL)
		return;
	print_properties(info->props, MARK_CHANGE(PW_CLIENT_CHANGE_MASK_PROPS), true);
	info->change_mask = 0;
}

static void info_link(struct proxy_data *pd)
{
	struct pw_link_info *info = pd->info;

	info_global(pd);
	if (info == NULL)
		return;
	printf("\toutput-node-id: %u\n", info->output_node_id);
	printf("\toutput-port-id: %u\n", info->output_port_id);
	printf("\tinput-node-id: %u\n", info->input_node_id);
	printf("\tinput-port-id: %u\n", info->input_port_id);

	printf("%c\tstate: \"%s\"", MARK_CHANGE(PW_LINK_CHANGE_MASK_STATE),
			pw_link_state_as_string(info->state));
	if (info->state == PW_LINK_STATE_ERROR && info->error)
		printf(" \"%s\"\n", info->error);
	else
		printf("\n");
	printf("%c\tformat:\n", MARK_CHANGE(PW_LINK_CHANGE_MASK_FORMAT));
	if (info->format)
		spa_debug_pod(2, NULL, info->format);
	else
		printf("\t\tnone\n");
	print_properties(info->props, MARK_CHANGE(PW_LINK_CHANGE_MASK_PROPS), true);
	info->change_mask = 0;
}

static void info_device(struct proxy_data *pd)
{
	struct pw_device_info *info = pd->info;

	info_global(pd);
	if (info == NULL)
		return;
	print_properties(info->props, MARK_CHANGE(PW_DEVICE_CHANGE_MASK_PROPS), true);
	print_params(info->params, info->n_params, MARK_CHANGE(PW_DEVICE_CHANGE_MASK_PARAMS), true);
	info->change_mask = 0;
}

static void info_session(struct proxy_data *pd)
{
	struct pw_session_info *info = pd->info;

	info_global(pd);
	if (info == NULL)
		return;
	print_properties(info->props, MARK_CHANGE(0), true);
	print_params(info->params, info->n_params, MARK_CHANGE(1), true);
	info->change_mask = 0;
}

static void info_endpoint(struct proxy_data *pd)
{
	struct pw_endpoint_info *info = pd->info;
	const char *direction;

	info_global(pd);
	if (info == NULL)
		return;
	printf("\tname: %s\n", info->name);
	printf("\tmedia-class: %s\n",  info->media_class);
	switch(info->direction) {
	case PW_DIRECTION_OUTPUT:
		direction = "source";
		break;
	case PW_DIRECTION_INPUT:
		direction = "sink";
		break;
	default:
		direction = "invalid";
		break;
	}
	printf("\tdirection: %s\n", direction);
	printf("\tflags: 0x%x\n", info->flags);
	printf("%c\tstreams: %u\n", MARK_CHANGE(0), info->n_streams);
	printf("%c\tsession: %u\n", MARK_CHANGE(1), info->session_id);
	print_properties(info->props, MARK_CHANGE(2), true);
	print_params(info->params, info->n_params, MARK_CHANGE(3), true);
	info->change_mask = 0;
}

static void info_endpoint_stream(struct proxy_data *pd)
{
	struct pw_endpoint_stream_info *info = pd->info;

	info_global(pd);
	if (info == NULL)
		return;
	printf("\tid: %u\n", info->id);
	printf("\tendpoint-id: %u\n", info->endpoint_id);
	printf("\tname: %s\n", info->name);
	print_properties(info->props, MARK_CHANGE(1), true);
	print_params(info->params, info->n_params, MARK_CHANGE(2), true);
	info->change_mask = 0;
}

static void core_event_info(void *data, const struct pw_core_info *info)
{
	struct proxy_data *pd = data;
	struct remote_data *rd = pd->rd;
	if (pd->info && rd->data->monitoring)
		printf("remote %d core %d changed\n", rd->id, info->id);
	pd->info = pw_core_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_core(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.info = core_event_info
};


static void module_event_info(void *data, const struct pw_module_info *info)
{
	struct proxy_data *pd = data;
	struct remote_data *rd = pd->rd;
	if (pd->info && rd->data->monitoring)
		printf("remote %d module %d changed\n", rd->id, info->id);
	pd->info = pw_module_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_module(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.info = module_event_info
};

static void node_event_info(void *data, const struct pw_node_info *info)
{
	struct proxy_data *pd = data;
	struct remote_data *rd = pd->rd;
	if (pd->info && rd->data->monitoring)
		printf("remote %d node %d changed\n", rd->id, info->id);
	pd->info = pw_node_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_node(pd);
		pd->global->info_pending = false;
	}
}

static void event_param(void *_data, int seq, uint32_t id,
		uint32_t index, uint32_t next, const struct spa_pod *param)
{
        struct proxy_data *data = _data;
	struct remote_data *rd = data->rd;

	if (rd->data->interactive)
		printf("remote %d object %d param %d index %d\n",
				rd->id, data->global->id, id, index);

	spa_debug_pod(2, NULL, param);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_event_info,
	.param = event_param
};


static void port_event_info(void *data, const struct pw_port_info *info)
{
	struct proxy_data *pd = data;
	struct remote_data *rd = pd->rd;
	if (pd->info && rd->data->monitoring)
		printf("remote %d port %d changed\n", rd->id, info->id);
	pd->info = pw_port_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_port(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_port_events port_events = {
	PW_VERSION_PORT_EVENTS,
	.info = port_event_info,
	.param = event_param
};

static void factory_event_info(void *data, const struct pw_factory_info *info)
{
	struct proxy_data *pd = data;
	struct remote_data *rd = pd->rd;
	if (pd->info && rd->data->monitoring)
		printf("remote %d factory %d changed\n", rd->id, info->id);
	pd->info = pw_factory_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_factory(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_factory_events factory_events = {
	PW_VERSION_FACTORY_EVENTS,
	.info = factory_event_info
};

static void client_event_info(void *data, const struct pw_client_info *info)
{
	struct proxy_data *pd = data;
	struct remote_data *rd = pd->rd;
	if (pd->info && rd->data->monitoring)
		printf("remote %d client %d changed\n", rd->id, info->id);
	pd->info = pw_client_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_client(pd);
		pd->global->info_pending = false;
	}
}

static void client_event_permissions(void *_data, uint32_t index,
		uint32_t n_permissions, const struct pw_permission *permissions)
{
        struct proxy_data *data = _data;
	struct remote_data *rd = data->rd;
	uint32_t i;

	printf("remote %d node %d index %d\n",
			rd->id, data->global->id, index);

	for (i = 0; i < n_permissions; i++) {
		if (permissions[i].id == PW_ID_ANY)
			printf("  default:");
		else
			printf("  %u:", permissions[i].id);
		printf(" "PW_PERMISSION_FORMAT"\n",
			PW_PERMISSION_ARGS(permissions[i].permissions));
	}
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.info = client_event_info,
	.permissions = client_event_permissions
};

static void link_event_info(void *data, const struct pw_link_info *info)
{
	struct proxy_data *pd = data;
	struct remote_data *rd = pd->rd;
	if (pd->info && rd->data->monitoring)
		printf("remote %d link %d changed\n", rd->id, info->id);
	pd->info = pw_link_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_link(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_link_events link_events = {
	PW_VERSION_LINK_EVENTS,
	.info = link_event_info
};


static void device_event_info(void *data, const struct pw_device_info *info)
{
	struct proxy_data *pd = data;
	struct remote_data *rd = pd->rd;
	if (pd->info && rd->data->monitoring)
		printf("remote %d device %d changed\n", rd->id, info->id);
	pd->info = pw_device_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_device(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.info = device_event_info,
	.param = event_param
};

static void session_info_free(struct pw_session_info *info)
{
	free(info->params);
	pw_properties_free ((struct pw_properties *)info->props);
	free(info);
}

static void session_event_info(void *data,
				const struct pw_session_info *update)
{
	struct proxy_data *pd = data;
	struct remote_data *rd = pd->rd;
	struct pw_session_info *info = pd->info;

	if (info == NULL)
		info = pd->info = calloc(1, sizeof(*info));
	if (info != NULL) {
		info->id = update->id;
		if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_PARAMS) {
			info->n_params = update->n_params;
			free(info->params);
			info->params = malloc(info->n_params * sizeof(struct spa_param_info));
			memcpy(info->params, update->params,
				info->n_params * sizeof(struct spa_param_info));
		}
		if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_PROPS) {
			pw_properties_free ((struct pw_properties *)info->props);
			info->props =
				(struct spa_dict *) pw_properties_new_dict (update->props);
		}
	}
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, update->id);
	if (pd->global && pd->global->info_pending) {
		info_session(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_session_events session_events = {
	PW_VERSION_SESSION_EVENTS,
	.info = session_event_info,
	.param = event_param
};

static void endpoint_info_free(struct pw_endpoint_info *info)
{
	free(info->name);
	free(info->media_class);
	free(info->params);
	pw_properties_free ((struct pw_properties *)info->props);
	free(info);
}

static void endpoint_event_info(void *data,
				const struct pw_endpoint_info *update)
{
	struct proxy_data *pd = data;
	struct remote_data *rd = pd->rd;
	struct pw_endpoint_info *info = pd->info;

	if (info == NULL)
		info = pd->info = calloc(1, sizeof(*info));
	if (info != NULL) {
		info->id = update->id;
		info->name = update->name ? strdup(update->name) : NULL;
		info->media_class = update->media_class ? strdup(update->media_class) : NULL;
		info->direction = update->direction;
		info->flags = update->flags;

		if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_STREAMS)
			info->n_streams = update->n_streams;
		if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_SESSION)
			info->session_id = update->session_id;
		if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_PARAMS) {
			info->n_params = update->n_params;
			free(info->params);
			info->params = malloc(info->n_params * sizeof(struct spa_param_info));
			memcpy(info->params, update->params,
				info->n_params * sizeof(struct spa_param_info));
		}
		if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_PROPS) {
			pw_properties_free ((struct pw_properties *)info->props);
			info->props =
				(struct spa_dict *) pw_properties_new_dict (update->props);
		}
	}
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, update->id);
	if (pd->global && pd->global->info_pending) {
		info_endpoint(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_endpoint_events endpoint_events = {
	PW_VERSION_ENDPOINT_EVENTS,
	.info = endpoint_event_info,
	.param = event_param
};

static void endpoint_stream_info_free(struct pw_endpoint_stream_info *info)
{
	free(info->name);
	free(info->params);
	pw_properties_free ((struct pw_properties *)info->props);
	free(info);
}

static void endpoint_stream_event_info(void *data,
				const struct pw_endpoint_stream_info *update)
{
	struct proxy_data *pd = data;
	struct remote_data *rd = pd->rd;
	struct pw_endpoint_stream_info *info = pd->info;

	if (info == NULL)
		info = pd->info = calloc(1, sizeof(*info));
	if (info != NULL) {
		info->id = update->id;
		info->endpoint_id = update->endpoint_id;
		info->name = update->name ? strdup(update->name) : NULL;

		if (update->change_mask & PW_ENDPOINT_STREAM_CHANGE_MASK_PARAMS) {
			info->n_params = update->n_params;
			free(info->params);
			info->params = malloc(info->n_params * sizeof(struct spa_param_info));
			memcpy(info->params, update->params,
				info->n_params * sizeof(struct spa_param_info));
		}
		if (update->change_mask & PW_ENDPOINT_STREAM_CHANGE_MASK_PROPS) {
			pw_properties_free ((struct pw_properties *)info->props);
			info->props =
				(struct spa_dict *) pw_properties_new_dict (update->props);
		}
	}
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, update->id);
	if (pd->global && pd->global->info_pending) {
		info_endpoint_stream(pd);
		pd->global->info_pending = false;
	}
}

static const struct pw_endpoint_stream_events endpoint_stream_events = {
	PW_VERSION_ENDPOINT_STREAM_EVENTS,
	.info = endpoint_stream_event_info,
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

	spa_hook_remove(&pd->proxy_listener);
	spa_hook_remove(&pd->object_listener);

	if (pd->global)
		pd->global->proxy = NULL;

	if (pd->info == NULL)
		return;

	if (pd->class->destroy)
		pd->class->destroy(pd->info);
	pd->info = NULL;
}

static const struct pw_proxy_events proxy_events = {
        PW_VERSION_PROXY_EVENTS,
        .removed = removed_proxy,
        .destroy = destroy_proxy,
};

static bool do_list_objects(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	pw_map_for_each(&rd->globals, print_global, args);
	return true;
}

static const struct class core_class = {
	.type = PW_TYPE_INTERFACE_Core,
	.version = PW_VERSION_CORE,
	.events = &core_events,
	.destroy = (pw_destroy_t) pw_core_info_free,
	.info = info_core,
	.name_key = PW_KEY_CORE_NAME,
};
static const struct class module_class = {
	.type = PW_TYPE_INTERFACE_Module,
	.version = PW_VERSION_MODULE,
	.events = &module_events,
	.destroy = (pw_destroy_t) pw_module_info_free,
	.info = info_module,
	.name_key = PW_KEY_MODULE_NAME,
};

static const struct class factory_class = {
	.type = PW_TYPE_INTERFACE_Factory,
	.version = PW_VERSION_FACTORY,
	.events = &factory_events,
	.destroy = (pw_destroy_t) pw_factory_info_free,
	.info = info_factory,
	.name_key = PW_KEY_FACTORY_NAME,
};

static const struct class client_class = {
	.type = PW_TYPE_INTERFACE_Client,
	.version = PW_VERSION_CLIENT,
	.events = &client_events,
	.destroy = (pw_destroy_t) pw_client_info_free,
	.info = info_client,
	.name_key = PW_KEY_APP_NAME,
};
static const struct class device_class = {
	.type = PW_TYPE_INTERFACE_Device,
	.version = PW_VERSION_DEVICE,
	.events = &device_events,
	.destroy = (pw_destroy_t) pw_device_info_free,
	.info = info_device,
	.name_key = PW_KEY_DEVICE_NAME,
};
static const struct class node_class = {
	.type = PW_TYPE_INTERFACE_Node,
	.version = PW_VERSION_NODE,
	.events = &node_events,
	.destroy = (pw_destroy_t) pw_node_info_free,
	.info = info_node,
	.name_key = PW_KEY_NODE_NAME,
};
static const struct class port_class = {
	.type = PW_TYPE_INTERFACE_Port,
	.version = PW_VERSION_PORT,
	.events = &port_events,
	.destroy = (pw_destroy_t) pw_port_info_free,
	.info = info_port,
	.name_key = PW_KEY_PORT_NAME,
};
static const struct class link_class = {
	.type = PW_TYPE_INTERFACE_Link,
	.version = PW_VERSION_LINK,
	.events = &link_events,
	.destroy = (pw_destroy_t) pw_link_info_free,
	.info = info_link,
};
static const struct class session_class = {
	.type = PW_TYPE_INTERFACE_Session,
	.version = PW_VERSION_SESSION,
	.events = &session_events,
	.destroy = (pw_destroy_t) session_info_free,
	.info = info_session,
};
static const struct class endpoint_class = {
	.type = PW_TYPE_INTERFACE_Endpoint,
	.version = PW_VERSION_ENDPOINT,
	.events = &endpoint_events,
	.destroy = (pw_destroy_t) endpoint_info_free,
	.info = info_endpoint,
};
static const struct class endpoint_stream_class = {
	.type = PW_TYPE_INTERFACE_EndpointStream,
	.version = PW_VERSION_ENDPOINT_STREAM,
	.events = &endpoint_stream_events,
	.destroy = (pw_destroy_t) endpoint_stream_info_free,
	.info = info_endpoint_stream,
};
static const struct class metadata_class = {
	.type = PW_TYPE_INTERFACE_Metadata,
	.version = PW_VERSION_METADATA,
	.name_key = PW_KEY_METADATA_NAME,
};

static const struct class *classes[] =
{
	&core_class,
	&module_class,
	&factory_class,
	&client_class,
	&device_class,
	&node_class,
	&port_class,
	&link_class,
	&session_class,
	&endpoint_class,
	&endpoint_stream_class,
	&metadata_class,
};

static const struct class *find_class(const char *type, uint32_t version)
{
	SPA_FOR_EACH_ELEMENT_VAR(classes, c) {
		if (spa_streq((*c)->type, type) &&
		    (*c)->version <= version)
			return *c;
	}
	return NULL;
}

static bool bind_global(struct remote_data *rd, struct global *global, char **error)
{
	const struct class *class;
	struct proxy_data *pd;
	struct pw_proxy *proxy;

	class = find_class(global->type, global->version);
	if (class == NULL) {
		*error = spa_aprintf("unsupported type %s", global->type);
		return false;
	}
	global->class = class;

	proxy = pw_registry_bind(rd->registry,
				       global->id,
				       global->type,
				       class->version,
				       sizeof(struct proxy_data));

	pd = pw_proxy_get_user_data(proxy);
	pd->rd = rd;
	pd->global = global;
	pd->proxy = proxy;
	pd->class = class;
	pw_proxy_add_object_listener(proxy, &pd->object_listener, class->events, pd);
	pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);

	global->proxy = proxy;

	rd->prompt_pending = pw_core_sync(rd->core, 0, 0);

	return true;
}

static bool do_global_info(struct global *global, char **error)
{
	struct remote_data *rd = global->rd;
	struct proxy_data *pd;

	if (global->proxy == NULL) {
		if (!bind_global(rd, global, error))
			return false;
		global->info_pending = true;
	} else {
		pd = pw_proxy_get_user_data(global->proxy);
		if (pd->class->info)
			pd->class->info(pd);
	}
	return true;
}
static int do_global_info_all(void *obj, void *data)
{
	struct global *global = obj;
	char *error;

	if (global == NULL)
		return 0;

	if (!do_global_info(global, &error)) {
		fprintf(stderr, "info: %s\n", error);
		free(error);
	}
	return 0;
}

static bool do_info(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[1];
        int n;
	struct global *global;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n < 1) {
		*error = spa_aprintf("%s <object-id>|all", cmd);
		return false;
	}
	if (spa_streq(a[0], "all")) {
		pw_map_for_each(&rd->globals, do_global_info_all, NULL);
	}
	else {
		global = find_global(rd, a[0]);
		if (global == NULL) {
			*error = spa_aprintf("%s: unknown global '%s'", cmd, a[0]);
			return false;
		}
		return do_global_info(global, error);
	}
	return true;
}

static bool do_create_device(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[2];
	int n;
	uint32_t id;
	struct pw_proxy *proxy;
	struct pw_properties *props = NULL;
	struct proxy_data *pd;

	n = pw_split_ip(args, WHITESPACE, 2, a);
	if (n < 1) {
		*error = spa_aprintf("%s <factory-name> [<properties>]", cmd);
		return false;
	}
	if (n == 2)
		props = pw_properties_new_string(a[1]);

	proxy = pw_core_create_object(rd->core, a[0],
					    PW_TYPE_INTERFACE_Device,
					    PW_VERSION_DEVICE,
					    props ? &props->dict : NULL,
					    sizeof(struct proxy_data));

	pw_properties_free(props);

	pd = pw_proxy_get_user_data(proxy);
	pd->rd = rd;
	pd->proxy = proxy;
	pd->class = &device_class;
	pw_proxy_add_object_listener(proxy, &pd->object_listener, &device_events, pd);
	pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);

	id = pw_map_insert_new(&data->vars, proxy);
	if (rd->data->interactive)
		printf("%d = @proxy:%d\n", id, pw_proxy_get_id(proxy));

	return true;
}

static bool do_create_node(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[2];
        int n;
	uint32_t id;
	struct pw_proxy *proxy;
	struct pw_properties *props = NULL;
	struct proxy_data *pd;

	n = pw_split_ip(args, WHITESPACE, 2, a);
	if (n < 1) {
		*error = spa_aprintf("%s <factory-name> [<properties>]", cmd);
		return false;
	}
	if (n == 2)
		props = pw_properties_new_string(a[1]);

	proxy = pw_core_create_object(rd->core, a[0],
					    PW_TYPE_INTERFACE_Node,
					    PW_VERSION_NODE,
					    props ? &props->dict : NULL,
					    sizeof(struct proxy_data));

	pw_properties_free(props);

	pd = pw_proxy_get_user_data(proxy);
	pd->rd = rd;
	pd->proxy = proxy;
        pd->class = &node_class;
        pw_proxy_add_object_listener(proxy, &pd->object_listener, &node_events, pd);
        pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);

	id = pw_map_insert_new(&data->vars, proxy);
	if (rd->data->interactive)
		printf("%d = @proxy:%d\n", id, pw_proxy_get_id(proxy));

	return true;
}

static bool do_destroy(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[1];
        int n;
	struct global *global;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n < 1) {
		*error = spa_aprintf("%s <object-id>", cmd);
		return false;
	}
	global = find_global(rd, a[0]);
	if (global == NULL) {
		*error = spa_aprintf("%s: unknown global '%s'", cmd, a[0]);
		return false;
	}
	pw_registry_destroy(rd->registry, global->id);

	return true;
}

static struct global *
obj_global_port(struct remote_data *rd, struct global *global, const char *port_direction, const char *port_id)
{
	struct global *global_port_found = NULL;
	uint32_t *ports = NULL;
	int port_count;

	port_count = children_of(rd, global->id, PW_TYPE_INTERFACE_Port, &ports);

	if (port_count <= 0)
		return NULL;

	for (int i = 0; i < port_count; i++) {
		struct global *global_port = obj_global(rd, ports[i]);

		if (!global_port)
			continue;

		struct spa_dict *props_port = global_props(global_port);

		if (spa_streq(spa_dict_lookup(props_port, "port.direction"), port_direction)
				&& spa_streq(spa_dict_lookup(props_port, "port.id"), port_id)) {
			global_port_found = global_port;
			break;
		}
	}

	free(ports);
	return global_port_found;
}

static void create_link_with_properties(struct data *data, const struct pw_properties *props)
{
	struct remote_data *rd = data->current;
	uint32_t id;
	struct pw_proxy *proxy;
	struct proxy_data *pd;

	proxy = (struct pw_proxy*)pw_core_create_object(rd->core,
					  "link-factory",
					  PW_TYPE_INTERFACE_Link,
					  PW_VERSION_LINK,
					  props ? &props->dict : NULL,
					  sizeof(struct proxy_data));

	pd = pw_proxy_get_user_data(proxy);
	pd->rd = rd;
	pd->proxy = proxy;
	pd->class = &link_class;
	pw_proxy_add_object_listener(proxy, &pd->object_listener, &link_events, pd);
	pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);

	id = pw_map_insert_new(&data->vars, proxy);
	if (rd->data->interactive)
		printf("%d = @proxy:%d\n", id, pw_proxy_get_id((struct pw_proxy*)proxy));
}

static bool do_create_link(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[5];
	int n;
	struct pw_properties *props = NULL;
	bool res = false;

	n = pw_split_ip(args, WHITESPACE, 5, a);
	if (n < 4) {
		*error = spa_aprintf("%s <node-id> <port> <node-id> <port> [<properties>]", cmd);
		return false;
	}
	if (n == 5)
		props = pw_properties_new_string(a[4]);
	else
		props = pw_properties_new(NULL, NULL);

	if (!spa_streq(a[0], "-"))
		pw_properties_set(props, PW_KEY_LINK_OUTPUT_NODE, a[0]);
	if (!spa_streq(a[1], "-"))
		pw_properties_set(props, PW_KEY_LINK_OUTPUT_PORT, a[1]);
	if (!spa_streq(a[2], "-"))
		pw_properties_set(props, PW_KEY_LINK_INPUT_NODE, a[2]);
	if (!spa_streq(a[3], "-"))
		pw_properties_set(props, PW_KEY_LINK_INPUT_PORT, a[3]);

	if (spa_streq(a[1], "*") && spa_streq(a[3], "*")) {
		struct global *global_out, *global_in;
		struct proxy_data *pd_out, *pd_in;
		uint32_t n_output_ports, n_input_ports;

		global_out = find_global(rd, a[0]);
		if (global_out == NULL) {
			*error = spa_aprintf("%s: unknown global '%s'", cmd, a[0]);
			goto done;
		}
		global_in = find_global(rd, a[2]);
		if (global_in == NULL) {
			*error = spa_aprintf("%s: unknown global '%s'", cmd, a[2]);
			goto done;
		}

		pd_out = pw_proxy_get_user_data(global_out->proxy);
		pd_in = pw_proxy_get_user_data(global_in->proxy);

		n_output_ports = ((struct pw_node_info *)pd_out->info)->n_output_ports;
		n_input_ports = ((struct pw_node_info *)pd_in->info)->n_input_ports;

		if (n_output_ports != n_input_ports) {
			*error = spa_aprintf("%s: Number of ports don't match (%u != %u)", cmd, n_output_ports, n_input_ports);
			goto done;
		}

		for (uint32_t i = 0; i < n_output_ports; i++) {
			char port_id[4];
			struct global *global_port_out, *global_port_in;

			snprintf(port_id, 4, "%d", i);

			global_port_out = obj_global_port(rd, global_out, "out", port_id);
			global_port_in = obj_global_port(rd, global_in, "in", port_id);

			if (!global_port_out || !global_port_in)
				continue;

			pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%d", global_port_out->id);
			pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%d", global_port_in->id);

			create_link_with_properties(data, props);
		}
	} else
		create_link_with_properties(data, props);

	res = true;

done:
	pw_properties_free(props);

	return res;
}

static bool do_export_node(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	struct pw_global *global;
	struct pw_node *node;
	struct pw_proxy *proxy;
	char *a[2];
	int n, idx;
	uint32_t id;

	n = pw_split_ip(args, WHITESPACE, 2, a);
	if (n < 1) {
		*error = spa_aprintf("%s <node-id> [<remote-var>]", cmd);
		return false;
	}
	if (n == 2) {
		idx = atoi(a[1]);
		rd = pw_map_lookup(&data->vars, idx);
		if (rd == NULL)
			goto no_remote;
	}

	global = pw_context_find_global(data->context, atoi(a[0]));
	if (global == NULL) {
		*error = spa_aprintf("object %d does not exist", atoi(a[0]));
		return false;
	}
	if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Node)) {
		*error = spa_aprintf("object %d is not a node", atoi(a[0]));
		return false;
	}
	node = pw_global_get_object(global);
	proxy = pw_core_export(rd->core, PW_TYPE_INTERFACE_Node, NULL, node, 0);

	id = pw_map_insert_new(&data->vars, proxy);
	if (rd->data->interactive)
		printf("%d = @proxy:%d\n", id, pw_proxy_get_id((struct pw_proxy*)proxy));

	return true;

      no_remote:
        *error = spa_aprintf("Remote %d does not exist", idx);
	return false;
}

static bool do_enum_params(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[2];
	int n;
	uint32_t param_id;
	const struct spa_type_info *ti;
	struct global *global;

	n = pw_split_ip(args, WHITESPACE, 2, a);
	if (n < 2) {
		*error = spa_aprintf("%s <object-id> <param-id>", cmd);
		return false;
	}

	ti = spa_debug_type_find_short(spa_type_param, a[1]);
	if (ti == NULL) {
		*error = spa_aprintf("%s: unknown param type: %s", cmd, a[1]);
		return false;
	}
	param_id = ti->type;

	global = find_global(rd, a[0]);
	if (global == NULL) {
		*error = spa_aprintf("%s: unknown global '%s'", cmd, a[0]);
		return false;
	}
	if (global->proxy == NULL) {
		if (!bind_global(rd, global, error))
			return false;
	}

	if (spa_streq(global->type, PW_TYPE_INTERFACE_Node))
		pw_node_enum_params((struct pw_node*)global->proxy, 0,
			param_id, 0, 0, NULL);
	else if (spa_streq(global->type, PW_TYPE_INTERFACE_Port))
		pw_port_enum_params((struct pw_port*)global->proxy, 0,
			param_id, 0, 0, NULL);
	else if (spa_streq(global->type, PW_TYPE_INTERFACE_Device))
		pw_device_enum_params((struct pw_device*)global->proxy, 0,
			param_id, 0, 0, NULL);
	else if (spa_streq(global->type, PW_TYPE_INTERFACE_Endpoint))
		pw_endpoint_enum_params((struct pw_endpoint*)global->proxy, 0,
			param_id, 0, 0, NULL);
	else {
		*error = spa_aprintf("enum-params not implemented on object %d type:%s",
				atoi(a[0]), global->type);
		return false;
	}
	return true;
}

static bool do_set_param(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[3];
	int res, n;
	uint32_t param_id;
	struct global *global;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_type_info *ti;
	struct spa_pod *pod;

	n = pw_split_ip(args, WHITESPACE, 3, a);
	if (n < 3) {
		*error = spa_aprintf("%s <object-id> <param-id> <param-json>", cmd);
		return false;
	}

	global = find_global(rd, a[0]);
	if (global == NULL) {
		*error = spa_aprintf("%s: unknown global '%s'", cmd, a[0]);
		return false;
	}
	if (global->proxy == NULL) {
		if (!bind_global(rd, global, error))
			return false;
	}

	ti = spa_debug_type_find_short(spa_type_param, a[1]);
	if (ti == NULL) {
		*error = spa_aprintf("%s: unknown param type: %s", cmd, a[1]);
		return false;
	}
	if ((res = spa_json_to_pod(&b, 0, ti, a[2], strlen(a[2]))) < 0) {
		*error = spa_aprintf("%s: can't make pod: %s", cmd, spa_strerror(res));
		return false;
	}
	if ((pod = spa_pod_builder_deref(&b, 0)) == NULL) {
		*error = spa_aprintf("%s: can't make pod", cmd);
		return false;
	}
	spa_debug_pod(0, NULL, pod);

	param_id = ti->type;

	if (spa_streq(global->type, PW_TYPE_INTERFACE_Node))
		pw_node_set_param((struct pw_node*)global->proxy,
				param_id, 0, pod);
	else if (spa_streq(global->type, PW_TYPE_INTERFACE_Device))
		pw_device_set_param((struct pw_device*)global->proxy,
				param_id, 0, pod);
	else if (spa_streq(global->type, PW_TYPE_INTERFACE_Endpoint))
		pw_endpoint_set_param((struct pw_endpoint*)global->proxy,
				param_id, 0, pod);
	else {
		*error = spa_aprintf("set-param not implemented on object %d type:%s",
				atoi(a[0]), global->type);
		return false;
	}
	return true;
}

static bool do_permissions(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[3];
	int n;
	uint32_t p;
	struct global *global;
	struct pw_permission permissions[1];

	n = pw_split_ip(args, WHITESPACE, 3, a);
	if (n < 3) {
		*error = spa_aprintf("%s <client-id> <object> <permission>", cmd);
		return false;
	}

	global = find_global(rd, a[0]);
	if (global == NULL) {
		*error = spa_aprintf("%s: unknown global '%s'", cmd, a[0]);
		return false;
	}
	if (!spa_streq(global->type, PW_TYPE_INTERFACE_Client)) {
		*error = spa_aprintf("object %d is not a client", atoi(a[0]));
		return false;
	}
	if (global->proxy == NULL) {
		if (!bind_global(rd, global, error))
			return false;
	}

	p = strtol(a[2], NULL, 0);
	if (rd->data->interactive)
		printf("setting permissions: "PW_PERMISSION_FORMAT"\n",
				PW_PERMISSION_ARGS(p));

	permissions[0] = PW_PERMISSION_INIT(atoi(a[1]), p);
	pw_client_update_permissions((struct pw_client*)global->proxy,
			1, permissions);

	return true;
}

static bool do_get_permissions(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[3];
        int n;
	struct global *global;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n < 1) {
		*error = spa_aprintf("%s <client-id>", cmd);
		return false;
	}

	global = find_global(rd, a[0]);
	if (global == NULL) {
		*error = spa_aprintf("%s: unknown global '%s'", cmd, a[0]);
		return false;
	}
	if (!spa_streq(global->type, PW_TYPE_INTERFACE_Client)) {
		*error = spa_aprintf("object %d is not a client", atoi(a[0]));
		return false;
	}
	if (global->proxy == NULL) {
		if (!bind_global(rd, global, error))
			return false;
	}
	pw_client_get_permissions((struct pw_client*)global->proxy,
			0, UINT32_MAX);

	return true;
}

static bool do_send_command(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[3];
	int res, n;
	struct global *global;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_type_info *ti;
	struct spa_pod *pod;

	n = pw_split_ip(args, WHITESPACE, 3, a);
	if (n < 3) {
		*error = spa_aprintf("%s <object-id> <command-id> <command-json>", cmd);
		return false;
	}

	global = find_global(rd, a[0]);
	if (global == NULL) {
		*error = spa_aprintf("%s: unknown global '%s'", cmd, a[0]);
		return false;
	}
	if (global->proxy == NULL) {
		if (!bind_global(rd, global, error))
			return false;
	}

	if (spa_streq(global->type, PW_TYPE_INTERFACE_Node)) {
		ti = spa_debug_type_find_short(spa_type_node_command_id, a[1]);
	} else {
		*error = spa_aprintf("send-command not implemented on object %d type:%s",
				atoi(a[0]), global->type);
		return false;
	}

	if (ti == NULL) {
		*error = spa_aprintf("%s: unknown node command type: %s", cmd, a[1]);
		return false;
	}
	if ((res = spa_json_to_pod(&b, 0, ti, a[2], strlen(a[2]))) < 0) {
		*error = spa_aprintf("%s: can't make pod: %s", cmd, spa_strerror(res));
		return false;
	}
	if ((pod = spa_pod_builder_deref(&b, 0)) == NULL) {
		*error = spa_aprintf("%s: can't make pod", cmd);
		return false;
	}
	spa_debug_pod(0, NULL, pod);

	pw_node_send_command((struct pw_node*)global->proxy, (struct spa_command*)pod);
	return true;
}

static struct global *
obj_global(struct remote_data *rd, uint32_t id)
{
	struct global *global;
	struct proxy_data *pd;

	if (!rd)
		return NULL;

	global = pw_map_lookup(&rd->globals, id);
	if (!global)
		return NULL;

	pd = pw_proxy_get_user_data(global->proxy);
	if (!pd || !pd->info)
		return NULL;

	return global;
}

static struct spa_dict *
global_props(struct global *global)
{
	struct proxy_data *pd;

	if (!global)
		return NULL;

	pd = pw_proxy_get_user_data(global->proxy);
	if (!pd || !pd->info)
		return NULL;

	if (spa_streq(global->type, PW_TYPE_INTERFACE_Core))
		return ((struct pw_core_info *)pd->info)->props;
	if (spa_streq(global->type, PW_TYPE_INTERFACE_Module))
		return ((struct pw_module_info *)pd->info)->props;
	if (spa_streq(global->type, PW_TYPE_INTERFACE_Device))
		return ((struct pw_device_info *)pd->info)->props;
	if (spa_streq(global->type, PW_TYPE_INTERFACE_Node))
		return ((struct pw_node_info *)pd->info)->props;
	if (spa_streq(global->type, PW_TYPE_INTERFACE_Port))
		return ((struct pw_port_info *)pd->info)->props;
	if (spa_streq(global->type, PW_TYPE_INTERFACE_Factory))
		return ((struct pw_factory_info *)pd->info)->props;
	if (spa_streq(global->type, PW_TYPE_INTERFACE_Client))
		return ((struct pw_client_info *)pd->info)->props;
	if (spa_streq(global->type, PW_TYPE_INTERFACE_Link))
		return ((struct pw_link_info *)pd->info)->props;
	if (spa_streq(global->type, PW_TYPE_INTERFACE_Session))
		return ((struct pw_session_info *)pd->info)->props;
	if (spa_streq(global->type, PW_TYPE_INTERFACE_Endpoint))
		return ((struct pw_endpoint_info *)pd->info)->props;
	if (spa_streq(global->type, PW_TYPE_INTERFACE_EndpointStream))
		return ((struct pw_endpoint_stream_info *)pd->info)->props;

	return NULL;
}

static const char *
global_lookup(struct global *global, const char *key)
{
	struct spa_dict *dict;

	dict = global_props(global);
	if (!dict)
		return NULL;
	return spa_dict_lookup(dict, key);
}


static int
children_of(struct remote_data *rd, uint32_t parent_id,
	    const char *child_type, uint32_t **children)
{
	const char *parent_type;
	union pw_map_item *item;
	struct global *global;
	struct proxy_data *pd;
	const char *parent_key = NULL, *child_key = NULL;
	const char *parent_value = NULL, *child_value = NULL;
	int pass, i, count;

	if (!rd || !children)
		return -1;

	/* get the device info */
	global = obj_global(rd, parent_id);
	if (!global)
		return -1;
	parent_type = global->type;
	pd = pw_proxy_get_user_data(global->proxy);
	if (!pd || !pd->info)
		return -1;

	/* supported combinations */
	if (spa_streq(parent_type, PW_TYPE_INTERFACE_Device) &&
	    spa_streq(child_type, PW_TYPE_INTERFACE_Node)) {
		parent_key = PW_KEY_OBJECT_ID;
		child_key = PW_KEY_DEVICE_ID;
	} else if (spa_streq(parent_type, PW_TYPE_INTERFACE_Node) &&
		   spa_streq(child_type, PW_TYPE_INTERFACE_Port)) {
		parent_key = PW_KEY_OBJECT_ID;
		child_key = PW_KEY_NODE_ID;
	} else if (spa_streq(parent_type, PW_TYPE_INTERFACE_Module) &&
		   spa_streq(child_type, PW_TYPE_INTERFACE_Factory)) {
		parent_key = PW_KEY_OBJECT_ID;
		child_key = PW_KEY_MODULE_ID;
	} else if (spa_streq(parent_type, PW_TYPE_INTERFACE_Factory) &&
		   spa_streq(child_type, PW_TYPE_INTERFACE_Device)) {
		parent_key = PW_KEY_OBJECT_ID;
		child_key = PW_KEY_FACTORY_ID;
	} else
		return -1;

	/* get the parent key value */
	if (parent_key) {
		parent_value = global_lookup(global, parent_key);
		if (!parent_value)
			return -1;
	}

	count = 0;
	*children = NULL;
	i = 0;
	for (pass = 1; pass <= 2; pass++) {
		if (pass == 2) {
			count = i;
			if (!count)
				return 0;

			*children = malloc(sizeof(uint32_t) * count);
			if (!*children)
				return -1;
		}
		i = 0;
		pw_array_for_each(item, &rd->globals.items) {
			if (pw_map_item_is_free(item) || item->data == NULL)
				continue;

			global = item->data;

			if (!spa_streq(global->type, child_type))
				continue;

			pd = pw_proxy_get_user_data(global->proxy);
			if (!pd || !pd->info)
				return -1;

			if (child_key) {
				/* get the device path */
				child_value = global_lookup(global, child_key);
				if (!child_value)
					continue;
			}

			/* match? */
			if (!spa_streq(parent_value, child_value))
				continue;

			if (*children)
				(*children)[i] = global->id;
			i++;

		}
	}
	return count;
}

#define INDENT(_level) \
	({ \
		int __level = (_level); \
		char *_indent = alloca(__level + 1); \
		memset(_indent, '\t', __level); \
		_indent[__level] = '\0'; \
		(const char *)_indent; \
	})

static bool parse(struct data *data, char *buf, char **error)
{
	char *a[2];
	int n;
	char *p, *cmd, *args;

	if ((p = strchr(buf, '#')))
		*p = '\0';

	p = pw_strip(buf, "\n\r \t");

	if (*p == '\0')
		return true;

	n = pw_split_ip(p, WHITESPACE, 2, a);
	if (n < 1)
		return true;

	cmd = a[0];
	args = n > 1 ? a[1] : "";

	SPA_FOR_EACH_ELEMENT_VAR(command_list, c) {
		if (spa_streq(c->name, cmd) ||
		    spa_streq(c->alias, cmd)) {
			return c->func(data, cmd, args, error);
		}
	}
        *error = spa_aprintf("Command \"%s\" does not exist. Type 'help' for usage.", cmd);
	return false;
}

/* We need a global variable, readline doesn't have a closure arg */
static struct data *input_dataptr;

static void input_process_line(char *line)
{
	struct data *d = input_dataptr;
	char *error;

	if (!line)
		line = strdup("quit");

	if (line[0] != '\0') {
#ifdef HAVE_READLINE
		add_history(line);
#endif
		if (!parse(d, line, &error)) {
			fprintf(stderr, "Error: \"%s\"\n", error);
			free(error);
		}
	}
	free(line);
}

static void do_input(void *data, int fd, uint32_t mask)
{
	struct data *d = data;

	if (mask & SPA_IO_IN) {
		input_dataptr = d;
#ifdef HAVE_READLINE
		rl_callback_read_char();
#else
		{
			char *line = NULL;
			size_t s = 0;

			if (getline(&line, &s, stdin) < 0) {
				free(line);
				line = NULL;
			}
			input_process_line(line);
		}
#endif

		if (d->current == NULL)
			pw_main_loop_quit(d->loop);
		else  {
			struct remote_data *rd = d->current;
			if (rd->core)
				rd->prompt_pending = pw_core_sync(rd->core, 0, 0);
		}
	}
}

#ifdef HAVE_READLINE
static char *
readline_match_command(const char *text, int state)
{
	static size_t idx;
	static int len;

	if (!state) {
		idx = 0;
		len = strlen(text);
	}

	while (idx < SPA_N_ELEMENTS(command_list))  {
		const char *name = command_list[idx].name;
		const char *alias = command_list[idx].alias;

		idx++;
		if (spa_strneq(name, text, len) || spa_strneq(alias, text, len))
			return strdup(name);
	}

	return NULL;
}

static char **
readline_command_completion(const char *text, int start, int end)
{
	char **matches = NULL;

	/* Only try to complete the first word in a line */
	if (start == 0)
		matches = rl_completion_matches(text, readline_match_command);

	/* Don't fall back to filename completion */
	rl_attempted_completion_over = true;

	return matches;
}

static void readline_init(void)
{
	rl_attempted_completion_function = readline_command_completion;
	rl_callback_handler_install(">> ", input_process_line);
}

static void readline_cleanup(void)
{
	rl_callback_handler_remove();
}
#endif

static void do_quit_on_signal(void *data, int signal_number)
{
	struct data *d = data;
	program_quit(d);
}

static void show_help(struct data *data, const char *name, bool error)
{
	fprintf(error ? stderr : stdout, _("%s [options] [command]\n"
		"  -h, --help                            Show this help\n"
		"      --version                         Show version\n"
		"  -d, --daemon                          Start as daemon (Default false)\n"
		"  -r, --remote                          Remote daemon name\n"
		"  -m, --monitor                         Monitor activity\n\n"),
		name);

	do_help(data, "help", "", NULL);
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct pw_loop *l;
	char *opt_remote = NULL;
	char *error;
	bool daemon = false, monitor = false;
	struct remote_data *rd;
	static const struct option long_options[] = {
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'V' },
		{ "monitor",	no_argument,		NULL, 'm' },
		{ "daemon",	no_argument,		NULL, 'd' },
		{ "remote",	required_argument,	NULL, 'r' },
		{ NULL,	0, NULL, 0}
	};
	int c, i;

	setlinebuf(stdout);

	setlocale(LC_ALL, "");
	pw_init(&argc, &argv);

	while ((c = getopt_long(argc, argv, "hVmdr:", long_options, NULL)) != -1) {
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
		case 'd':
			daemon = true;
			break;
		case 'm':
			monitor = true;
			break;
		case 'r':
			opt_remote = optarg;
			break;
		default:
			show_help(&data, argv[0], true);
			return -1;
		}
	}

	data.loop = pw_main_loop_new(NULL);
	if (data.loop == NULL) {
		fprintf(stderr, "Broken installation: %m\n");
		return -1;
	}
	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit_on_signal, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit_on_signal, &data);

	spa_list_init(&data.remotes);
	pw_map_init(&data.vars, 64, 16);

	data.context = pw_context_new(l,
			pw_properties_new(
				PW_KEY_CORE_DAEMON, daemon ? "true" : NULL,
				NULL),
			0);
	if (data.context == NULL) {
		fprintf(stderr, "Can't create context: %m\n");
		return -1;
	}

	pw_context_load_module(data.context, "libpipewire-module-link-factory", NULL, NULL);

	if (!do_connect(&data, "connect", opt_remote, &error)) {
		fprintf(stderr, "Error: \"%s\"\n", error);
		return -1;
	}

	if (optind == argc) {
		data.interactive = true;

		printf("Welcome to PipeWire version %s. Type 'help' for usage.\n",
				pw_get_library_version());

#ifdef HAVE_READLINE
		readline_init();
#endif

		pw_loop_add_io(l, STDIN_FILENO, SPA_IO_IN|SPA_IO_HUP, false, do_input, &data);

		pw_main_loop_run(data.loop);

#ifdef HAVE_READLINE
		readline_cleanup();
#endif
	} else {
		char buf[4096], *p, *error;

		p = buf;
		for (i = optind; i < argc; i++) {
			p = stpcpy(p, argv[i]);
			p = stpcpy(p, " ");
		}

		pw_main_loop_run(data.loop);

		if (!parse(&data, buf, &error)) {
			fprintf(stderr, "Error: \"%s\"\n", error);
			free(error);
		}
		if (data.current != NULL)
			data.current->prompt_pending = pw_core_sync(data.current->core, 0, 0);

		while (!data.quit && data.current != NULL) {
			pw_main_loop_run(data.loop);
			if (!monitor)
				break;
		}
	}
	spa_list_consume(rd, &data.remotes, link)
		remote_data_free(rd);

	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);
	pw_map_clear(&data.vars);
	pw_deinit();

	return 0;
}
