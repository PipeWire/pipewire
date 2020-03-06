/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
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

#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#include <alloca.h>
#include <limits.h>
#include <float.h>

#include <spa/utils/result.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>
#include <spa/utils/keys.h>

#include <pipewire/impl.h>

#include <extensions/session-manager.h>

static const char WHITESPACE[] = " \t";

struct remote_data;

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;

	struct spa_list remotes;
	struct remote_data *current;

	struct pw_map vars;
};

struct param_entry {
	struct spa_list link;
	uint32_t index;
	struct spa_pod *param;
};

struct param {
	struct spa_list link;
	uint32_t index;
	struct spa_param_info info;
	struct spa_list entries;
	uint32_t flags;
#define PARAM_ENUMERATED	(1U << 0)
#define PARAM_ENUMERATING	(1U << 1)
#define PARAM_SUBSCRIBED	(1U << 2)
#define PARAM_SUBSCRIBING	(1U << 3)
#define PARAM_ENUM_ERROR	(1U << 4)
#define PARAM_SUBSCRIBE_ERROR	(1U << 5)
	int enum_req;		/* the enum req id */
	int enum_pending;	/* core sync */
	int subscribe_req;
	int subscribe_pending;
};

struct global {
	struct remote_data *rd;
	uint32_t id;
	uint32_t permissions;
	uint32_t version;
	char *type;
	struct pw_proxy *proxy;
	uint32_t proxy_id;
	bool info_pending;
	struct pw_properties *properties;

	uint32_t flags;
#define GLOBAL_CAN_SUBSCRIBE_PARAMS		(1U << 0)
#define GLOBAL_CAN_ENUM_PARAMS			(1U << 1)
#define GLOBAL_PARAM_LIST_VALID     		(1U << 2)
#define GLOBAL_PARAM_SUBSCRIBE_IN_PROGRESS	(1U << 3)
#define GLOBAL_PARAM_ENUM_IN_PROGRESS		(1U << 5)
#define GLOBAL_PARAM_ENUM_COMPLETE		(1U << 6)
#define GLOBAL_PARAM_ENUM_DISPLAY		(1U << 7)

	int param_enum_pending;
	int param_subscribe_pending;

	struct spa_list params;
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
	struct pw_map globals_by_proxy;
};

struct proxy_data;

typedef void (*info_func_t) (struct proxy_data *pd);

struct proxy_data {
	struct remote_data *rd;
	struct global *global;
	struct pw_proxy *proxy;
        void *info;
	info_func_t info_func;
        pw_destroy_t destroy;
        struct spa_hook proxy_listener;
        struct spa_hook object_listener;
};

struct command {
	const char *name;
	const char *alias;
	const char *description;
	bool (*func) (struct data *data, const char *cmd, char *args, char **error);
};

static struct global *remote_global(struct remote_data *rd, uint32_t id);
static struct global *remote_global_by_proxy(struct remote_data *rd, uint32_t id);
static bool bind_global(struct remote_data *rd, struct global *global, char **error);
static bool global_can_subscribe_params(struct global *global);
static bool global_can_enum_params(struct global *global);
static int global_subscribe_params(struct global *global,
				   uint32_t *sub_param_ids,
				   uint32_t sub_n_params);
static int global_enum_params(struct global *global, int seq, uint32_t id,
			      uint32_t start, uint32_t num,
			      const struct spa_pod *filter);
static bool global_do_param_enum(struct global *global);
static bool global_do_param_subscribe(struct global *global);
static void global_done(struct global *global, int seq);

static int pw_split_ip(char *str, const char *delimiter, int max_tokens, char *tokens[])
{
	const char *state = NULL;
	char *s;
	size_t len;
	int n = 0;

        s = (char *)pw_split_walk(str, delimiter, &len, &state);
        while (s && n + 1 < max_tokens) {
		s[len] = '\0';
		tokens[n++] = s;
                s = (char*)pw_split_walk(str, delimiter, &len, &state);
        }
        if (s) {
		tokens[n++] = s;
        }
        return n;
}

static struct pw_properties *parse_props(char *str)
{
	const char *state = NULL;
	char *s, *p[3];
	size_t len, n;
	struct pw_properties *props = NULL;

	while (true) {
		s = (char *)pw_split_walk(str, WHITESPACE, &len, &state);
		if (s == NULL)
			break;

		s[len] = '\0';
		n = pw_split_ip(s, "=", 2, p);
		if (n == 2) {
			if (props == NULL)
				props = pw_properties_new(p[0], p[1], NULL);
			else
				pw_properties_set(props, p[0], p[1]);
		}
	}
	return props;
}

static void print_properties(struct spa_dict *props, char mark, bool header)
{
	const struct spa_dict_item *item;

	if (header)
		fprintf(stdout, "%c\tproperties:\n", mark);
	if (props == NULL || props->n_items == 0) {
		if (header)
			fprintf(stdout, "\t\tnone\n");
		return;
	}

	spa_dict_for_each(item, props) {
		fprintf(stdout, "%c\t\t%s = \"%s\"\n", mark, item->key, item->value);
	}
}

static void print_params(struct spa_param_info *params, uint32_t n_params, char mark, bool header)
{
	uint32_t i;

	if (header)
		fprintf(stdout, "%c\tparams: (%u)\n", mark, n_params);
	if (params == NULL || n_params == 0) {
		if (header)
			fprintf(stdout, "\t\tnone\n");
		return;
	}
	for (i = 0; i < n_params; i++) {
		const struct spa_type_info *type_info = spa_type_param;

		fprintf(stdout, "%c\t  %d (%s) %c%c\n",
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
static bool do_permissions(struct data *data, const char *cmd, char *args, char **error);
static bool do_get_permissions(struct data *data, const char *cmd, char *args, char **error);
static bool do_dump(struct data *data, const char *cmd, char *args, char **error);
static bool do_graph(struct data *data, const char *cmd, char *args, char **error);

#define DUMP_NAMES "Core|Module|Device|Node|Port|Factory|Client|Link|Session|Endpoint|EndpointStream|EndpointLink"

static struct command command_list[] = {
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
	{ "enum-params", "e", "Enumerate params of an object <object-id> [<param-id-name>]", do_enum_params },
	{ "permissions", "sp", "Set permissions for a client <client-id> <object> <permission>", do_permissions },
	{ "get-permissions", "gp", "Get permissions of a client <client-id>", do_get_permissions },
	{ "dump", "D", "Dump objects in ways that are cleaner for humans to understand "
		 "[short|deep|resolve|notype] [-sdrt] [all|"DUMP_NAMES"|<id>]", do_dump },
	{ "graph", "g", "Display tree graph in YAML/JSON format. <path>", do_graph },
};

static bool do_help(struct data *data, const char *cmd, char *args, char **error)
{
	size_t i;

	fprintf(stdout, "Available commands:\n");
	for (i = 0; i < SPA_N_ELEMENTS(command_list); i++) {
		fprintf(stdout, "\t%-20.20s\t%s\n", command_list[i].name, command_list[i].description);
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
	fprintf(stdout, "%d = @module:%d\n", id, pw_global_get_id(pw_impl_module_get_global(module)));

	return true;
}

static void on_core_info(void *_data, const struct pw_core_info *info)
{
	struct remote_data *rd = _data;
	free(rd->name);
	rd->name = info->name ? strdup(info->name) : NULL;
	fprintf(stdout, "remote %d is named '%s'\n", rd->id, rd->name);
}

static void show_prompt(struct remote_data *rd)
{
	fprintf(stdout, "%s>>", rd->name);
	fflush(stdout);
}

static void on_core_done(void *_data, uint32_t id, int seq)
{
	struct remote_data *rd = _data;

	if (id != 0)
		global_done(remote_global(rd, id), seq);

	if (id == 0 && seq == rd->prompt_pending)
		show_prompt(rd);
}

static int print_global(void *obj, void *data)
{
	struct global *global = obj;
	const char *filter = data;

	if (global == NULL)
		return 0;

	if (filter && !strstr(global->type, filter))
		return 0;

	fprintf(stdout, "\tid %d, type %s/%d\n", global->id,
					global->type, global->version);

	if (global->properties)
		print_properties(&global->properties->dict, ' ', false);

	return 0;
}


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
	global->rd = rd;
	global->id = id;
	global->permissions = permissions;
	global->type = strdup(type);
	global->version = version;
	global->properties = props ? pw_properties_new_dict(props) : NULL;

	spa_list_init(&global->params);

	global->flags =
		(global_can_subscribe_params(global) ? GLOBAL_CAN_SUBSCRIBE_PARAMS : 0) |
		(global_can_enum_params(global) ? GLOBAL_CAN_ENUM_PARAMS : 0);

	fprintf(stdout, "remote %d added global: ", rd->id);
	print_global(global, NULL);

	size = pw_map_get_size(&rd->globals);
	while (id > size)
		pw_map_insert_at(&rd->globals, size++, NULL);
	pw_map_insert_at(&rd->globals, id, global);

	/* immediately bind the object always */
	ret = bind_global(rd, global, &error);
	if (!ret) {
		fprintf(stdout, "Error: \"%s\"\n", error);
		free(error);
	}
}

static int destroy_global(void *obj, void *data)
{
	struct remote_data *rd;
	struct global *global = obj;
	struct param *p;
	struct param_entry *pe;

	if (global == NULL)
		return 0;

	while (!spa_list_is_empty(&global->params)) {
		p = spa_list_last(&global->params, struct param, link);
		spa_list_remove(&p->link);

		while (!spa_list_is_empty(&p->entries)) {
			pe = spa_list_last(&p->entries, struct param_entry, link);
			spa_list_remove(&pe->link);
			free(pe->param);
			free(pe);
		}

		free(p);
	}

	rd = global->rd;

	if (global->proxy_id)
		pw_map_remove(&rd->globals_by_proxy, global->proxy_id);

	pw_map_remove(&rd->globals, global->id);
	if (global->properties)
		pw_properties_free(global->properties);
	free(global->type);
	free(global);
	return 0;
}

/* get a global object, but only when everything is right */
static struct global *
remote_global(struct remote_data *rd, uint32_t id)
{
	struct global *global;

	if (!rd)
		return NULL;

	global = pw_map_lookup(&rd->globals, id);
	if (!global || !global->proxy || !pw_proxy_get_user_data(global->proxy))
		return NULL;
	return global;
}

static struct global *
remote_global_by_proxy(struct remote_data *rd, uint32_t id)
{
	struct global *global;

	if (!rd)
		return NULL;

	global = pw_map_lookup(&rd->globals_by_proxy, id);
	if (!global || !global->proxy || !pw_proxy_get_user_data(global->proxy))
		return NULL;
	return global;
}

static bool global_can_subscribe_params(struct global *global)
{
	if (!global)
		return false;

	return !strcmp(global->type, PW_TYPE_INTERFACE_Node) ||
	       !strcmp(global->type, PW_TYPE_INTERFACE_Port) ||
	       !strcmp(global->type, PW_TYPE_INTERFACE_Device) ||
	       !strcmp(global->type, PW_TYPE_INTERFACE_Endpoint) ||
	       !strcmp(global->type, PW_TYPE_INTERFACE_EndpointStream) ||
	       !strcmp(global->type, PW_TYPE_INTERFACE_EndpointLink);
}

static bool global_can_enum_params(struct global *global)
{
	if (!global)
		return false;

	return !strcmp(global->type, PW_TYPE_INTERFACE_Node) ||
	       !strcmp(global->type, PW_TYPE_INTERFACE_Port) ||
	       !strcmp(global->type, PW_TYPE_INTERFACE_Device) ||
	       !strcmp(global->type, PW_TYPE_INTERFACE_Endpoint) ||
	       !strcmp(global->type, PW_TYPE_INTERFACE_EndpointStream) ||
	       !strcmp(global->type, PW_TYPE_INTERFACE_EndpointLink);
}

static int global_subscribe_params(struct global *global,
				   uint32_t *sub_param_ids,
				   uint32_t sub_n_params)
{
	int ret;

	if (!global || !global->proxy)
		return -1;

	if (!sub_n_params)
		return 0;

	if (!sub_param_ids)
		return -1;

	if (!strcmp(global->type, PW_TYPE_INTERFACE_Node))
		ret = pw_node_subscribe_params((struct pw_node *)global->proxy,
				sub_param_ids, sub_n_params);
	else if (!strcmp(global->type, PW_TYPE_INTERFACE_Port))
		ret = pw_port_subscribe_params((struct pw_port *)global->proxy,
				sub_param_ids, sub_n_params);
	else if (!strcmp(global->type, PW_TYPE_INTERFACE_Device))
		ret = pw_device_subscribe_params((struct pw_device *)global->proxy,
				sub_param_ids, sub_n_params);
	else if (!strcmp(global->type, PW_TYPE_INTERFACE_Endpoint))
		ret = pw_endpoint_subscribe_params((struct pw_endpoint *)global->proxy,
				sub_param_ids, sub_n_params);
	else if (!strcmp(global->type, PW_TYPE_INTERFACE_EndpointStream))
		ret = pw_endpoint_stream_subscribe_params((struct pw_endpoint_stream *)global->proxy,
				sub_param_ids, sub_n_params);
	else if (!strcmp(global->type, PW_TYPE_INTERFACE_EndpointLink))
		ret = pw_endpoint_link_subscribe_params((struct pw_endpoint_link *)global->proxy,
				sub_param_ids, sub_n_params);
	else
		ret = -1;

	return ret;
}

static int global_enum_params(struct global *global, int seq, uint32_t id,
			      uint32_t start, uint32_t num,
			      const struct spa_pod *filter)
{
	int ret;

	if (!global || !global->proxy)
		return -EINVAL;

	if (!strcmp(global->type, PW_TYPE_INTERFACE_Node))
		ret = pw_node_enum_params((struct pw_node *)global->proxy,
					  seq, id, start, num, filter);
	else if (!strcmp(global->type, PW_TYPE_INTERFACE_Port))
		ret = pw_port_enum_params((struct pw_port *)global->proxy,
					  seq, id, start, num, filter);
	else if (!strcmp(global->type, PW_TYPE_INTERFACE_Device))
		ret = pw_device_enum_params((struct pw_device *)global->proxy,
					    seq, id, start, num, filter);
	else if (!strcmp(global->type, PW_TYPE_INTERFACE_Endpoint))
		ret = pw_endpoint_enum_params((struct pw_endpoint *)global->proxy,
					      seq, id, start, num, filter);
	else if (!strcmp(global->type, PW_TYPE_INTERFACE_EndpointStream))
		ret = pw_endpoint_stream_enum_params((struct pw_endpoint_stream *)global->proxy,
					      seq, id, start, num, filter);
	else if (!strcmp(global->type, PW_TYPE_INTERFACE_EndpointLink))
		ret = pw_endpoint_link_enum_params((struct pw_endpoint_link *)global->proxy,
					      seq, id, start, num, filter);
	else
		ret = -EINVAL;

	return ret;
}

static bool global_do_param_subscribe(struct global *global)
{
	struct remote_data *rd;
	uint32_t subscribe_n_params;
	struct param *p;
	int ret;

	if (!global)
		return false;

	if (!(global->flags & GLOBAL_CAN_SUBSCRIBE_PARAMS))
		return false;

	rd = global->rd;

	subscribe_n_params = 0;
	spa_list_for_each(p, &global->params, link) {
		if ((p->flags & (PARAM_SUBSCRIBED | PARAM_SUBSCRIBING | PARAM_SUBSCRIBE_ERROR)))
			continue;

		/* can't subscribed non-readable */
		if (!(p->info.flags & SPA_PARAM_INFO_READ))
			continue;

		ret = global_subscribe_params(global, &p->info.id, 1);

		if (SPA_RESULT_IS_OK(ret)) {
			subscribe_n_params++;
			p->flags |= PARAM_SUBSCRIBING;
			p->subscribe_req = ret;
			p->subscribe_pending = pw_core_sync(rd->core, global->id, 0);

		} else {
			fprintf(stdout, "id=%"PRIu32" param.id=%"PRIu32" subscribe error\n",
					global->id, p->info.id);
			p->flags |= PARAM_SUBSCRIBE_ERROR;
		}
	}

	if (!subscribe_n_params)
		return false;

	global->param_subscribe_pending = pw_core_sync(rd->core, global->id, 0);
	global->flags |= GLOBAL_PARAM_SUBSCRIBE_IN_PROGRESS;

	return true;
}

/* subscribe complete */
static void global_done_param_subscribe(struct global *global, int seq)
{
	if (!global)
		return;

	if (!(global->flags & GLOBAL_PARAM_SUBSCRIBE_IN_PROGRESS))
		return;

	if (seq != global->param_subscribe_pending)
		return;

	global->flags &= ~GLOBAL_PARAM_SUBSCRIBE_IN_PROGRESS;

	if (global->flags & GLOBAL_CAN_ENUM_PARAMS)
		global_do_param_enum(global);
}

static bool global_do_param_enum(struct global *global)
{
	struct remote_data *rd;
	uint32_t enum_n_params;
	struct param *p;
	int ret;

	if (!global)
		return false;

	if (!(global->flags & GLOBAL_CAN_ENUM_PARAMS))
		return false;

	rd = global->rd;

	enum_n_params = 0;
	spa_list_for_each(p, &global->params, link) {
		/* if enumerated, or enumerating or error skip */
		if (p->flags & (PARAM_ENUMERATED | PARAM_ENUMERATING | PARAM_ENUM_ERROR))
			continue;

		/* can't enumerate non-readable */
		if (!(p->info.flags & SPA_PARAM_INFO_READ))
			continue;

		ret = global_enum_params(global, 0, p->info.id, 0, 0, NULL);

		if (SPA_RESULT_IS_OK(ret)) {
			enum_n_params++;
			p->flags |= PARAM_ENUMERATING;
			p->enum_req = ret;
			p->enum_pending = pw_core_sync(rd->core, global->id, 0);

		} else {
			fprintf(stdout, "id=%"PRIu32" param.id=%"PRIu32" enumeration error\n",
					global->id, p->info.id);
			p->flags |= PARAM_ENUM_ERROR;
		}
	}

	if (!enum_n_params) {
		global->flags |= GLOBAL_PARAM_ENUM_COMPLETE;
		return false;
	}

	global->param_enum_pending = pw_core_sync(rd->core, global->id, 0);
	global->flags |= GLOBAL_PARAM_ENUM_IN_PROGRESS;

	return true;
}

/* parameter collection ended? */
static void global_done_param_enum(struct global *global, int seq)
{
	struct param *p;

	if (!global)
		return;

	if (!(global->flags & GLOBAL_PARAM_ENUM_IN_PROGRESS))
		return;

	spa_list_for_each(p, &global->params, link) {
		if ((p->flags & PARAM_ENUMERATING) && seq == p->enum_pending) {
			p->flags &= ~PARAM_ENUMERATING;
			p->flags |= PARAM_ENUMERATED;
		}
	}

	if (seq != global->param_enum_pending)
		return;

	global->flags &= ~GLOBAL_PARAM_ENUM_IN_PROGRESS;
	global->flags |= GLOBAL_PARAM_ENUM_COMPLETE;
}

static void global_done(struct global *global, int seq)
{
	if (!global)
		return;

	global_done_param_subscribe(global, seq);
	global_done_param_enum(global, seq);
}

static struct spa_param_info *
global_info_params(struct global *global, uint32_t *n_paramsp)
{
	struct proxy_data *pd;

	if (!global || !global->proxy || !n_paramsp)
		return NULL;

	pd = pw_proxy_get_user_data(global->proxy);
	if (!pd)
		return NULL;

	if (!strcmp(global->type, PW_TYPE_INTERFACE_Node)) {
		struct pw_node_info *info = pd->info;
		*n_paramsp = info->n_params;
		return info->params;
	}
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Port)) {
		struct pw_port_info *info = pd->info;
		*n_paramsp = info->n_params;
		return info->params;
	}
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Device)) {
		struct pw_device_info *info = pd->info;
		*n_paramsp = info->n_params;
		return info->params;
	}
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Endpoint)) {
		struct pw_endpoint_info *info = pd->info;
		*n_paramsp = info->n_params;
		return info->params;
	}
	if (!strcmp(global->type, PW_TYPE_INTERFACE_EndpointStream)) {
		struct pw_endpoint_stream_info *info = pd->info;
		*n_paramsp = info->n_params;
		return info->params;
	}
	if (!strcmp(global->type, PW_TYPE_INTERFACE_EndpointLink)) {
		struct pw_endpoint_link_info *info = pd->info;
		*n_paramsp = info->n_params;
		return info->params;
	}

	*n_paramsp = 0;
	return NULL;
}

static void global_param_event_info(struct global *global)
{
	struct proxy_data *pd;
	struct remote_data *rd;
	struct param *p, *pfound;
	uint32_t i, param_id;
	struct spa_param_info *params;
	uint32_t n_params;

	if (!global)
		return;

	params = global_info_params(global, &n_params);
	if (!params || !n_params)
		return;

	pd = pw_proxy_get_user_data(global->proxy);
	spa_assert(pd);

	rd = pd->rd;
	spa_assert(rd);

	for (i = 0; i < n_params; i++) {

		param_id = params[i].id;

		/* lookup param */
		pfound = NULL;
		spa_list_for_each(p, &global->params, link) {
			if (param_id == p->info.id) {
				pfound = p;
				break;
			}
		}
		/* not found? create it */
		if (!pfound) {
			p = malloc(sizeof(*p));
			spa_assert(p);
			p->index = i;
			memcpy(&p->info, &params[i], sizeof(p->info));
			spa_list_init(&p->entries);
			p->flags = 0;

			spa_list_append(&global->params, &p->link);
		}
	}

	if (!global_do_param_subscribe(global))
		global_do_param_enum(global);
}

static void registry_event_global_remove(void *data, uint32_t id)
{
	struct remote_data *rd = data;
	struct global *global;

	global = pw_map_lookup(&rd->globals, id);
	if (global == NULL) {
		fprintf(stdout, "remote %d removed unknown global %d\n", rd->id, id);
		return;
	}

	fprintf(stdout, "remote %d removed global: ", rd->id);
	print_global(global, NULL);
	destroy_global(global, rd);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void on_core_error(void *_data, uint32_t id, int seq, int res, const char *message)
{
	struct remote_data *rd = _data;
	struct data *data = rd->data;
	struct global *global;
	struct param *p;

	pw_log_error("remote %p: error id:%u seq:%d res:%d (%s): %s", rd,
			id, seq, res, spa_strerror(res), message);

	if (id == 0) {
		pw_main_loop_quit(data->loop);
		return;
	}

	/* try to associate with an operation in progress */
	global = remote_global_by_proxy(rd, id);
	if (global == NULL)
		return;

	spa_list_for_each(p, &global->params, link) {
		if ((p->flags & PARAM_ENUMERATING) && seq == p->enum_req) {
			p->flags &= ~PARAM_ENUMERATING;
			p->flags |= PARAM_ENUM_ERROR;
			pw_log_error("param %u.%u (%s) failed to enumerate",
					id, p->info.id,
					spa_debug_type_find_name(spa_type_param, p->info.id));
			continue;
		}
		if ((p->flags & PARAM_SUBSCRIBING) && seq == p->subscribe_req) {
			p->flags &= ~PARAM_SUBSCRIBING;
			p->flags |= PARAM_SUBSCRIBE_ERROR;
			pw_log_error("param %u.%u (%s) failed to subscribe",
					id, p->info.id,
					spa_debug_type_find_name(spa_type_param, p->info.id));
			continue;
		}
	}

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

	pw_map_remove(&data->vars, rd->id);
	pw_map_for_each(&rd->globals, destroy_global, rd);

	if (data->current == rd)
		data->current = NULL;
	free(rd->name);
}

static const struct pw_proxy_events proxy_core_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = on_core_destroy,
};

static bool do_connect(struct data *data, const char *cmd, char *args, char **error)
{
	char *a[1];
        int n;
	struct pw_properties *props = NULL;
	struct pw_core *core;
	struct remote_data *rd;

	n = pw_split_ip(args, WHITESPACE, 1, a);
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
	pw_map_init(&rd->globals_by_proxy, 64, 16);
	rd->id = pw_map_insert_new(&data->vars, rd);
	spa_list_append(&data->remotes, &rd->link);

	fprintf(stdout, "%d = @remote:%p\n", rd->id, rd->core);
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
	pw_core_disconnect(rd->core);

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
		fprintf(stdout, "\t%d = @remote:%p '%s'\n", rd->id, rd->core, rd->name);

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

	fprintf(stdout, "\tid: %d\n", global->id);
	fprintf(stdout, "\tpermissions: %c%c%c\n", global->permissions & PW_PERM_R ? 'r' : '-',
					  global->permissions & PW_PERM_W ? 'w' : '-',
					  global->permissions & PW_PERM_X ? 'x' : '-');
	fprintf(stdout, "\ttype: %s/%d\n", global->type, global->version);
}

static void info_core(struct proxy_data *pd)
{
	struct pw_core_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "\tcookie: %u\n", info->cookie);
	fprintf(stdout, "\tuser-name: \"%s\"\n", info->user_name);
	fprintf(stdout, "\thost-name: \"%s\"\n", info->host_name);
	fprintf(stdout, "\tversion: \"%s\"\n", info->version);
	fprintf(stdout, "\tname: \"%s\"\n", info->name);
	print_properties(info->props, MARK_CHANGE(PW_CORE_CHANGE_MASK_PROPS), true);
	info->change_mask = 0;
}

static void info_module(struct proxy_data *pd)
{
	struct pw_module_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "\tname: \"%s\"\n", info->name);
	fprintf(stdout, "\tfilename: \"%s\"\n", info->filename);
	fprintf(stdout, "\targs: \"%s\"\n", info->args);
	print_properties(info->props, MARK_CHANGE(PW_MODULE_CHANGE_MASK_PROPS), true);
	info->change_mask = 0;
}

static void info_node(struct proxy_data *pd)
{
	struct pw_node_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "%c\tinput ports: %u/%u\n", MARK_CHANGE(PW_NODE_CHANGE_MASK_INPUT_PORTS),
			info->n_input_ports, info->max_input_ports);
	fprintf(stdout, "%c\toutput ports: %u/%u\n", MARK_CHANGE(PW_NODE_CHANGE_MASK_OUTPUT_PORTS),
			info->n_output_ports, info->max_output_ports);
	fprintf(stdout, "%c\tstate: \"%s\"", MARK_CHANGE(PW_NODE_CHANGE_MASK_STATE),
			pw_node_state_as_string(info->state));
	if (info->state == PW_NODE_STATE_ERROR && info->error)
		fprintf(stdout, " \"%s\"\n", info->error);
	else
		fprintf(stdout, "\n");
	print_properties(info->props, MARK_CHANGE(PW_NODE_CHANGE_MASK_PROPS), true);
	print_params(info->params, info->n_params, MARK_CHANGE(PW_NODE_CHANGE_MASK_PARAMS), true);
	info->change_mask = 0;
}

static void info_port(struct proxy_data *pd)
{
	struct pw_port_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "\tdirection: \"%s\"\n", pw_direction_as_string(info->direction));
	print_properties(info->props, MARK_CHANGE(PW_PORT_CHANGE_MASK_PROPS), true);
	print_params(info->params, info->n_params, MARK_CHANGE(PW_PORT_CHANGE_MASK_PARAMS), true);
	info->change_mask = 0;
}

static void info_factory(struct proxy_data *pd)
{
	struct pw_factory_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "\tname: \"%s\"\n", info->name);
	fprintf(stdout, "\tobject-type: %s/%d\n", info->type, info->version);
	print_properties(info->props, MARK_CHANGE(PW_FACTORY_CHANGE_MASK_PROPS), true);
	info->change_mask = 0;
}

static void info_client(struct proxy_data *pd)
{
	struct pw_client_info *info = pd->info;

	info_global(pd);
	print_properties(info->props, MARK_CHANGE(PW_CLIENT_CHANGE_MASK_PROPS), true);
	info->change_mask = 0;
}

static void info_link(struct proxy_data *pd)
{
	struct pw_link_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "\toutput-node-id: %u\n", info->output_node_id);
	fprintf(stdout, "\toutput-port-id: %u\n", info->output_port_id);
	fprintf(stdout, "\tinput-node-id: %u\n", info->input_node_id);
	fprintf(stdout, "\tinput-port-id: %u\n", info->input_port_id);

	fprintf(stdout, "%c\tstate: \"%s\"", MARK_CHANGE(PW_LINK_CHANGE_MASK_STATE),
			pw_link_state_as_string(info->state));
	if (info->state == PW_LINK_STATE_ERROR && info->error)
		printf(" \"%s\"\n", info->error);
	else
		printf("\n");
	fprintf(stdout, "%c\tformat:\n", MARK_CHANGE(PW_LINK_CHANGE_MASK_FORMAT));
	if (info->format)
		spa_debug_format(2, NULL, info->format);
	else
		fprintf(stdout, "\t\tnone\n");
	print_properties(info->props, MARK_CHANGE(PW_LINK_CHANGE_MASK_PROPS), true);
	info->change_mask = 0;
}

static void info_device(struct proxy_data *pd)
{
	struct pw_device_info *info = pd->info;

	info_global(pd);
	print_properties(info->props, MARK_CHANGE(PW_DEVICE_CHANGE_MASK_PROPS), true);
	print_params(info->params, info->n_params, MARK_CHANGE(PW_DEVICE_CHANGE_MASK_PARAMS), true);
	info->change_mask = 0;
}

static void info_session(struct proxy_data *pd)
{
	struct pw_session_info *info = pd->info;

	info_global(pd);
	print_properties(info->props, MARK_CHANGE(0), true);
	print_params(info->params, info->n_params, MARK_CHANGE(1), true);
	info->change_mask = 0;
}

static void info_endpoint(struct proxy_data *pd)
{
	struct pw_endpoint_info *info = pd->info;
	const char *direction;

	info_global(pd);
	fprintf(stdout, "\tname: %s\n", info->name);
	fprintf(stdout, "\tmedia-class: %s\n",  info->media_class);
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
	fprintf(stdout, "\tdirection: %s\n", direction);
	fprintf(stdout, "\tflags: 0x%x\n", info->flags);
	fprintf(stdout, "%c\tstreams: %u\n", MARK_CHANGE(0), info->n_streams);
	fprintf(stdout, "%c\tsession: %u\n", MARK_CHANGE(1), info->session_id);
	print_properties(info->props, MARK_CHANGE(2), true);
	print_params(info->params, info->n_params, MARK_CHANGE(3), true);
	info->change_mask = 0;
}

static void info_endpoint_stream(struct proxy_data *pd)
{
	struct pw_endpoint_stream_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "\tid: %u\n", info->id);
	fprintf(stdout, "\tendpoint-id: %u\n", info->endpoint_id);
	fprintf(stdout, "\tname: %s\n", info->name);
	print_properties(info->props, MARK_CHANGE(1), true);
	print_params(info->params, info->n_params, MARK_CHANGE(2), true);
	info->change_mask = 0;
}

const char *_pw_endpoint_link_state_as_string(enum pw_endpoint_link_state state)
{
	switch (state) {
	case PW_ENDPOINT_LINK_STATE_ERROR:
		return "error";
	case PW_ENDPOINT_LINK_STATE_PREPARING:
		return "preparing";
	case PW_ENDPOINT_LINK_STATE_INACTIVE:
		return "inactive";
	case PW_ENDPOINT_LINK_STATE_ACTIVE:
		return "active";
	}
	return "invalid-state";
}

static void info_endpoint_link(struct proxy_data *pd)
{
	struct pw_endpoint_link_info *info = pd->info;

	info_global(pd);
	fprintf(stdout, "\tid: %u\n", info->id);
	fprintf(stdout, "\tsession-id: %u\n", info->session_id);
	fprintf(stdout, "\toutput-endpoint-id: %u\n", info->output_endpoint_id);
	fprintf(stdout, "\toutput-stream-id: %u\n", info->output_stream_id);
	fprintf(stdout, "\tinput-endpoint-id: %u\n", info->input_endpoint_id);
	fprintf(stdout, "\tinput-stream-id: %u\n", info->input_stream_id);
	fprintf(stdout, "%c\tstate: \"%s\"", MARK_CHANGE(PW_ENDPOINT_LINK_CHANGE_MASK_STATE),
			_pw_endpoint_link_state_as_string(info->state));
	if (info->state == PW_ENDPOINT_LINK_STATE_ERROR && info->error)
		fprintf(stdout, " \"%s\"\n", info->error);
	else
		fprintf(stdout, "\n");
	print_properties(info->props, MARK_CHANGE(1), true);
	print_params(info->params, info->n_params, MARK_CHANGE(2), true);
	info->change_mask = 0;
}

static void core_event_info(void *object, const struct pw_core_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d core %d changed\n", rd->id, info->id);
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


static void module_event_info(void *object, const struct pw_module_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d module %d changed\n", rd->id, info->id);
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

static void node_event_info(void *object, const struct pw_node_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	struct global *global;

	if (pd->info)
		fprintf(stdout, "remote %d node %d changed\n", rd->id, info->id);
	pd->info = pw_node_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);

	global = pd->global;
	if (!global)
		return;

	if (global && global->info_pending) {
		info_node(pd);
		global->info_pending = false;
	}

	global_param_event_info(global);
}

static void event_param(void *object, int seq, uint32_t id,
		uint32_t index, uint32_t next, const struct spa_pod *param)
{
        struct proxy_data *data = object;
	struct remote_data *rd = data->rd;
	struct global *global;
	struct param *p, *pfound;
	struct param_entry *pe, *pefound;

	/* get global */
	global = data->global;
	if (!global)
		return;

	if (global->flags & GLOBAL_PARAM_ENUM_DISPLAY) {
		global->flags &= ~GLOBAL_PARAM_ENUM_DISPLAY;

		fprintf(stdout, "remote %d object %d param %d index %d\n",
				rd->id, global->id, id, index);

		if (spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Format))
			spa_debug_format(2, NULL, param);
		else
			spa_debug_pod(2, NULL, param);
	}

	/* find param (it should exist) */
	pfound = NULL;
	spa_list_for_each(p, &global->params, link) {
		if (p->info.id == id) {
			pfound = p;
			break;
		}
	}

	if (!pfound) {
		fprintf(stdout, "could not find object %d param %d index %d\n", global->id, id, index);
		return;
	}
	p = pfound;

	/* find param entry (it may not exist) */
	pefound = NULL;
	spa_list_for_each(pe, &pfound->entries, link) {
		if (pe->index == index) {
			pefound = pe;
			break;
		}
	}

	if (!pefound) {
		pe = malloc(sizeof(*pe));
		spa_assert(pe);
		pe->index = index;
		pe->param = malloc(SPA_POD_SIZE(param));
		spa_assert(pe->param);
		memcpy(pe->param, param, SPA_POD_SIZE(param));

		spa_list_append(&p->entries, &pe->link);
	} else {
		pe = pefound;
		if (SPA_POD_SIZE(param) != SPA_POD_SIZE(pe->param) ||
		    memcmp(param, pe->param, SPA_POD_SIZE(pe->param))) {

			/* updated */

			free(pe->param);

			pe->param = malloc(SPA_POD_SIZE(param));
			spa_assert(pe->param);
			memcpy(pe->param, param, SPA_POD_SIZE(param));
		}
	}
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_event_info,
	.param = event_param
};


static void port_event_info(void *object, const struct pw_port_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	struct global *global;

	if (pd->info)
		fprintf(stdout, "remote %d port %d changed\n", rd->id, info->id);
	pd->info = pw_port_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);

	global = pd->global;
	if (!global)
		return;

	if (global->info_pending) {
		info_port(pd);
		global->info_pending = false;
	}

	global_param_event_info(global);
}

static const struct pw_port_events port_events = {
	PW_VERSION_PORT_EVENTS,
	.info = port_event_info,
	.param = event_param
};

static void factory_event_info(void *object, const struct pw_factory_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d factory %d changed\n", rd->id, info->id);
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

static void client_event_info(void *object, const struct pw_client_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d client %d changed\n", rd->id, info->id);
	pd->info = pw_client_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);
	if (pd->global && pd->global->info_pending) {
		info_client(pd);
		pd->global->info_pending = false;
	}
}

static void client_event_permissions(void *object, uint32_t index,
		uint32_t n_permissions, const struct pw_permission *permissions)
{
        struct proxy_data *data = object;
	struct remote_data *rd = data->rd;
	uint32_t i;

	fprintf(stdout, "remote %d node %d index %d\n",
			rd->id, data->global->id, index);

	for (i = 0; i < n_permissions; i++) {
		if (permissions[i].id == PW_ID_ANY)
			fprintf(stdout, "  default:");
		else
			fprintf(stdout, "  %u:", permissions[i].id);
		fprintf(stdout, " %08x\n", permissions[i].permissions);
	}
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.info = client_event_info,
	.permissions = client_event_permissions
};

static void link_event_info(void *object, const struct pw_link_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	if (pd->info)
		fprintf(stdout, "remote %d link %d changed\n", rd->id, info->id);
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


static void device_event_info(void *object, const struct pw_device_info *info)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	struct global *global;

	if (pd->info)
		fprintf(stdout, "remote %d device %d changed\n", rd->id, info->id);
	pd->info = pw_device_info_update(pd->info, info);
	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);

	global = pd->global;
	if (!global)
		return;

	if (global->info_pending) {
		info_device(pd);
		global->info_pending = false;
	}

	global_param_event_info(global);
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.info = device_event_info,
	.param = event_param
};

static void session_info_free(struct pw_session_info *info)
{
	free(info->params);
	if (info->props)
		pw_properties_free ((struct pw_properties *)info->props);
	free(info);
}

static void session_event_info(void *object,
				const struct pw_session_info *update)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	struct pw_session_info *info = pd->info;
	struct global *global;

	if (!info) {
		info = pd->info = calloc(1, sizeof(*info));
		info->id = update->id;
	}
	if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_PARAMS) {
		info->n_params = update->n_params;
		free(info->params);
		info->params = malloc(info->n_params * sizeof(struct spa_param_info));
		memcpy(info->params, update->params,
			info->n_params * sizeof(struct spa_param_info));
	}
	if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_PROPS) {
		if (info->props)
			pw_properties_free ((struct pw_properties *)info->props);
		info->props =
			(struct spa_dict *) pw_properties_new_dict (update->props);
	}

	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);

	global = pd->global;
	if (!global)
		return;

	if (pd->global->info_pending) {
		info_session(pd);
		global->info_pending = false;
	}

	global_param_event_info(global);
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
	if (info->props)
		pw_properties_free ((struct pw_properties *)info->props);
	free(info);
}

static void endpoint_event_info(void *object,
				const struct pw_endpoint_info *update)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	struct pw_endpoint_info *info = pd->info;
	struct global *global;

	if (!info) {
		info = pd->info = calloc(1, sizeof(*info));
		info->id = update->id;
		info->name = update->name ? strdup(update->name) : NULL;
		info->media_class = update->media_class ? strdup(update->media_class) : NULL;
		info->direction = update->direction;
		info->flags = update->flags;
	}
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
		if (info->props)
			pw_properties_free ((struct pw_properties *)info->props);
		info->props =
			(struct spa_dict *) pw_properties_new_dict (update->props);
	}

	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);

	global = pd->global;
	if (!global)
		return;

	if (global->info_pending) {
		info_endpoint(pd);
		global->info_pending = false;
	}

	global_param_event_info(global);
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
	if (info->props)
		pw_properties_free ((struct pw_properties *)info->props);
	free(info);
}

static void endpoint_stream_event_info(void *object,
				const struct pw_endpoint_stream_info *update)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	struct pw_endpoint_stream_info *info = pd->info;
	struct global *global;

	if (!info) {
		info = pd->info = calloc(1, sizeof(*info));
		info->id = update->id;
		info->endpoint_id = update->endpoint_id;
		info->name = update->name ? strdup(update->name) : NULL;
	}
	if (update->change_mask & PW_ENDPOINT_STREAM_CHANGE_MASK_PARAMS) {
		info->n_params = update->n_params;
		free(info->params);
		info->params = malloc(info->n_params * sizeof(struct spa_param_info));
		memcpy(info->params, update->params,
			info->n_params * sizeof(struct spa_param_info));
	}
	if (update->change_mask & PW_ENDPOINT_STREAM_CHANGE_MASK_PROPS) {
		if (info->props)
			pw_properties_free ((struct pw_properties *)info->props);
		info->props =
			(struct spa_dict *) pw_properties_new_dict (update->props);
	}

	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);

	global = pd->global;

	if (!global)
		return;

	if (global->info_pending) {
		info_endpoint_stream(pd);
		global->info_pending = false;
	}

	global_param_event_info(global);
}

static const struct pw_endpoint_stream_events endpoint_stream_events = {
	PW_VERSION_ENDPOINT_STREAM_EVENTS,
	.info = endpoint_stream_event_info,
	.param = event_param
};

static void endpoint_link_info_free(struct pw_endpoint_link_info *info)
{
	if (info->error)
		free(info->error);
	free(info->params);
	if (info->props)
		pw_properties_free ((struct pw_properties *)info->props);
	free(info);
}

static void endpoint_link_event_info(void *object,
				const struct pw_endpoint_link_info *update)
{
	struct proxy_data *pd = object;
	struct remote_data *rd = pd->rd;
	struct pw_endpoint_link_info *info = pd->info;
	struct global *global;

	if (!info) {
		info = pd->info = calloc(1, sizeof(*info));
		info->id = update->id;
		info->session_id = update->session_id;
		info->output_endpoint_id = update->output_endpoint_id;
		info->output_stream_id = update->output_stream_id;
		info->input_endpoint_id = update->input_endpoint_id;
		info->input_stream_id = update->input_stream_id;
		info->error = update->error ? strdup(update->error) : NULL;
	}
	if (update->change_mask & PW_ENDPOINT_LINK_CHANGE_MASK_STATE)
		info->state = update->state;
	if (update->change_mask & PW_ENDPOINT_LINK_CHANGE_MASK_PARAMS) {
		info->n_params = update->n_params;
		free(info->params);
		info->params = malloc(info->n_params * sizeof(struct spa_param_info));
		memcpy(info->params, update->params,
			info->n_params * sizeof(struct spa_param_info));
	}
	if (update->change_mask & PW_ENDPOINT_LINK_CHANGE_MASK_PROPS) {
		if (info->props)
			pw_properties_free ((struct pw_properties *)info->props);
		info->props =
			(struct spa_dict *) pw_properties_new_dict (update->props);
	}

	if (pd->global == NULL)
		pd->global = pw_map_lookup(&rd->globals, info->id);

	global = pd->global;

	if (!global)
		return;

	if (global->info_pending) {
		info_endpoint_link(pd);
		global->info_pending = false;
	}

	global_param_event_info(global);
}

static const struct pw_endpoint_link_events endpoint_link_events = {
	PW_VERSION_ENDPOINT_LINK_EVENTS,
	.info = endpoint_link_event_info,
	.param = event_param
};

static void
destroy_proxy (void *data)
{
	struct proxy_data *pd = data;

	if (pd->info == NULL)
		return;

	if (pd->global)
		pd->global->proxy = NULL;

	if (pd->destroy)
		pd->destroy(pd->info);
	pd->info = NULL;
}

static const struct pw_proxy_events proxy_events = {
        PW_VERSION_PROXY_EVENTS,
        .destroy = destroy_proxy,
};

static bool do_list_objects(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	pw_map_for_each(&rd->globals, print_global, args);
	return true;
}

static bool bind_global(struct remote_data *rd, struct global *global, char **error)
{
        const void *events;
        uint32_t client_version;
	info_func_t info_func;
        pw_destroy_t destroy;
	struct proxy_data *pd;
	struct pw_proxy *proxy;
	size_t size;

	if (strcmp(global->type, PW_TYPE_INTERFACE_Core) == 0) {
		events = &core_events;
		client_version = PW_VERSION_CORE;
		destroy = (pw_destroy_t) pw_core_info_free;
		info_func = info_core;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Module) == 0) {
		events = &module_events;
		client_version = PW_VERSION_MODULE;
		destroy = (pw_destroy_t) pw_module_info_free;
		info_func = info_module;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Device) == 0) {
		events = &device_events;
		client_version = PW_VERSION_DEVICE;
		destroy = (pw_destroy_t) pw_device_info_free;
		info_func = info_device;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Node) == 0) {
		events = &node_events;
		client_version = PW_VERSION_NODE;
		destroy = (pw_destroy_t) pw_node_info_free;
		info_func = info_node;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Port) == 0) {
		events = &port_events;
		client_version = PW_VERSION_PORT;
		destroy = (pw_destroy_t) pw_port_info_free;
		info_func = info_port;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Factory) == 0) {
		events = &factory_events;
		client_version = PW_VERSION_FACTORY;
		destroy = (pw_destroy_t) pw_factory_info_free;
		info_func = info_factory;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Client) == 0) {
		events = &client_events;
		client_version = PW_VERSION_CLIENT;
		destroy = (pw_destroy_t) pw_client_info_free;
		info_func = info_client;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Link) == 0) {
		events = &link_events;
		client_version = PW_VERSION_LINK;
		destroy = (pw_destroy_t) pw_link_info_free;
		info_func = info_link;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Session) == 0) {
		events = &session_events;
		client_version = PW_VERSION_SESSION;
		destroy = (pw_destroy_t) session_info_free;
		info_func = info_session;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_Endpoint) == 0) {
		events = &endpoint_events;
		client_version = PW_VERSION_ENDPOINT;
		destroy = (pw_destroy_t) endpoint_info_free;
		info_func = info_endpoint;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_EndpointStream) == 0) {
		events = &endpoint_stream_events;
		client_version = PW_VERSION_ENDPOINT_STREAM;
		destroy = (pw_destroy_t) endpoint_stream_info_free;
		info_func = info_endpoint_stream;
	} else if (strcmp(global->type, PW_TYPE_INTERFACE_EndpointLink) == 0) {
		events = &endpoint_link_events;
		client_version = PW_VERSION_ENDPOINT_LINK;
		destroy = (pw_destroy_t) endpoint_link_info_free;
		info_func = info_endpoint_link;
	} else {
		*error = spa_aprintf("unsupported type %s", global->type);
		return false;
	}

	proxy = pw_registry_bind(rd->registry,
				       global->id,
				       global->type,
				       client_version,
				       sizeof(struct proxy_data));

	pd = pw_proxy_get_user_data(proxy);
	pd->rd = rd;
	pd->global = global;
	pd->proxy = proxy;
	pd->info_func = info_func;
	pd->destroy = destroy;
	pw_proxy_add_object_listener(proxy, &pd->object_listener, events, pd);
	pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);

	global->proxy = proxy;
	global->proxy_id = pw_proxy_get_id(proxy);

	size = pw_map_get_size(&rd->globals_by_proxy);
	while (global->proxy_id > size)
		pw_map_insert_at(&rd->globals_by_proxy, size++, NULL);
	pw_map_insert_at(&rd->globals_by_proxy, global->proxy_id, global);

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
		if (pd->info_func)
			pd->info_func(pd);
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
	uint32_t id;
	struct global *global;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n < 1) {
		*error = spa_aprintf("%s <object-id>|all", cmd);
		return false;
	}
	if (strcmp(a[0], "all") == 0) {
		pw_map_for_each(&rd->globals, do_global_info_all, NULL);
	}
	else {
		id = atoi(a[0]);
		global = pw_map_lookup(&rd->globals, id);
		if (global == NULL) {
			*error = spa_aprintf("%s: unknown global %d", cmd, id);
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
		props = parse_props(a[1]);

	proxy = pw_core_create_object(rd->core, a[0],
					    PW_TYPE_INTERFACE_Device,
					    PW_VERSION_DEVICE,
					    props ? &props->dict : NULL,
					    sizeof(struct proxy_data));

	pd = pw_proxy_get_user_data(proxy);
	pd->rd = rd;
	pd->proxy = proxy;
	pd->destroy = (pw_destroy_t) pw_device_info_free;
	pw_proxy_add_object_listener(proxy, &pd->object_listener, &device_events, pd);
	pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);

	id = pw_map_insert_new(&data->vars, proxy);
	fprintf(stdout, "%d = @proxy:%d\n", id, pw_proxy_get_id(proxy));

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
		props = parse_props(a[1]);

	proxy = pw_core_create_object(rd->core, a[0],
					    PW_TYPE_INTERFACE_Node,
					    PW_VERSION_NODE,
					    props ? &props->dict : NULL,
					    sizeof(struct proxy_data));

	pd = pw_proxy_get_user_data(proxy);
	pd->rd = rd;
	pd->proxy = proxy;
        pd->destroy = (pw_destroy_t) pw_node_info_free;
        pw_proxy_add_object_listener(proxy, &pd->object_listener, &node_events, pd);
        pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);

	id = pw_map_insert_new(&data->vars, proxy);
	fprintf(stdout, "%d = @proxy:%d\n", id, pw_proxy_get_id(proxy));

	return true;
}

static bool do_destroy(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[1];
        int n;
	uint32_t id;
	struct global *global;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n < 1) {
		*error = spa_aprintf("%s <object-id>", cmd);
		return false;
	}
	id = atoi(a[0]);
	global = pw_map_lookup(&rd->globals, id);
	if (global == NULL) {
		*error = spa_aprintf("%s: unknown global %d", cmd, id);
		return false;
	}
	pw_registry_destroy(rd->registry, id);

	return true;
}

static bool do_create_link(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[5];
        int n;
	uint32_t id;
	struct pw_proxy *proxy;
	struct pw_properties *props = NULL;
	struct proxy_data *pd;

	n = pw_split_ip(args, WHITESPACE, 5, a);
	if (n < 4) {
		*error = spa_aprintf("%s <node-id> <port> <node-id> <port> [<properties>]", cmd);
		return false;
	}
	if (n == 5)
		props = parse_props(a[4]);
	else
		props = pw_properties_new(NULL, NULL);

	pw_properties_set(props, PW_KEY_LINK_OUTPUT_NODE, a[0]);
	pw_properties_set(props, PW_KEY_LINK_OUTPUT_PORT, a[1]);
	pw_properties_set(props, PW_KEY_LINK_INPUT_NODE, a[2]);
	pw_properties_set(props, PW_KEY_LINK_INPUT_PORT, a[3]);

	proxy = (struct pw_proxy*)pw_core_create_object(rd->core,
					  "link-factory",
					  PW_TYPE_INTERFACE_Link,
					  PW_VERSION_LINK,
					  props ? &props->dict : NULL,
					  sizeof(struct proxy_data));

	pd = pw_proxy_get_user_data(proxy);
	pd->rd = rd;
	pd->proxy = proxy;
        pd->destroy = (pw_destroy_t) pw_link_info_free;
        pw_proxy_add_object_listener(proxy, &pd->object_listener, &link_events, pd);
        pw_proxy_add_listener(proxy, &pd->proxy_listener, &proxy_events, pd);

	id = pw_map_insert_new(&data->vars, proxy);
	fprintf(stdout, "%d = @proxy:%d\n", id, pw_proxy_get_id((struct pw_proxy*)proxy));

	return true;
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
	fprintf(stdout, "%d = @proxy:%d\n", id, pw_proxy_get_id((struct pw_proxy*)proxy));

	return true;

      no_remote:
        *error = spa_aprintf("Remote %d does not exist", idx);
	return false;
}

enum var_format {
	var_cmdline,	/* foo=bar format */
	var_json,
	var_yaml,
};

#define VF_TYPE_NUMERIC	SPA_TYPE_NUMERIC
#define VF_TYPE_FULL	SPA_TYPE_FULL

struct var_ctx {
	char *bs, *be;
	char *buf;
	enum var_format fmt;
	unsigned int flags;
	int level;
	int ind;	/* this is used by the formatters */
	bool is_ind;	/* used by formatters (mainly YAML) */
	unsigned int ind_top;
	int ind_stack[32];
};

static int
var_printf(struct var_ctx *v, const char *fmt, ...)
	 SPA_PRINTF_FUNC(2, 3);
static int
var_vprintf(struct var_ctx *v, const char *fmt, va_list ap)
	 SPA_PRINTF_FUNC(2, 0);

void
var_init(struct var_ctx *v, char *buf, size_t bufsz, enum var_format fmt, unsigned int flags, int level, int ind)
{
	/* protect against stupidity */
	spa_assert(v);
	spa_assert(buf);
	spa_assert(bufsz);

	memset(v, 0, sizeof(*v));

	v->buf = buf;
	v->bs = v->buf;
	v->be = v->buf + bufsz;
	v->buf[0] = '\0';
	v->fmt = fmt;
	v->flags = flags;
	v->level = level;
	v->ind = ind;
	v->is_ind = true;
}

static int
var_vprintf(struct var_ctx *v, const char *fmt, va_list ap)
{
	int ret;

	ret = vsnprintf(v->bs, v->be - v->bs, fmt, ap);
	if (ret < 0)
		return ret;
	v->bs += ret;
	return ret;
}

static int
var_printf(struct var_ctx *v, const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = var_vprintf(v, fmt, args);
	va_end(args);

	return ret;
}

const char *
var_get_index(struct var_ctx *v, const char *s, unsigned int *idxp)
{
	const char *ss;
	unsigned long ul;
	char cs;

	if (!s || !idxp)
		return NULL;
	if (*s != '[' && *s != '/' && !isdigit(*s))
		return NULL;
	cs = *s;
	if (*s == '[' || *s == '/')
		s++;
	ss = s;
	while (isdigit(*s))
		s++;
	if (cs == '[') {
		if (*s != ']')
			return NULL;
	} else {
		if (*s != '/' && *s != '\0')
			return NULL;
	}
	ul = strtoul(ss, NULL, 0);
	if (ul > UINT_MAX)
		return NULL;
	*idxp = (unsigned int)ul;
	return s + (*s ? 1 : 0);
}

const char *
var_get_key(struct var_ctx *v, const char *s, char *buf, size_t sz)
{
	const char *ss;
	size_t len;
	char c;
	char *bs, *be;

	if (!s || !buf || !*s)
		return NULL;

	/* skip over . (or /) */
	if (*s == '.' || *s == '/')
		s++;

	if (*s != '"') {
		ss = s;
		while (*s && !isspace(*s) && *s != '[' && *s != '.' && *s != '/')
			s++;
		len = s - ss;
		if (len + 1 > sz)
			return NULL;
		memcpy(buf, ss, len);
		buf[len] = '\0';
		if (*s == '.' || *s == '/')
			s++;
		return s;
	}

	s++;
	bs = buf;
	be = bs + sz;
	while ((c = *s) != '\0') {
		if (c == '"') {
			if (bs >= be)
				return NULL;
			*bs++ = '\0';
			s++;
			if (*s == '.' || *s == '/')
				s++;
			return s;
		}
		if (c == '\\') {
			c = *++s;
			switch (c) {
			case '0':
				c = '\0';
				break;
			case 't':
				c = '\t';
				break;
			case 'n':
				c = '\n';
				break;
			case 'a':
				c = '\a';
				break;
			case 'b':
				c = '\b';
				break;
			case 'v':
				c = '\v';
				break;
			case 'f':
				c = '\f';
				break;
			case '/':
				c = '/';
				break;
			default:
				break;
			}
		}
		if (bs >= be)
			return NULL;
		*bs++ = c;
		s++;
	}

	/* end without '"' */
	return NULL;
}

#define quote_if_needed(_str) \
	({ \
		const char *__str = (_str), *_s; \
		char *_as, *_ss; \
		size_t _l; \
		char _c; \
		\
		_s = __str; \
		while ((_c = *_s++) != '\0' && isprint(_c)) { \
			if (strchr(" {}<>[],\"", _c)) \
				break; \
			if (_c == ':' && isspace(*_s)) \
				break; \
		} \
		if (_c) { \
			_s = __str; \
			_l = 0; \
			while ((_c = *_s++) != '\0' && isprint(_c)) { \
				if (_c == '\"' || _c == '\\') \
					_l++; \
				_l++; \
			} \
			_as = alloca(1 + _l + 1 + 1); \
			_ss = _as; \
			*_ss++ = '"'; \
			_s = __str; \
			while ((_c = *_s++) != '\0' && isprint(_c)) { \
				if (_c == '\"' || _c == '\\') \
					*_ss++ = '\\'; \
				*_ss++ = _c; \
			} \
			*_ss++ = '"'; \
			*_ss = '\0'; \
			__str = _as; \
		} \
		__str; \
	})

#define SPA_TYPE_NUMERIC	(1U << 0)
#define SPA_TYPE_FULL		(1U << 1)

const char *
spa_type_get_choice_key(enum spa_choice_type type, unsigned int idx, unsigned int flags)
{
	static const char *ranges[] = {
		"default", "min", "max"
	};
	static const char *steps[] = {
		"default", "min", "max", "step"
	};
	static const char *alts[] = {
		"default",	/* default + 10 alternatives */
		"alt0", "alt1", "alt2", "alt3",
		"alt4", "alt5", "alt6", "alt7",
		"alt8", "alt9"
	};
	static const char *ranges_n[] = {
		"0", "1", "2"
	};
	static const char *steps_n[] = {
		"0", "1", "2", "3"
	};
	static const char *alts_n[] = {
		"0",	/* default + 10 alternatives */
		"1", "2", "3", "4",
		"5", "6", "7", "8",
		"9", "10"
	};
	bool is_numeric = !!(flags & SPA_TYPE_NUMERIC);

	switch (type) {
	case SPA_CHOICE_None:
		if (idx == 0)
			return !is_numeric ? "None" : "0";
		break;

	case SPA_CHOICE_Range:
		if (idx < SPA_N_ELEMENTS(ranges))
			return !is_numeric ? ranges[idx] : ranges_n[idx];
		break;

	case SPA_CHOICE_Step:
		if (idx < SPA_N_ELEMENTS(steps))
			return !is_numeric ? steps[idx] : steps_n[idx];
		break;

	case SPA_CHOICE_Enum:
	case SPA_CHOICE_Flags:
		if (idx < SPA_N_ELEMENTS(alts))
			return !is_numeric ? alts[idx] : alts_n[idx];
		break;

	default:
		break;
	}

	return NULL;
}

static const char *
spa_type_get_name(const struct spa_type_info *info, uint32_t type,
		  unsigned int flags, char *buf, size_t bufsz)
{
	const char *str = NULL;

	if (!(flags & SPA_TYPE_NUMERIC))
		str = (flags & SPA_TYPE_FULL) ?
			spa_debug_type_find_name(info, type) :
			spa_debug_type_find_short_name(info, type);
	if (str)
		return str;

	snprintf(buf, bufsz, "0x%08"PRIx32, type);
	return buf;
}

static bool
spa_type_key_eq(const struct spa_type_info *info, uint32_t type, const char *key)
{
	const char *str;

	info = spa_debug_type_find(info, type);
	if (!info)
		return false;

	/* compare against full name first */
	if (!strcmp(key, info->name))
		return true;

	/* try short name now */
	str = rindex(info->name, ':');
	if (str)
		str++;
	if (!str)
		return false;

	return !strcmp(key, str);
}

static bool
spa_choice_key_eq(enum spa_choice_type type, unsigned int idx, const char *key)
{
	const char *ckey = spa_type_get_choice_key(type, idx, 0);

	if (!ckey)
		return false;

	return !strcmp(ckey, key);
}

static void
var_seq_start(struct var_ctx *v, bool is_final, bool is_empty)
{
	if (!is_final)
		return;

	switch (v->fmt) {
	case var_yaml:
		if (is_empty)
			var_printf(v, "[]");

		spa_assert(v->ind_top < SPA_N_ELEMENTS(v->ind_stack));;
		v->ind_stack[v->ind_top++] = v->ind;
		if (v->level > 0 && !v->is_ind)
			v->ind += 4;
		else if (v->is_ind && v->ind == 0) {
			v->ind = 2;	/* kick start special */
			v->is_ind = true;
			var_printf(v, "- ");
		}
		break;
	case var_json:
		if (!is_empty)
			var_printf(v, "[\n");
		else
			var_printf(v, "[]");
		v->ind += 4;
		break;
	case var_cmdline:
	default:
		if (!is_empty) {
			var_printf(v, "[");
			if (v->level <= 1) {
				var_printf(v, "\n");
				v->ind += 4;
			}
		} else
			var_printf(v, "[]");
		break;
	}
}

static void
var_seq_prefix(struct var_ctx *v, bool is_final, bool is_first)
{
	if (!is_final)
		return;

	switch (v->fmt) {
	case var_yaml:
		if (!v->is_ind) {
			var_printf(v, "\n");
			if (v->ind >= 2)
				var_printf(v, "%*s- ", v->ind - 2, "");
		}
		v->is_ind = true;
		break;
	case var_json:
		var_printf(v, "%*s", v->ind, "");
		break;
	case var_cmdline:
	default:
		if (v->level <= 1)
			var_printf(v, "%*s", v->ind, "");
		else
			var_printf(v, "%s", " ");
		break;
	}
}

static void
var_seq_suffix(struct var_ctx *v, bool is_final, bool is_last)
{
	if (!is_final)
		return;

	switch (v->fmt) {
	case var_yaml:
		v->is_ind = false;
		break;
	case var_json:
		var_printf(v, "%s", !is_last ? ",\n" : "\n");
		break;
	case var_cmdline:
	default:
		var_printf(v, "%s", !is_last ? "," : "");
		if (v->level <= 1)
			var_printf(v, "\n");
		break;
	}
}

static void
var_seq_end(struct var_ctx *v, bool is_final, bool is_empty)
{
	if (!is_final)
		return;

	switch (v->fmt) {
	case var_yaml:
		spa_assert(v->ind_top > 0);
		v->ind = v->ind_stack[--v->ind_top];
		v->is_ind = false;
		break;
	case var_json:
		v->ind -= 4;
		if (v->ind < 0)
			v->ind = 0;
		if (!is_empty) {
			var_printf(v, "%*s", v->ind, "");
			var_printf(v, "]");
		}
		break;
	case var_cmdline:
	default:
		if (!is_empty) {
			if (v->level <= 1) {
				v->ind -= 4;
				if (v->ind < 0)
					v->ind = 0;
				var_printf(v, "%*s", v->ind, "");
			} else
				var_printf(v, " ");
			var_printf(v, "]");
		}
		break;
	}
}

static void
var_map_start(struct var_ctx *v, bool is_final, bool is_empty)
{
	if (!is_final)
		return;

	switch (v->fmt) {
	case var_yaml:
		if (is_empty)
			var_printf(v, "{}");

		spa_assert(v->ind_top < SPA_N_ELEMENTS(v->ind_stack));;
		v->ind_stack[v->ind_top++] = v->ind;
		if (v->level > 0 && !v->is_ind)
			v->ind += 4;
		break;
	case var_json:
		if (!is_empty)
			var_printf(v, "{\n");
		else
			var_printf(v, "{}");
		v->ind += 4;
		break;
	case var_cmdline:
	default:
		if (!is_empty) {
			var_printf(v, "{");
			if (v->level <= 1) {
				var_printf(v, "\n");
				v->ind += 4;
			}
		} else
			var_printf(v, "{}");
		break;
	}
}

static void
var_map_prefix(struct var_ctx *v, bool is_final, bool is_first, const char *key)
{
	if (!is_final)
		return;

	switch (v->fmt) {
	case var_yaml:
		if (!v->is_ind) {
			var_printf(v, "\n");
			var_printf(v, "%*s", v->ind, "");
		}
		var_printf(v, "%s: ", quote_if_needed(key));
		v->is_ind = false;
		break;
	case var_json:
		var_printf(v, "%*s", v->ind, "");
		var_printf(v, "\"%s\": ", key);
		break;
	case var_cmdline:
	default:
		if (v->level <= 1)
			var_printf(v, "%*s", v->ind, "");
		else
			var_printf(v, "%s", " ");
		var_printf(v, "%s", key);
		var_printf(v, "=");
		break;
	}
}

static void
var_map_suffix(struct var_ctx *v, bool is_final, bool is_last)
{
	if (!is_final)
		return;

	switch (v->fmt) {
	case var_yaml:
		v->is_ind = false;
		break;
	case var_json:
		var_printf(v, "%s", !is_last ? ",\n" : "\n");
		break;
	case var_cmdline:
	default:
		var_printf(v, "%s", !is_last ? "," : "");
		if (v->level <= 1)
			var_printf(v, "\n");
		break;
	}
}

static void
var_map_end(struct var_ctx *v, bool is_final, bool is_empty)
{
	if (!is_final)
		return;

	switch (v->fmt) {
	case var_yaml:
		spa_assert(v->ind_top > 0);
		v->ind = v->ind_stack[--v->ind_top];
		v->is_ind = false;
		break;
	case var_json:
		v->ind -= 4;
		if (v->ind < 0)
			v->ind = 0;
		if (!is_empty) {
			var_printf(v, "%*s", v->ind, "");
			var_printf(v, "}");
		}
		break;
	case var_cmdline:
	default:
		if (!is_empty) {
			if (v->level <= 1) {
				v->ind -= 4;
				if (v->ind < 0)
					v->ind = 0;
				var_printf(v, "%*s", v->ind, "");
			} else
				var_printf(v, " ");
			var_printf(v, "}");
		}
		break;
	}
}

static const char *
var_scalar_bool(struct var_ctx *v, const struct spa_type_info *info, bool val)
{
	var_printf(v, "%s", val ? "true" : "false");
	return v->buf;
}

static const char *
var_scalar_id(struct var_ctx *v, const struct spa_type_info *info, uint32_t id)
{
	char tbuf[80];
	const char *str;

	str = spa_type_get_name(info, id,
				((v->flags & VF_TYPE_NUMERIC) ? SPA_TYPE_NUMERIC : 0) |
				((v->flags & VF_TYPE_FULL) ? SPA_TYPE_FULL : 0),
				tbuf, sizeof(tbuf));

	switch (v->fmt) {
	case var_yaml:
		var_printf(v, "%s", quote_if_needed(str));
		break;
	case var_json:
		var_printf(v, "\"%s\"", str);
		break;
	default:
		var_printf(v, "%s", quote_if_needed(str));
		break;
	}
	return v->buf;
}

static const char *
var_scalar_int(struct var_ctx *v, const struct spa_type_info *info, int32_t val)
{
	var_printf(v, "%"PRIi32, val);
	return v->buf;
}

static const char *
var_scalar_long(struct var_ctx *v, const struct spa_type_info *info, int64_t val)
{
	var_printf(v, "%"PRIi64, val);
	return v->buf;
}

static const char *
var_scalar_float(struct var_ctx *v, const struct spa_type_info *info, float val)
{
	var_printf(v, "%f", val);
	return v->buf;
}

static const char *
var_scalar_double(struct var_ctx *v, const struct spa_type_info *info, double val)
{
	var_printf(v, "%f", val);
	return v->buf;
}

static const char *
var_scalar_string(struct var_ctx *v, const struct spa_type_info *info, const char *str)
{
	const char *tstr;

	tstr = quote_if_needed(str);
	switch (v->fmt) {
	case var_yaml:
		var_printf(v, "%s", tstr);
		break;
	case var_json:
		if (*tstr != '"')
			var_printf(v, "\"%s\"", str);
		else
			var_printf(v, "%s", tstr);
		break;
	default:
		var_printf(v, "%s", tstr);
		break;
	}
	return v->buf;
}

static const char *
var_scalar_fd(struct var_ctx *v, const struct spa_type_info *info, int val)
{
	var_printf(v, "%d", val);
	return v->buf;
}

static const char *
var_scalar_property_value(struct var_ctx *v, const struct spa_type_info *info, const char *str)
{
	long long ll;
	double d;
	char *end;

	if (!str)
		str = "";

	/* null string */
	if (!*str)
		return var_scalar_string(v, info, str);

	/* try booleans */
	if (!strcmp(str, "true"))
		return var_scalar_bool(v, info, true);
	if (!strcmp(str, "false"))
		return var_scalar_bool(v, info, false);

	/* try integers */
	ll = strtoll(str, &end, 10);
	if (*end == '\0') {
		/* completely valid */
		if (ll < INT32_MIN || ll > INT32_MAX)
			return var_scalar_long(v, info, (int64_t)ll);
		return var_scalar_int(v, info, (int32_t)ll);
	}

	/* try floats */
	d = strtod(str, &end);
	if (*end == '\0') {
		if (d < FLT_MIN || d > FLT_MAX)
			return var_scalar_double(v, info, d);
		return var_scalar_float(v, info, d);
	}

	/* meh, just a string */
	return var_scalar_string(v, info, str);
}

static bool
var_is_final(struct var_ctx *v, const char *var)
{
	return !*var || (var[1] == '\0' && (*var == '.' || *var == '/'));
}

static const char *
var_get(struct var_ctx *v, const char *var,
	const struct spa_type_info *info, uint32_t type,
	const void *body, uint32_t size)
{
	const char *s, *e;
	bool is_final;
	const char *tmp;

	if (!info)
		info = SPA_TYPE_ROOT;

	/* fprintf(stderr, "%s: type=0x%08"PRIx32" (%s) var=%s size=%"PRIu32"\n",
			__func__, info->name, var, size); */

	s = var;
	is_final = var_is_final(v, s);

	switch (type) {
	case SPA_TYPE_Bool:
		if (size < sizeof(uint32_t) || !is_final)
			return NULL;
		return var_scalar_bool(v, info, !!*(const int32_t *)body);

	case SPA_TYPE_Id:
		if (size < sizeof(uint32_t) || !is_final)
			return NULL;

		return var_scalar_id(v, info, *(const int32_t *)body);

	case SPA_TYPE_Int:
		if (size < sizeof(int32_t) || !is_final)
			return NULL;
		return var_scalar_int(v, info, *(const int32_t *)body);

	case SPA_TYPE_Long:
		if (size < sizeof(int64_t) || !is_final)
			return NULL;
		return var_scalar_long(v, info, *(const int64_t *)body);

	case SPA_TYPE_Float:
		if (size < sizeof(float) || !is_final)
			return NULL;
		return var_scalar_float(v, info, *(const float *)body);

	case SPA_TYPE_Double:
		if (size < sizeof(double) || !is_final)
			return NULL;
		return var_scalar_double(v, info, *(const double *)body);

	case SPA_TYPE_String:
		if (size < 1 || !is_final)
			return NULL;
		return var_scalar_string(v, info, (const char *)body);

	case SPA_TYPE_Fd:
		if (size < sizeof(int) || !is_final)
			return NULL;
		return var_scalar_fd(v, info, *(const int *)body);

	case SPA_TYPE_Pointer: {
		const struct spa_pod_pointer_body *b = (const struct spa_pod_pointer_body *)body;

		if (size < sizeof(*b) || !is_final)
			return NULL;

		var_printf(v, "%p", b->value);
		return v->buf;
		}

	case SPA_TYPE_Rectangle: {
		const struct spa_rectangle *r = (const struct spa_rectangle *)body;

		if (size < sizeof(*r) || !is_final)
			return NULL;

		/* XXX should be an object { width=<>, height=<> } */
		var_printf(v, "%"PRIu32"x%"PRIu32, r->width, r->height);
		return v->buf;
		}

	case SPA_TYPE_Fraction: {
		const struct spa_fraction *f = (const struct spa_fraction *)body;

		if (size < sizeof(*f) || !is_final)
			return NULL;

		/* XXX should be an object { num=<>, denom=<> } */
		var_printf(v, "%"PRIu32"/%"PRIu32, f->num, f->denom);
		return v->buf;
		}

	case SPA_TYPE_Bitmap:
		if (!is_final)
			return NULL;
		/* XXX should be a base64 encoded string */
		var_printf(v, "Bitmap");
		return v->buf;

	case SPA_TYPE_Bytes:
		if (!is_final)
			return NULL;
		/* XXX should be a base64 encoded string */
		var_printf(v, "Bytes");
		return v->buf;

	case SPA_TYPE_Array: {
		const struct spa_pod_array_body *b = (const struct spa_pod_array_body *)body;
		const void *p;
		unsigned int i, n, cnt;

		if (size < sizeof(*b))
			return NULL;

		if (!is_final) {
			e = var_get_index(v, s, &n);
			if (!e)
				return NULL;
		} else {
			e = "";
			n = (unsigned int)-1;
		}

		/* count number of items */
		cnt = 0;
		SPA_POD_ARRAY_BODY_FOREACH(b, size, p)
			cnt++;

		var_seq_start(v, is_final, cnt == 0);

		i = 0;
		SPA_POD_ARRAY_BODY_FOREACH(b, size, p) {

			var_seq_prefix(v, is_final, i == 0);
			if (n == i || is_final) {
				v->level++;
				tmp = var_get(v, e, info, b->child.type, p, b->child.size);
				v->level--;
				if (!tmp)
					return NULL;
				if (!is_final)
					break;
			}
			var_seq_suffix(v, is_final, (i + 1) >= cnt);

			i++;
		}

		var_seq_end(v, is_final, cnt == 0);

		return v->buf;
		}

	case SPA_TYPE_Choice: {
		const struct spa_pod_choice_body *b = (const struct spa_pod_choice_body *)body;
		void *p;
		unsigned int i, cnt;
		char key[64];

		if (size < sizeof(*b))
			return NULL;

		if (!is_final) {
			e = var_get_key(v, s, key, sizeof(key));
			if (!e)
				return NULL;
		} else
			e = "";

		cnt = 0;
		SPA_POD_CHOICE_BODY_FOREACH(b, size, p)
			cnt++;

		var_map_start(v, is_final, cnt == 0);

		i = 0;
		SPA_POD_CHOICE_BODY_FOREACH(b, size, p) {

			var_map_prefix(v, is_final, i == 0,
					spa_type_get_choice_key(b->type, i,
						(v->flags & VF_TYPE_NUMERIC) ? SPA_TYPE_NUMERIC : 0));

			if (spa_choice_key_eq(b->type, i, key) || is_final) {
				v->level++;
				tmp = var_get(v, e, info, b->child.type, p, b->child.size);
				v->level--;
				if (!tmp)
					return NULL;

				if (!is_final)
					break;
			}
			var_map_suffix(v, is_final, (i + 1) >= cnt);

			i++;
		}

		var_map_end(v, is_final, cnt == 0);

		return v->buf;
		}

	case SPA_TYPE_Struct: {
		struct spa_pod *b = (struct spa_pod *)body, *p;
		unsigned int i, n, cnt;

		if (size < sizeof(*b))
			return NULL;

		if (!is_final) {
			e = var_get_index(v, s, &n);
			if (!e)
				return NULL;
		} else {
			e = "";
			n = (unsigned int)-1;
		}

		cnt = 0;
		SPA_POD_FOREACH(b, size, p)
			cnt++;

		var_seq_start(v, is_final, cnt == 0);

		i = 0;
		SPA_POD_FOREACH(b, size, p) {

			var_seq_prefix(v, is_final, i == 0);
			if (n == i || is_final) {

				v->level++;
				tmp = var_get(v, e, info, p->type, SPA_POD_BODY(p), p->size);
				v->level--;
				if (!tmp)
					return NULL;

				if (!is_final)
					break;
			}
			var_seq_suffix(v, is_final, (i + 1) >= cnt);

			i++;
		}

		var_seq_end(v, is_final, cnt == 0);

		return v->buf;
		}

	case SPA_TYPE_Sequence: {
		struct spa_pod_sequence_body *b = (struct spa_pod_sequence_body *)body;
		const struct spa_type_info *ii, *ni;
		struct spa_pod_control *c;
		unsigned int i, n, cnt;

		if (size < sizeof(*b))
			return NULL;

		if (!is_final) {
			e = var_get_index(v, s, &n);
			if (!e)
				return NULL;
		} else {
			e = "";
			n = (unsigned int)-1;
		}

		cnt = 0;
		SPA_POD_SEQUENCE_BODY_FOREACH(b, size, c)
			cnt++;

		var_seq_start(v, is_final, cnt == 0);

		i = 0;
		SPA_POD_SEQUENCE_BODY_FOREACH(b, size, c) {

			var_seq_prefix(v, is_final, i == 0);
			if (n == i || is_final) {

				ii = spa_debug_type_find(spa_type_control, c->type);

				ni = ii ? ii->values : NULL;
				if (!ni)
					ni = SPA_TYPE_ROOT;

				v->level++;
				tmp = var_get(v, e, ni, c->value.type,
						SPA_POD_CONTENTS(struct spa_pod_control, c),
						c->value.size);
				v->level--;
				if (!tmp)
					return NULL;
				if (!is_final)
					break;
			}
			var_seq_suffix(v, is_final, (i + 1) >= cnt);

			i++;
		}

		var_seq_end(v, is_final, cnt == 0);

		return v->buf;
		}

	case SPA_TYPE_Object: {
		const struct spa_pod_object_body *b = (const struct spa_pod_object_body *)body;
		struct spa_pod_prop *p;
		const struct spa_type_info *ti, *ii, *ni;
		uint32_t i, cnt;
		char key[64];
		char tbuf[80];

		if (size < sizeof(*b))
			return NULL;

		ti = spa_debug_type_find(info, b->type);
		ii = ti ? spa_debug_type_find(ti->values, 0) : NULL;
		ii = ii ? spa_debug_type_find(ii->values, b->id) : NULL;

		ni = ti ? ti->values : info;

		if (!is_final) {
			e = var_get_key(v, s, key, sizeof(key));
			if (!e)
				return NULL;
		} else {
			e = "";
			key[0] = '\0';
		}

		cnt = 0;
		SPA_POD_OBJECT_BODY_FOREACH(b, size, p)
			cnt++;

		var_map_start(v, is_final, cnt == 0);

		i = 0;
		SPA_POD_OBJECT_BODY_FOREACH(b, size, p) {

			var_map_prefix(v, is_final, i == 0,
					spa_type_get_name(ni, p->key, 
						((v->flags & VF_TYPE_NUMERIC) ? SPA_TYPE_NUMERIC : 0) |
						((v->flags & VF_TYPE_FULL) ? SPA_TYPE_FULL : 0),
						tbuf, sizeof(tbuf)));

			if (is_final || spa_type_key_eq(ni, p->key, key)) {
				ii = spa_debug_type_find(ni, p->key);
				if (ii)
					ii = ii->values;
				if (!ii)
					ii = SPA_TYPE_ROOT;

				v->level++;
				tmp = var_get(v, e,
						ii, p->value.type,
						SPA_POD_CONTENTS(struct spa_pod_prop, p),
						p->value.size);
				v->level--;
				if (!tmp)
					return NULL;
			}
			var_map_suffix(v, is_final, (i + 1) >= cnt);

			i++;
		}

		var_map_end(v, is_final, cnt == 0);

		return v->buf;
		}
		break;
	default:
		break;
	}
	return NULL;
}

const char *
global_param_get(struct global *global, struct var_ctx *v, const char *var)
{
	struct param *p;
	struct param_entry *pe;
	const char *s, *e, *ee, *value, *str;
	unsigned int n_ps, n_pes, i_p, i_pe, n_pe;
	char tbuf[80];
	char key[64];
	bool is_final, is_inner_final;

	if (!global || !v)
		return NULL;

	if (!var)
		var = "";
	s = var;

	is_final = var_is_final(v, var);

	/* everything */
	if (!is_final) {
		s = var;
		e = var_get_key(v, s, key, sizeof(key));
		if (!e)
			return NULL;
		s = e;

		ee = var_get_index(v, s, &n_pe);
		if (ee)
			e = ee;
		else
			n_pe = (unsigned int)-1;
	} else {
		e = "";
		key[0] = '\0';
		n_pe = (unsigned int)-1;
	}

	/* count number of parameters */
	n_ps = 0;
	spa_list_for_each(p, &global->params, link)
		n_ps++;

	var_map_start(v, is_final, n_ps == 0);
	v->level++;

	i_p = 0;
	spa_list_for_each(p, &global->params, link) {

		str = spa_type_get_name(spa_type_param, p->info.id,
				((v->flags & VF_TYPE_NUMERIC) ? SPA_TYPE_NUMERIC : 0) |
				((v->flags & VF_TYPE_FULL) ? SPA_TYPE_FULL : 0),
				tbuf, sizeof(tbuf));

		if (is_final || spa_type_key_eq(spa_type_param, p->info.id, key)) {

			/* count number of parameter entries */
			n_pes = 0;
			spa_list_for_each(pe, &p->entries, link)
				n_pes++;

			/* if an index is provided is shouldn't be larger */
			if (n_pe != (unsigned int)-1 && n_pe >= n_pes)
				return NULL;

			var_map_prefix(v, is_final, i_p == 0, str);

			is_inner_final = is_final || n_pe == (unsigned int)-1;

			if (n_pes > 1)
				var_seq_start(v, is_inner_final, false);
			else if (n_pes == 0)
				var_map_start(v, is_inner_final, true);

			i_pe = 0;
			spa_list_for_each(pe, &p->entries, link) {

				if (n_pes > 1)
					var_seq_prefix(v, is_inner_final, i_pe == 0);

				if (is_inner_final || i_pe == n_pe) {
					v->level++;
					value = var_get(v, e,
							NULL, SPA_POD_TYPE(pe->param),
							SPA_POD_BODY(pe->param),
							SPA_POD_BODY_SIZE(pe->param));
					v->level--;
					if (!value)
						return NULL;

					if (!is_inner_final)
						break;
				}

				if (n_pes > 1)
					var_seq_suffix(v, is_inner_final, (i_pe + 1) >= n_pes);

				i_pe++;
			}

			if (n_pes > 1)
				var_seq_end(v, is_inner_final, false);
			else if (n_pes == 0)
				var_map_end(v, is_inner_final, true);

			var_map_suffix(v, is_final, (i_p + 1) >= n_ps);

			if (!is_final)
				break;
		}

		i_p++;
	}

	v->level--;
	var_map_end(v, is_final, n_ps == 0);

	return v->buf[0] ? v->buf : NULL;
}

static struct spa_dict *
global_props(struct global *global, bool info_property);

const char *
global_property_get(struct global *global, struct var_ctx *v, const char *var, bool info_property)
{
	const char *s, *e, *value;
	unsigned int i, count;
	char key[64];
	bool is_final;
	struct spa_dict *props;
	const struct spa_dict_item *item;

	if (!global || !v)
		return NULL;

	if (!var)
		var = "";
	s = var;

	is_final = var_is_final(v, var);

	/* everything */
	if (!is_final) {
		s = var;
		e = var_get_key(v, s, key, sizeof(key));
		if (!e)
			return NULL;
		s = e;
	} else {
		e = "";
		key[0] = '\0';
	}

	props = global_props(global, info_property);
	count = props ? props->n_items : 0;

	var_map_start(v, is_final, count == 0);
	v->level++;

	if (!count)
		goto skip;

	i = 0;
	spa_dict_for_each(item, props) {

		var_map_prefix(v, is_final, i == 0, item->key);

		if (is_final || !strcmp(key, item->key)) {

			v->level++;
			value = var_scalar_property_value(v, SPA_TYPE_ROOT, item->value);
			v->level--;
			if (!value)
				return NULL;

			if (!is_final)
				break;
		}

		var_map_suffix(v, is_final, (i + 1) >= count);
		i++;
	}
skip:
	v->level--;
	var_map_end(v, is_final, count == 0);

	return v->buf[0] ? v->buf : NULL;
}

/* NOTE: these macros have side effects,
 * so don't get too fancy with the arguments
 */
#define G_COMMA_START(_buf, _s, _e) \
	do { \
		(_s) = (_buf); \
		(_e) = (_buf) + sizeof(_buf); \
	} while(0)

#define G_COMMA_APPEND(_buf, _s, _e, _expr, _str) \
	do { \
		if ((_expr)) \
			(_s) += snprintf((_s), (_e) - (_s), "%s%s", \
				(_s) > (_buf) && (_s)[-1] != ',' ? "," : "", (_str)); \
	} while(0)

#define G_COMMA_END(_buf, _s, _e) \
	do { \
		*(_s) = '\0'; \
	} while(0)

static const char *
global_info_core_get(struct global *global, struct var_ctx *v, const char *var)
{
#define GIC_ID		0
#define GIC_COOKIE	1
#define GIC_USER_NAME	2
#define GIC_HOST_NAME	3
#define GIC_VERSION	4
#define GIC_NAME	5
#define GIC_CHANGE_MASK	6
#define GIC_PROPS	7
	static const char *keys[] = {
		[GIC_ID]		= "id",
		[GIC_COOKIE]		= "cookie",
		[GIC_USER_NAME]		= "user_name",
		[GIC_HOST_NAME]		= "host_name",
		[GIC_VERSION]		= "version",
		[GIC_NAME]		= "name",
		[GIC_CHANGE_MASK]	= "change_mask",
		[GIC_PROPS]		= "props",
	};
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_core_info *info = pd->info;
	const char *s, *e, *value;
	char *ss, *ee;
	unsigned int i, count;
	char tbuf[256];
	char key[64];
	bool is_final;

	if (!var)
		var = "";
	s = var;

	is_final = var_is_final(v, var);

	/* everything */
	if (!is_final) {
		s = var;
		e = var_get_key(v, s, key, sizeof(key));
		if (!e)
			return NULL;
		s = e;
	} else {
		e = "";
		key[0] = '\0';
	}

	count = SPA_N_ELEMENTS(keys);	/* properties, parameters */

	var_map_start(v, is_final, count == 0);
	v->level++;

	for (i = 0; i < count; i++) {

		var_map_prefix(v, is_final, i == 0, keys[i]);

		if (is_final || !strcmp(key, keys[i])) {
			v->level++;

			switch (i) {
			case GIC_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->id);
				break;
			case GIC_COOKIE:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->cookie);
				break;
			case GIC_USER_NAME:
				value = var_scalar_string(v, SPA_TYPE_ROOT, info->user_name);
				break;
			case GIC_HOST_NAME:
				value = var_scalar_string(v, SPA_TYPE_ROOT, info->host_name);
				break;
			case GIC_VERSION:
				value = var_scalar_string(v, SPA_TYPE_ROOT, info->version);
				break;
			case GIC_NAME:
				value = var_scalar_string(v, SPA_TYPE_ROOT, info->name);
				break;
			case GIC_CHANGE_MASK:
				G_COMMA_START(tbuf, ss, ee);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_CORE_CHANGE_MASK_PROPS, keys[GIC_PROPS]);
				G_COMMA_END(tbuf, ss, ee);
				value = var_scalar_string(v, SPA_TYPE_ROOT, tbuf);
				info->change_mask = 0;	/* clear change bits */
				break;
			case GIC_PROPS:
				value = global_property_get(global, v, e, true);
				break;
			default:
				value = NULL;
				break;
			}

			v->level--;
			if (!value)
				return NULL;

			if (!is_final)
				break;
		}

		var_map_suffix(v, is_final, (i + 1) >= count);

	}

	v->level--;
	var_map_end(v, is_final, count == 0);

	return v->buf[0] ? v->buf : NULL;
}

static const char *
global_info_module_get(struct global *global, struct var_ctx *v, const char *var)
{
#define GIM_ID		0
#define GIM_NAME	1
#define GIM_FILENAME	2
#define GIM_ARGS	3
#define GIM_CHANGE_MASK	4
#define GIM_PROPS	5
	static const char *keys[] = {
		[GIM_ID]		= "id",
		[GIM_NAME]		= "name",
		[GIM_FILENAME]		= "filename",
		[GIM_ARGS]		= "args",
		[GIM_CHANGE_MASK]	= "change_mask",
		[GIM_PROPS]		= "props",
	};
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_module_info *info = pd->info;
	const char *s, *e, *value;
	char *ss, *ee;
	unsigned int i, count;
	char tbuf[256];
	char key[64];
	bool is_final;

	if (!var)
		var = "";
	s = var;

	is_final = var_is_final(v, var);

	/* everything */
	if (!is_final) {
		s = var;
		e = var_get_key(v, s, key, sizeof(key));
		if (!e)
			return NULL;
		s = e;
	} else {
		e = "";
		key[0] = '\0';
	}

	count = SPA_N_ELEMENTS(keys);	/* properties, parameters */

	var_map_start(v, is_final, count == 0);
	v->level++;

	for (i = 0; i < count; i++) {

		var_map_prefix(v, is_final, i == 0, keys[i]);

		if (is_final || !strcmp(key, keys[i])) {
			v->level++;

			switch (i) {
			case GIM_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->id);
				break;
			case GIM_NAME:
				value = var_scalar_string(v, SPA_TYPE_ROOT, info->name);
				break;
			case GIM_FILENAME:
				value = var_scalar_string(v, SPA_TYPE_ROOT, info->filename);
				break;
			case GIM_ARGS:
				value = var_scalar_string(v, SPA_TYPE_ROOT, info->args ? : "");
				break;
			case GIM_CHANGE_MASK:
				G_COMMA_START(tbuf, ss, ee);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_MODULE_CHANGE_MASK_PROPS, keys[GIM_PROPS]);
				G_COMMA_END(tbuf, ss, ee);

				value = var_scalar_string(v, SPA_TYPE_ROOT, tbuf);

				info->change_mask = 0;	/* clear change bits */

				break;
			case GIM_PROPS:
				value = global_property_get(global, v, e, true);
				break;
			default:
				value = NULL;
				break;
			}

			v->level--;
			if (!value)
				return NULL;

			if (!is_final)
				break;
		}

		var_map_suffix(v, is_final, (i + 1) >= count);

	}

	v->level--;
	var_map_end(v, is_final, count == 0);

	return v->buf[0] ? v->buf : NULL;
}

static const char *
global_info_device_get(struct global *global, struct var_ctx *v, const char *var)
{
#define GID_ID		0
#define GID_CHANGE_MASK	1
#define GID_PROPS	2
#define GID_PARAMS	3
	static const char *keys[] = {
		[GID_ID]		= "id",
		[GID_CHANGE_MASK]	= "change_mask",
		[GID_PROPS]		= "props",
		[GID_PARAMS]		= "params",
	};
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_device_info *info = pd->info;
	const char *s, *e, *value;
	char *ss, *ee;
	unsigned int i, count;
	char tbuf[256];
	char key[64];
	bool is_final;

	if (!var)
		var = "";
	s = var;

	is_final = var_is_final(v, var);

	/* everything */
	if (!is_final) {
		s = var;
		e = var_get_key(v, s, key, sizeof(key));
		if (!e)
			return NULL;
		s = e;
	} else {
		e = "";
		key[0] = '\0';
	}

	count = SPA_N_ELEMENTS(keys);	/* properties, parameters */

	var_map_start(v, is_final, count == 0);
	v->level++;

	for (i = 0; i < count; i++) {

		var_map_prefix(v, is_final, i == 0, keys[i]);

		if (is_final || !strcmp(key, keys[i])) {
			v->level++;

			switch (i) {
			case GID_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->id);
				break;
			case GID_CHANGE_MASK:
				G_COMMA_START(tbuf, ss, ee);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_DEVICE_CHANGE_MASK_PROPS, keys[GID_PROPS]);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS, keys[GID_PARAMS]);
				G_COMMA_END(tbuf, ss, ee);
				value = var_scalar_string(v, SPA_TYPE_ROOT, tbuf);
				info->change_mask = 0;	/* clear change bits */
				break;
			case GID_PROPS:
				value = global_property_get(global, v, e, true);
				break;
			case GID_PARAMS:
				value = global_param_get(global, v, e);
				break;
			default:
				value = NULL;
				break;
			}

			v->level--;
			if (!value)
				return NULL;

			if (!is_final)
				break;
		}

		var_map_suffix(v, is_final, (i + 1) >= count);

	}

	v->level--;
	var_map_end(v, is_final, count == 0);

	return v->buf[0] ? v->buf : NULL;
}

static const char *
global_info_node_get(struct global *global, struct var_ctx *v, const char *var)
{
#define GIN_ID			0
#define GIN_MAX_INPUT_PORTS	1
#define GIN_MAX_OUTPUT_PORTS	2
#define GIN_N_INPUT_PORTS	3
#define GIN_N_OUTPUT_PORTS	4
#define GIN_STATE		5
#define GIN_ERROR		6
#define GIN_CHANGE_MASK		7
#define GIN_PROPS		8
#define GIN_PARAMS		9
	static const char *keys[] = {
		[GIN_ID]		= "id",
		[GIN_MAX_INPUT_PORTS]	= "max_input_ports",
		[GIN_MAX_OUTPUT_PORTS]	= "max_output_ports",
		[GIN_N_INPUT_PORTS]	= "n_input_ports",
		[GIN_N_OUTPUT_PORTS]	= "n_output_ports",
		[GIN_STATE]		= "state",
		[GIN_ERROR]		= "error",
		[GIN_CHANGE_MASK]	= "change_mask",
		[GIN_PROPS]		= "props",
		[GIN_PARAMS]		= "params",
	};
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_node_info *info = pd->info;
	const char *s, *e, *value, *state;
	char *ss, *ee;
	unsigned int i, count;
	char tbuf[256];
	char key[64];
	bool is_final;

	if (!var)
		var = "";
	s = var;

	is_final = var_is_final(v, var);

	/* everything */
	if (!is_final) {
		s = var;
		e = var_get_key(v, s, key, sizeof(key));
		if (!e)
			return NULL;
		s = e;
	} else {
		e = "";
		key[0] = '\0';
	}

	count = SPA_N_ELEMENTS(keys);	/* properties, parameters */

	var_map_start(v, is_final, count == 0);
	v->level++;

	for (i = 0; i < count; i++) {

		var_map_prefix(v, is_final, i == 0, keys[i]);

		if (is_final || !strcmp(key, keys[i])) {
			v->level++;

			switch (i) {
			case GIN_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->id);
				break;
			case GIN_MAX_INPUT_PORTS:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->max_input_ports);
				break;
			case GIN_MAX_OUTPUT_PORTS:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->max_output_ports);
				break;
			case GIN_N_INPUT_PORTS:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->n_input_ports);
				break;
			case GIN_N_OUTPUT_PORTS:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->n_output_ports);
				break;
			case GIN_STATE:
				switch (info->state) {
				case PW_NODE_STATE_ERROR:
					state = "error";
					break;
				case PW_NODE_STATE_CREATING:
					state = "creating";
					break;
				case PW_NODE_STATE_SUSPENDED:
					state = "suspended";
					break;
				case PW_NODE_STATE_IDLE:
					state = "idle";
					break;
				case PW_NODE_STATE_RUNNING:
					state = "running";
					break;
				default:
					snprintf(tbuf, sizeof(tbuf), "unknown-%d", (int)info->state);
					state = tbuf;
					break;
				}
				value = var_scalar_string(v, SPA_TYPE_ROOT, state);
				break;
			case GIN_ERROR:
				value = var_scalar_string(v, SPA_TYPE_ROOT, info->error ? : "");
				break;
			case GIN_CHANGE_MASK:
				G_COMMA_START(tbuf, ss, ee);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_NODE_CHANGE_MASK_INPUT_PORTS, keys[GIN_N_INPUT_PORTS]);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_NODE_CHANGE_MASK_OUTPUT_PORTS, keys[GIN_N_OUTPUT_PORTS]);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_NODE_CHANGE_MASK_STATE, keys[GIN_STATE]);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_NODE_CHANGE_MASK_PROPS, keys[GIN_PROPS]);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_NODE_CHANGE_MASK_PARAMS, keys[GIN_PARAMS]);
				G_COMMA_END(tbuf, ss, ee);
				value = var_scalar_string(v, SPA_TYPE_ROOT, tbuf);
				info->change_mask = 0;	/* clear change bits */
				break;
			case GIN_PROPS:
				value = global_property_get(global, v, e, true);
				break;
			case GIN_PARAMS:
				value = global_param_get(global, v, e);
				break;
			default:
				value = NULL;
				break;
			}

			v->level--;
			if (!value)
				return NULL;

			if (!is_final)
				break;
		}

		var_map_suffix(v, is_final, (i + 1) >= count);

	}

	v->level--;
	var_map_end(v, is_final, count == 0);

	return v->buf[0] ? v->buf : NULL;
}

static const char *
global_info_port_get(struct global *global, struct var_ctx *v, const char *var)
{
#define GIP_ID		0
#define GIP_DIRECTION	1
#define GIP_CHANGE_MASK	2
#define GIP_PROPS	3
#define GIP_PARAMS	4
	static const char *keys[] = {
		[GIP_ID]		= "id",
		[GIP_DIRECTION]		= "direction",
		[GIP_CHANGE_MASK]	= "change_mask",
		[GIP_PROPS]		= "props",
		[GIP_PARAMS]		= "params",
	};
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_port_info *info = pd->info;
	const char *s, *e, *value, *direction;
	char *ss, *ee;
	unsigned int i, count;
	char tbuf[256];
	char key[64];
	bool is_final;

	if (!var)
		var = "";
	s = var;

	is_final = var_is_final(v, var);

	/* everything */
	if (!is_final) {
		s = var;
		e = var_get_key(v, s, key, sizeof(key));
		if (!e)
			return NULL;
		s = e;
	} else {
		e = "";
		key[0] = '\0';
	}

	count = SPA_N_ELEMENTS(keys);	/* properties, parameters */

	var_map_start(v, is_final, count == 0);
	v->level++;

	for (i = 0; i < count; i++) {

		var_map_prefix(v, is_final, i == 0, keys[i]);

		if (is_final || !strcmp(key, keys[i])) {
			v->level++;

			switch (i) {
			case GIP_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->id);
				break;
			case GIP_DIRECTION:
				switch (info->direction) {
				case PW_DIRECTION_INPUT:
					direction = "input";
					break;
				case PW_DIRECTION_OUTPUT:
					direction = "output";
					break;
				default:
					snprintf(tbuf, sizeof(tbuf), "unknown-%d", (int)info->direction);
					direction = tbuf;
					break;
				}
				value = var_scalar_string(v, SPA_TYPE_ROOT, direction);
				break;
			case GIP_CHANGE_MASK:
				G_COMMA_START(tbuf, ss, ee);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_PORT_CHANGE_MASK_PROPS, keys[GIP_PROPS]);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_PORT_CHANGE_MASK_PARAMS, keys[GIP_PARAMS]);
				G_COMMA_END(tbuf, ss, ee);
				value = var_scalar_string(v, SPA_TYPE_ROOT, tbuf);
				info->change_mask = 0;	/* clear change bits */
				break;
			case GIP_PROPS:
				value = global_property_get(global, v, e, true);
				break;
			case GIP_PARAMS:
				value = global_param_get(global, v, e);
				break;
			default:
				value = NULL;
				break;
			}

			v->level--;
			if (!value)
				return NULL;

			if (!is_final)
				break;
		}

		var_map_suffix(v, is_final, (i + 1) >= count);

	}

	v->level--;
	var_map_end(v, is_final, count == 0);

	return v->buf[0] ? v->buf : NULL;
}

static const char *
global_info_factory_get(struct global *global, struct var_ctx *v, const char *var)
{
#define GIF_ID		0
#define GIF_NAME	1
#define GIF_TYPE	2
#define GIF_VERSION	3
#define GIF_CHANGE_MASK	4
#define GIF_PROPS	5
	static const char *keys[] = {
		[GIF_ID]		= "id",
		[GIF_NAME]		= "name",
		[GIF_TYPE]		= "type",
		[GIF_VERSION]		= "version",
		[GIF_CHANGE_MASK]	= "change_mask",
		[GIF_PROPS]		= "props",
	};
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_factory_info *info = pd->info;
	const char *s, *e, *value;
	char *ss, *ee;
	unsigned int i, count;
	char tbuf[256];
	char key[64];
	bool is_final;

	if (!var)
		var = "";
	s = var;

	is_final = var_is_final(v, var);

	/* everything */
	if (!is_final) {
		s = var;
		e = var_get_key(v, s, key, sizeof(key));
		if (!e)
			return NULL;
		s = e;
	} else {
		e = "";
		key[0] = '\0';
	}

	count = SPA_N_ELEMENTS(keys);	/* properties, parameters */

	var_map_start(v, is_final, count == 0);
	v->level++;

	for (i = 0; i < count; i++) {

		var_map_prefix(v, is_final, i == 0, keys[i]);

		if (is_final || !strcmp(key, keys[i])) {
			v->level++;

			switch (i) {
			case GIF_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->id);
				break;
			case GIF_NAME:
				value = var_scalar_string(v, SPA_TYPE_ROOT, info->name);
				break;
			case GIF_TYPE:
				value = var_scalar_string(v, SPA_TYPE_ROOT, info->type);
				break;
			case GIF_VERSION:
				value = var_scalar_int(v, SPA_TYPE_ROOT, info->version);
				break;
			case GIF_CHANGE_MASK:
				G_COMMA_START(tbuf, ss, ee);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_FACTORY_CHANGE_MASK_PROPS, keys[GIF_PROPS]);
				G_COMMA_END(tbuf, ss, ee);
				value = var_scalar_string(v, SPA_TYPE_ROOT, tbuf);
				info->change_mask = 0;	/* clear change bits */
				break;
			case GIF_PROPS:
				value = global_property_get(global, v, e, true);
				break;
			default:
				value = NULL;
				break;
			}

			v->level--;
			if (!value)
				return NULL;

			if (!is_final)
				break;
		}

		var_map_suffix(v, is_final, (i + 1) >= count);

	}

	v->level--;
	var_map_end(v, is_final, count == 0);

	return v->buf[0] ? v->buf : NULL;
}

static const char *
global_info_client_get(struct global *global, struct var_ctx *v, const char *var)
{
#define GICL_ID			0
#define GICL_CHANGE_MASK	1
#define GICL_PROPS		2
	static const char *keys[] = {
		[GICL_ID]		= "id",
		[GICL_CHANGE_MASK]	= "change_mask",
		[GICL_PROPS]		= "props",
	};
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_client_info *info = pd->info;
	const char *s, *e, *value;
	char *ss, *ee;
	unsigned int i, count;
	char tbuf[256];
	char key[64];
	bool is_final;

	if (!var)
		var = "";
	s = var;

	is_final = var_is_final(v, var);

	/* everything */
	if (!is_final) {
		s = var;
		e = var_get_key(v, s, key, sizeof(key));
		if (!e)
			return NULL;
		s = e;
	} else {
		e = "";
		key[0] = '\0';
	}

	count = SPA_N_ELEMENTS(keys);	/* properties, parameters */

	var_map_start(v, is_final, count == 0);
	v->level++;

	for (i = 0; i < count; i++) {

		var_map_prefix(v, is_final, i == 0, keys[i]);

		if (is_final || !strcmp(key, keys[i])) {
			v->level++;

			switch (i) {
			case GICL_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->id);
				break;
			case GICL_CHANGE_MASK:
				G_COMMA_START(tbuf, ss, ee);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_CLIENT_CHANGE_MASK_PROPS, keys[GICL_PROPS]);
				G_COMMA_END(tbuf, ss, ee);
				value = var_scalar_string(v, SPA_TYPE_ROOT, tbuf);
				info->change_mask = 0;	/* clear change bits */
				break;
			case GICL_PROPS:
				value = global_property_get(global, v, e, true);
				break;
			default:
				value = NULL;
				break;
			}

			v->level--;
			if (!value)
				return NULL;

			if (!is_final)
				break;
		}

		var_map_suffix(v, is_final, (i + 1) >= count);

	}

	v->level--;
	var_map_end(v, is_final, count == 0);

	return v->buf[0] ? v->buf : NULL;
}

static const char *
global_info_link_get(struct global *global, struct var_ctx *v, const char *var)
{
#define GIL_ID			0
#define GIL_OUTPUT_NODE_ID	1
#define GIL_OUTPUT_PORT_ID	2
#define GIL_INPUT_NODE_ID	3
#define GIL_INPUT_PORT_ID	4
#define GIL_STATE		5
#define GIL_ERROR		6
#define GIL_FORMAT		7
#define GIL_CHANGE_MASK		8
#define GIL_PROPS		9
	static const char *keys[] = {
		[GIL_ID]		= "id",
		[GIL_OUTPUT_NODE_ID]	= "output_node_id",
		[GIL_OUTPUT_PORT_ID]	= "output_port_id",
		[GIL_INPUT_NODE_ID]	= "input_node_id",
		[GIL_INPUT_PORT_ID]	= "input_port_id",
		[GIL_STATE]		= "state",
		[GIL_ERROR]		= "error",
		[GIL_FORMAT]		= "format",
		[GIL_CHANGE_MASK]	= "change_mask",
		[GIL_PROPS]		= "props",
	};
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_link_info *info = pd->info;
	const char *s, *e, *value, *state;
	char *ss, *ee;
	unsigned int i, count;
	char tbuf[256];
	char key[64];
	bool is_final;

	if (!var)
		var = "";
	s = var;

	is_final = var_is_final(v, var);

	/* everything */
	if (!is_final) {
		s = var;
		e = var_get_key(v, s, key, sizeof(key));
		if (!e)
			return NULL;
		s = e;
	} else {
		e = "";
		key[0] = '\0';
	}

	count = SPA_N_ELEMENTS(keys);	/* properties, parameters */

	var_map_start(v, is_final, count == 0);
	v->level++;

	for (i = 0; i < count; i++) {

		var_map_prefix(v, is_final, i == 0, keys[i]);

		if (is_final || !strcmp(key, keys[i])) {
			v->level++;

			switch (i) {
			case GIL_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->id);
				break;
			case GIL_OUTPUT_NODE_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->output_node_id);
				break;
			case GIL_OUTPUT_PORT_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->output_port_id);
				break;
			case GIL_INPUT_NODE_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->input_node_id);
				break;
			case GIL_INPUT_PORT_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->input_port_id);
				break;
			case GIL_STATE:
				switch (info->state) {
				case PW_LINK_STATE_ERROR:
					state = "error";
					break;
				case PW_LINK_STATE_UNLINKED:
					state = "unlinked";
					break;
				case PW_LINK_STATE_INIT:
					state = "init";
					break;
				case PW_LINK_STATE_NEGOTIATING:
					state = "negotiating";
					break;
				case PW_LINK_STATE_ALLOCATING:
					state = "allocating";
					break;
				case PW_LINK_STATE_PAUSED:
					state = "paused";
					break;
				default:
					snprintf(tbuf, sizeof(tbuf), "unknown-%d", (int)info->state);
					state = tbuf;
					break;
				}
				value = var_scalar_string(v, SPA_TYPE_ROOT, state);
				break;
			case GIL_ERROR:
				value = var_scalar_string(v, SPA_TYPE_ROOT, info->error ? : "");
				break;
			case GIL_FORMAT:
				if (info->format) {
					value = var_get(v, e,
							NULL, SPA_POD_TYPE(info->format),
							SPA_POD_BODY(info->format),
							SPA_POD_BODY_SIZE(info->format));
				} else
					value = var_scalar_string(v, SPA_TYPE_ROOT, "");
				break;
			case GIL_CHANGE_MASK:
				G_COMMA_START(tbuf, ss, ee);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_LINK_CHANGE_MASK_STATE, keys[GIL_STATE]);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_LINK_CHANGE_MASK_FORMAT, keys[GIL_FORMAT]);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_LINK_CHANGE_MASK_PROPS, keys[GIL_PROPS]);
				G_COMMA_END(tbuf, ss, ee);
				value = var_scalar_string(v, SPA_TYPE_ROOT, tbuf);
				info->change_mask = 0;	/* clear change bits */
				break;
			case GIL_PROPS:
				value = global_property_get(global, v, e, true);
				break;
			default:
				value = NULL;
				break;
			}

			v->level--;
			if (!value)
				return NULL;

			if (!is_final)
				break;
		}

		var_map_suffix(v, is_final, (i + 1) >= count);

	}

	v->level--;
	var_map_end(v, is_final, count == 0);

	return v->buf[0] ? v->buf : NULL;
}

static const char *
global_info_session_get(struct global *global, struct var_ctx *v, const char *var)
{
#define GIS_VERSION	0
#define GIS_ID		1
#define GIS_CHANGE_MASK	2
#define GIS_PROPS	3
#define GIS_PARAMS	4
	static const char *keys[] = {
		[GIS_VERSION]		= "version",
		[GIS_ID]		= "id",
		[GIS_CHANGE_MASK]	= "change_mask",
		[GIS_PROPS]		= "props",
		[GIS_PARAMS]		= "params",
	};
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_session_info *info = pd->info;
	const char *s, *e, *value;
	char *ss, *ee;
	unsigned int i, count;
	char tbuf[256];
	char key[64];
	bool is_final;

	if (!var)
		var = "";
	s = var;

	is_final = var_is_final(v, var);

	/* everything */
	if (!is_final) {
		s = var;
		e = var_get_key(v, s, key, sizeof(key));
		if (!e)
			return NULL;
		s = e;
	} else {
		e = "";
		key[0] = '\0';
	}

	count = SPA_N_ELEMENTS(keys);	/* properties, parameters */

	var_map_start(v, is_final, count == 0);
	v->level++;

	for (i = 0; i < count; i++) {

		var_map_prefix(v, is_final, i == 0, keys[i]);

		if (is_final || !strcmp(key, keys[i])) {
			v->level++;

			switch (i) {
			case GIS_VERSION:
				value = var_scalar_int(v, SPA_TYPE_ROOT, info->version);
				break;
			case GIS_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->id);
				break;
			case GIS_CHANGE_MASK:
				G_COMMA_START(tbuf, ss, ee);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_SESSION_CHANGE_MASK_PROPS, keys[GIS_PROPS]);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_SESSION_CHANGE_MASK_PARAMS, keys[GIS_PARAMS]);
				G_COMMA_END(tbuf, ss, ee);
				value = var_scalar_string(v, SPA_TYPE_ROOT, tbuf);
				info->change_mask = 0;	/* clear change bits */
				break;
			case GIS_PROPS:
				value = global_property_get(global, v, e, true);
				break;
			case GIS_PARAMS:
				value = global_param_get(global, v, e);
				break;
			default:
				value = NULL;
				break;
			}

			v->level--;
			if (!value)
				return NULL;

			if (!is_final)
				break;
		}

		var_map_suffix(v, is_final, (i + 1) >= count);

	}

	v->level--;
	var_map_end(v, is_final, count == 0);

	return v->buf[0] ? v->buf : NULL;
}

static const char *
global_info_endpoint_get(struct global *global, struct var_ctx *v, const char *var)
{
#define GIE_VERSION		0
#define GIE_ID			1
#define GIE_NAME		2
#define GIE_MEDIA_CLASS		3
#define GIE_DIRECTION		4
#define GIE_FLAGS		5
#define GIE_N_STREAMS		6
#define GIE_SESSION_ID		7
#define GIE_CHANGE_MASK		8
#define GIE_PROPS		9
#define GIE_PARAMS		10
	static const char *keys[] = {
		[GIE_VERSION]		= "version",
		[GIE_ID]		= "id",
		[GIE_NAME]		= "name",
		[GIE_MEDIA_CLASS]	= "media_class",
		[GIE_DIRECTION]		= "direction",
		[GIE_FLAGS]		= "flags",
		[GIE_N_STREAMS]		= "n_streams",
		[GIE_SESSION_ID]	= "session_id",
		[GIE_CHANGE_MASK]	= "change_mask",
		[GIE_PROPS]		= "props",
		[GIE_PARAMS]		= "params",
	};
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_endpoint_info *info = pd->info;
	const char *s, *e, *value, *direction;
	char *ss, *ee;
	unsigned int i, count;
	char tbuf[256];
	char key[64];
	bool is_final;

	if (!var)
		var = "";
	s = var;

	is_final = var_is_final(v, var);

	/* everything */
	if (!is_final) {
		s = var;
		e = var_get_key(v, s, key, sizeof(key));
		if (!e)
			return NULL;
		s = e;
	} else {
		e = "";
		key[0] = '\0';
	}

	count = SPA_N_ELEMENTS(keys);	/* properties, parameters */

	var_map_start(v, is_final, count == 0);
	v->level++;

	for (i = 0; i < count; i++) {

		var_map_prefix(v, is_final, i == 0, keys[i]);

		if (is_final || !strcmp(key, keys[i])) {
			v->level++;

			switch (i) {
			case GIE_VERSION:
				value = var_scalar_int(v, SPA_TYPE_ROOT, info->version);
				break;
			case GIE_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->id);
				break;
			case GIE_NAME:
				value = var_scalar_string(v, SPA_TYPE_ROOT, info->name);
				break;
			case GIE_MEDIA_CLASS:
				value = var_scalar_string(v, SPA_TYPE_ROOT, info->media_class);
				break;
			case GIE_DIRECTION:
				switch (info->direction) {
				case PW_DIRECTION_INPUT:
					direction = "input";
					break;
				case PW_DIRECTION_OUTPUT:
					direction = "output";
					break;
				default:
					snprintf(tbuf, sizeof(tbuf), "unknown-%d", (int)info->direction);
					direction = tbuf;
					break;
				}
				value = var_scalar_string(v, SPA_TYPE_ROOT, direction);
				break;
			case GIE_FLAGS:
				ss = tbuf;
				ee = tbuf + sizeof(tbuf);

				ss += snprintf(ss, ee - ss, "%s%s",
					ss > tbuf && ss[-1] != ',' ? "," : "",
					(info->flags & PW_ENDPOINT_FLAG_PROVIDES_SESSION) ? "PROVIDES_SESSION" : "");

				value = var_scalar_string(v, SPA_TYPE_ROOT, tbuf);
				break;
			case GIE_N_STREAMS:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->n_streams);
				break;
			case GIE_SESSION_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->session_id);
				break;
			case GIE_CHANGE_MASK:
				G_COMMA_START(tbuf, ss, ee);
				G_COMMA_APPEND(tbuf, ss, ee,info->change_mask & PW_ENDPOINT_CHANGE_MASK_STREAMS, keys[GIE_N_STREAMS]);
				G_COMMA_APPEND(tbuf, ss, ee,info->change_mask & PW_ENDPOINT_CHANGE_MASK_SESSION, keys[GIE_SESSION_ID]);
				G_COMMA_APPEND(tbuf, ss, ee,info->change_mask & PW_ENDPOINT_CHANGE_MASK_PROPS, keys[GIE_PROPS]);
				G_COMMA_APPEND(tbuf, ss, ee,info->change_mask & PW_ENDPOINT_CHANGE_MASK_PARAMS, keys[GIE_PARAMS]);
				G_COMMA_END(tbuf, ss, ee);
				value = var_scalar_string(v, SPA_TYPE_ROOT, tbuf);
				info->change_mask = 0;	/* clear change bits */
				break;
			case GIE_PROPS:
				value = global_property_get(global, v, e, true);
				break;
			case GIE_PARAMS:
				value = global_param_get(global, v, e);
				break;
			default:
				value = NULL;
				break;
			}

			v->level--;
			if (!value)
				return NULL;

			if (!is_final)
				break;
		}

		var_map_suffix(v, is_final, (i + 1) >= count);

	}

	v->level--;
	var_map_end(v, is_final, count == 0);

	return v->buf[0] ? v->buf : NULL;
}

static const char *
global_info_endpoint_stream_get(struct global *global, struct var_ctx *v, const char *var)
{
#define GIES_VERSION		0
#define GIES_ID			1
#define GIES_ENDPOINT_ID	2
#define GIES_NAME		3
#define GIES_LINK_PARAMS	4
#define GIES_CHANGE_MASK	5
#define GIES_PROPS		6
#define GIES_PARAMS		7
	static const char *keys[] = {
		[GIES_VERSION]		= "version",
		[GIES_ID]		= "id",
		[GIES_ENDPOINT_ID]	= "endpoint_id",
		[GIES_NAME]		= "name",
		[GIES_LINK_PARAMS]	= "link_params",
		[GIES_CHANGE_MASK]	= "change_mask",
		[GIES_PROPS]		= "props",
		[GIES_PARAMS]		= "params",
	};
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_endpoint_stream_info *info = pd->info;
	const char *s, *e, *value;
	char *ss, *ee;
	unsigned int i, count;
	char tbuf[256];
	char key[64];
	bool is_final;

	if (!var)
		var = "";
	s = var;

	is_final = var_is_final(v, var);

	/* everything */
	if (!is_final) {
		s = var;
		e = var_get_key(v, s, key, sizeof(key));
		if (!e)
			return NULL;
		s = e;
	} else {
		e = "";
		key[0] = '\0';
	}

	count = SPA_N_ELEMENTS(keys);	/* properties, parameters */

	var_map_start(v, is_final, count == 0);
	v->level++;

	for (i = 0; i < count; i++) {

		var_map_prefix(v, is_final, i == 0, keys[i]);

		if (is_final || !strcmp(key, keys[i])) {
			v->level++;

			switch (i) {
			case GIES_VERSION:
				value = var_scalar_int(v, SPA_TYPE_ROOT, info->version);
				break;
			case GIES_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->id);
				break;
			case GIES_ENDPOINT_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->endpoint_id);
				break;
			case GIES_NAME:
				value = var_scalar_string(v, SPA_TYPE_ROOT, info->name);
				break;
			case GIES_LINK_PARAMS:
				if (info->link_params) {
					value = var_get(v, e,
							NULL, SPA_POD_TYPE(info->link_params),
							SPA_POD_BODY(info->link_params),
							SPA_POD_BODY_SIZE(info->link_params));
				} else
					value = var_scalar_string(v, SPA_TYPE_ROOT, "");
				break;
			case GIES_CHANGE_MASK:
				G_COMMA_START(tbuf, ss, ee);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_ENDPOINT_STREAM_CHANGE_MASK_LINK_PARAMS, keys[GIES_LINK_PARAMS]);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_ENDPOINT_STREAM_CHANGE_MASK_PROPS, keys[GIES_PROPS]);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_ENDPOINT_STREAM_CHANGE_MASK_PARAMS, keys[GIES_PARAMS]);
				G_COMMA_END(tbuf, ss, ee);
				value = var_scalar_string(v, SPA_TYPE_ROOT, tbuf);
				info->change_mask = 0;	/* clear change bits */
				break;
			case GIES_PROPS:
				value = global_property_get(global, v, e, true);
				break;
			case GIES_PARAMS:
				value = global_param_get(global, v, e);
				break;
			default:
				value = NULL;
				break;
			}

			v->level--;
			if (!value)
				return NULL;

			if (!is_final)
				break;
		}

		var_map_suffix(v, is_final, (i + 1) >= count);

	}

	v->level--;
	var_map_end(v, is_final, count == 0);

	return v->buf[0] ? v->buf : NULL;
}

static const char *
global_info_endpoint_link_get(struct global *global, struct var_ctx *v, const char *var)
{
#define GIEL_VERSION		0
#define GIEL_ID			1
#define GIEL_SESSION_ID		2
#define GIEL_OUTPUT_ENDPOINT_ID	3
#define GIEL_OUTPUT_STREAM_ID	4
#define GIEL_INPUT_ENDPOINT_ID	5
#define GIEL_INPUT_STREAM_ID	6
#define GIEL_STATE		7
#define GIEL_ERROR		8
#define GIEL_CHANGE_MASK	9
#define GIEL_PROPS		10
#define GIEL_PARAMS		11
	static const char *keys[] = {
		[GIEL_VERSION]			= "version",
		[GIEL_ID]			= "id",
		[GIEL_SESSION_ID]		= "session_id",
		[GIEL_OUTPUT_ENDPOINT_ID]	= "output_endpoint_id",
		[GIEL_OUTPUT_STREAM_ID]		= "output_stream_id",
		[GIEL_INPUT_ENDPOINT_ID]	= "input_endpoint_id",
		[GIEL_INPUT_STREAM_ID]		= "input_stream_id",
		[GIEL_STATE]			= "state",
		[GIEL_ERROR]			= "error",
		[GIEL_CHANGE_MASK]		= "change_mask",
		[GIEL_PROPS]			= "props",
		[GIEL_PARAMS]			= "params",
	};
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_endpoint_link_info *info = pd->info;
	const char *s, *e, *value, *state;
	char *ss, *ee;
	unsigned int i, count;
	char tbuf[256];
	char key[64];
	bool is_final;

	if (!var)
		var = "";
	s = var;

	is_final = var_is_final(v, var);

	/* everything */
	if (!is_final) {
		s = var;
		e = var_get_key(v, s, key, sizeof(key));
		if (!e)
			return NULL;
		s = e;
	} else {
		e = "";
		key[0] = '\0';
	}

	count = SPA_N_ELEMENTS(keys);	/* properties, parameters */

	var_map_start(v, is_final, count == 0);
	v->level++;

	for (i = 0; i < count; i++) {

		var_map_prefix(v, is_final, i == 0, keys[i]);

		if (is_final || !strcmp(key, keys[i])) {
			v->level++;

			switch (i) {
			case GIEL_VERSION:
				value = var_scalar_int(v, SPA_TYPE_ROOT, info->version);
				break;
			case GIEL_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->id);
				break;
			case GIEL_SESSION_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->session_id);
				break;
			case GIEL_OUTPUT_ENDPOINT_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->output_endpoint_id);
				break;
			case GIEL_OUTPUT_STREAM_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->output_stream_id);
				break;
			case GIEL_INPUT_ENDPOINT_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->input_endpoint_id);
				break;
			case GIEL_INPUT_STREAM_ID:
				value = var_scalar_long(v, SPA_TYPE_ROOT, info->input_stream_id);
				break;
			case GIEL_STATE:
				switch (info->state) {
				case PW_ENDPOINT_LINK_STATE_ERROR:
					state = "error";
					break;
				case PW_ENDPOINT_LINK_STATE_PREPARING:
					state = "preparing";
					break;
				case PW_ENDPOINT_LINK_STATE_INACTIVE:
					state = "inactive";
					break;
				case PW_ENDPOINT_LINK_STATE_ACTIVE:
					state = "active";
					break;
				default:
					snprintf(tbuf, sizeof(tbuf), "unknown-%d", (int)info->state);
					state = tbuf;
					break;
				}
				value = var_scalar_string(v, SPA_TYPE_ROOT, state);
				break;
			case GIEL_ERROR:
				value = var_scalar_string(v, SPA_TYPE_ROOT, info->error ? : "");
				break;
			case GIEL_CHANGE_MASK:
				G_COMMA_START(tbuf, ss, ee);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_ENDPOINT_LINK_CHANGE_MASK_STATE, keys[GIEL_STATE]);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_ENDPOINT_LINK_CHANGE_MASK_PROPS, keys[GIEL_PROPS]);
				G_COMMA_APPEND(tbuf, ss, ee, info->change_mask & PW_ENDPOINT_LINK_CHANGE_MASK_PARAMS, keys[GIEL_PARAMS]);
				G_COMMA_END(tbuf, ss, ee);
				value = var_scalar_string(v, SPA_TYPE_ROOT, tbuf);
				info->change_mask = 0;	/* clear change bits */
				break;
			case GIEL_PROPS:
				value = global_property_get(global, v, e, true);
				break;
			case GIEL_PARAMS:
				value = global_param_get(global, v, e);
				break;
			default:
				value = NULL;
				break;
			}

			v->level--;
			if (!value)
				return NULL;

			if (!is_final)
				break;
		}

		var_map_suffix(v, is_final, (i + 1) >= count);

	}

	v->level--;
	var_map_end(v, is_final, count == 0);

	return v->buf[0] ? v->buf : NULL;
}
const char *
global_info_get(struct global *global, struct var_ctx *v, const char *var)
{
	if (!global || !v)
		return NULL;

	if (!strcmp(global->type, PW_TYPE_INTERFACE_Core))
		return global_info_core_get(global, v, var);
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Module))
		return global_info_module_get(global, v, var);
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Device))
		return global_info_device_get(global, v, var);
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Node))
		return global_info_node_get(global, v, var);
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Port))
		return global_info_port_get(global, v, var);
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Factory))
		return global_info_factory_get(global, v, var);
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Client))
		return global_info_client_get(global, v, var);
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Link))
		return global_info_link_get(global, v, var);
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Session))
		return global_info_session_get(global, v, var);
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Endpoint))
		return global_info_endpoint_get(global, v, var);
	if (!strcmp(global->type, PW_TYPE_INTERFACE_EndpointStream))
		return global_info_endpoint_stream_get(global, v, var);
	if (!strcmp(global->type, PW_TYPE_INTERFACE_EndpointStream))
		return global_info_endpoint_link_get(global, v, var);

	return NULL;
}

const char *
global_graph_get(struct global *global, struct var_ctx *v, const char *var)
{
	const char *s, *e, *value;
	unsigned int i, count;
	char tbuf[80];
	char key[64];
	bool is_final;
#define G_ID		0
#define G_TYPE		1
#define G_PERMISSIONS	2
#define G_VERSION	3
#define G_INFO		4
#define G_PROPERTIES	5
	static const char *keys[] = {
		[G_ID]		= "id",
		[G_TYPE]	= "type",
		[G_PERMISSIONS]	= "permissions",
		[G_VERSION]	= "version",
		[G_INFO]	= "info",
		[G_PROPERTIES]	= "properties",
	};

	if (!global || !v)
		return NULL;

	if (!var)
		var = "";
	s = var;

	is_final = var_is_final(v, var);

	/* everything */
	if (!is_final) {
		s = var;
		e = var_get_key(v, s, key, sizeof(key));
		if (!e)
			return NULL;
		s = e;
	} else {
		e = "";
		key[0] = '\0';
	}

	count = SPA_N_ELEMENTS(keys);	/* properties, parameters */

	var_map_start(v, is_final, count == 0);
	v->level++;

	for (i = 0; i < count; i++) {

		var_map_prefix(v, is_final, i == 0, keys[i]);

		if (is_final || !strcmp(key, keys[i])) {
			v->level++;

			switch (i) {
			case G_ID:
				value = var_scalar_int(v, SPA_TYPE_ROOT, global->id);
				break;
			case G_TYPE:
				value = var_scalar_string(v, SPA_TYPE_ROOT, global->type);
				break;
			case G_PERMISSIONS:
				snprintf(tbuf, sizeof(tbuf), "%c%c%c",
						global->permissions & PW_PERM_R ? 'r' : '-',
						global->permissions & PW_PERM_W ? 'w' : '-',
						global->permissions & PW_PERM_X ? 'x' : '-');
				value = var_scalar_string(v, SPA_TYPE_ROOT, tbuf);
				break;
			case G_VERSION:
				value = var_scalar_int(v, SPA_TYPE_ROOT, global->version);
				break;
			case G_INFO:
				value = global_info_get(global, v, e);
				break;
			case G_PROPERTIES:
				value = global_property_get(global, v, e, false);
				break;
			default:
				value = NULL;
				break;
			}

			v->level--;
			if (!value)
				return NULL;

			if (!is_final)
				break;
		}

		var_map_suffix(v, is_final, (i + 1) >= count);

	}

	v->level--;
	var_map_end(v, is_final, count == 0);

	return v->buf[0] ? v->buf : NULL;
}

const char *
remote_graph_get(struct remote_data *rd, struct var_ctx *v, const char *var)
{
	const char *s, *e, *value;
	unsigned int i, n, cnt;
	struct global *global;
	bool is_final;

	if (!rd || !v)
		return NULL;

	if (!var)
		var = "";
	s = var;

	is_final = var_is_final(v, var);

	/* everything */
	if (!is_final) {
		e = var_get_index(v, s, &n);
		if (!e)
			return NULL;
	} else {
		e = "";
		n = (unsigned int)-1;
	}

	/* count globals namespace */
	cnt = pw_map_get_size(&rd->globals);

	var_seq_start(v, is_final, cnt == 0);
	v->level++;

	/* iterate now */
	for (i = 0; i < cnt; i++) {
		global = remote_global(rd, i);

		var_seq_prefix(v, is_final, i == 0);

		if (n == i || is_final) {
			if (global) {
				value = global_graph_get(global, v, e);
				if (!value)
					return NULL;
			} else {
				var_map_start(v, true, true);
				var_map_end(v, true, true);
			}
		}

		var_seq_suffix(v, is_final, (i + 1) >= cnt);
	}

	v->level--;
	var_seq_end(v, is_final, cnt == 0);

	return v->buf[0] ? v->buf : NULL;
}

static void enum_single_param(struct global *global, struct param *p, bool is_short)
{
	struct var_ctx v;
	const char *str, *value;
	struct param_entry *pe;
	char buf[4096];

	if (!is_short) {
		fprintf(stdout, "%c%c%c id=%"PRIu32" (%s)\n",
				(p->info.flags & SPA_PARAM_INFO_SERIAL) ? 's' : '-',
				(p->info.flags & SPA_PARAM_INFO_READ)   ? 'r' : '-',
				(p->info.flags & SPA_PARAM_INFO_WRITE)  ? 'w' : '-',
				p->info.id,
				spa_debug_type_find_name(spa_type_param, p->info.id));

		spa_list_for_each(pe, &p->entries, link) {
			if (spa_pod_is_object_type(pe->param, SPA_TYPE_OBJECT_Format))
				spa_debug_format(2, NULL, pe->param);
			else
				spa_debug_pod(2, NULL, pe->param);
		}
	} else {

		str = spa_debug_type_find_short_name(spa_type_param, p->info.id);
		spa_assert(str);

		var_init(&v, buf, sizeof(buf), var_cmdline, 0, 0, 0);
		value = global_param_get(global, &v, str);
		if (!value) {
			fprintf(stderr, "*error* global_param_get() failed");
			return;
		}
		printf("%s=%s\n", str, value);
	}
}

static bool do_enum_params(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *aa[16], **a;
        int i, n;
	char c;
	uint32_t id, param_id;
	struct global *global;
	struct param *p;
	bool is_short = false;

	n = pw_split_ip(args, WHITESPACE, SPA_N_ELEMENTS(aa), aa);
	if (n < 1) {
		*error = spa_aprintf("%s <object-id> [<param-id>]", cmd);
		return false;
	}

	a = aa;
	while (n > 0 && a[0][0] == '-') {
		for (i = 1; (c = a[0][i]) != '\0'; i++) {
			if (c == 's')
				is_short = true;
			else
				goto usage;
		}
		n--;
		a++;
	}

	id = atoi(a[0]);
	param_id = n >= 2 ? (uint32_t)atoi(a[1]) : (uint32_t)-1;

	global = remote_global(rd, id);
	if (global == NULL) {
		*error = spa_aprintf("%s: unknown global %d", cmd, id);
		return false;
	}

	if (!global_can_enum_params(global)) {
		*error = spa_aprintf("enum-params not implemented on object %d type:%s",
				id, global->type);
		return false;
	}

	/* wait until param enum is complete */
	if (!(global->flags & GLOBAL_PARAM_ENUM_COMPLETE)) {
		*error = spa_aprintf("enum-params not complete on object %d type:%s",
				id, global->type);
		return false;
	}

	spa_list_for_each(p, &global->params, link) {
		/* only if all, or selected */
		if (param_id != (uint32_t)-1 && p->info.id != param_id)
			continue;

		enum_single_param(global, p, is_short);
	}

	return true;
usage:
	*error = spa_aprintf("%s [-s] <id> [<param-id>]", cmd);
	return false;
}

static bool do_permissions(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[3];
        int n;
	uint32_t id;
	struct global *global;
	struct pw_permission permissions[1];

	n = pw_split_ip(args, WHITESPACE, 3, a);
	if (n < 3) {
		*error = spa_aprintf("%s <client-id> <object> <permission>", cmd);
		return false;
	}

	id = atoi(a[0]);
	global = pw_map_lookup(&rd->globals, id);
	if (global == NULL) {
		*error = spa_aprintf("%s: unknown global %d", cmd, id);
		return false;
	}
	if (strcmp(global->type, PW_TYPE_INTERFACE_Client) != 0) {
		*error = spa_aprintf("object %d is not a client", atoi(a[0]));
		return false;
	}
	if (global->proxy == NULL) {
		if (!bind_global(rd, global, error))
			return false;
	}

	permissions[0] = PW_PERMISSION_INIT(atoi(a[1]), atoi(a[2]));

	pw_client_update_permissions((struct pw_client*)global->proxy,
			1, permissions);

	return true;
}

static bool do_get_permissions(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *a[3];
        int n;
	uint32_t id;
	struct global *global;

	n = pw_split_ip(args, WHITESPACE, 1, a);
	if (n < 1) {
		*error = spa_aprintf("%s <client-id>", cmd);
		return false;
	}

	id = atoi(a[0]);
	global = pw_map_lookup(&rd->globals, id);
	if (global == NULL) {
		*error = spa_aprintf("%s: unknown global %d", cmd, id);
		return false;
	}
	if (strcmp(global->type, PW_TYPE_INTERFACE_Client) != 0) {
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

static const char *
pw_interface_short(const char *type)
{
	size_t ilen;

	ilen = strlen(PW_TYPE_INFO_INTERFACE_BASE);

	if (!type || strlen(type) <= ilen ||
	    memcmp(type, PW_TYPE_INFO_INTERFACE_BASE, ilen))
		return NULL;

	return type + ilen;
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
global_props(struct global *global, bool info_property)
{
	struct proxy_data *pd;

	if (!global)
		return NULL;

	if (!info_property) {
		if (!global->properties)
			return NULL;
		return &global->properties->dict;
	}

	pd = pw_proxy_get_user_data(global->proxy);
	if (!pd || !pd->info)
		return NULL;

	if (!strcmp(global->type, PW_TYPE_INTERFACE_Core))
		return ((struct pw_core_info *)pd->info)->props;
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Module))
		return ((struct pw_module_info *)pd->info)->props;
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Device))
		return ((struct pw_device_info *)pd->info)->props;
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Node))
		return ((struct pw_node_info *)pd->info)->props;
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Port))
		return ((struct pw_port_info *)pd->info)->props;
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Factory))
		return ((struct pw_factory_info *)pd->info)->props;
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Client))
		return ((struct pw_client_info *)pd->info)->props;
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Link))
		return ((struct pw_link_info *)pd->info)->props;
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Session))
		return ((struct pw_session_info *)pd->info)->props;
	if (!strcmp(global->type, PW_TYPE_INTERFACE_Endpoint))
		return ((struct pw_endpoint_info *)pd->info)->props;
	if (!strcmp(global->type, PW_TYPE_INTERFACE_EndpointStream))
		return ((struct pw_endpoint_stream_info *)pd->info)->props;
	if (!strcmp(global->type, PW_TYPE_INTERFACE_EndpointLink))
		return ((struct pw_endpoint_link_info *)pd->info)->props;

	return NULL;
}

static struct spa_dict *
obj_props(struct remote_data *rd, uint32_t id)
{
	struct global *global;

	if (!rd)
		return NULL;

	global = obj_global(rd, id);
	if (!global)
		return NULL;
	return global_props(global, true);
}

static const char *
global_lookup(struct global *global, const char *key)
{
	struct spa_dict *dict;

	dict = global_props(global, true);
	if (!dict)
		return NULL;
	return spa_dict_lookup(dict, key);
}

static const char *
obj_lookup(struct remote_data *rd, uint32_t id, const char *key)
{
	struct spa_dict *dict;

	dict = obj_props(rd, id);
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
	if (!strcmp(parent_type, PW_TYPE_INTERFACE_Device) &&
	    !strcmp(child_type, PW_TYPE_INTERFACE_Node)) {
		parent_key = PW_KEY_OBJECT_ID;
		child_key = PW_KEY_DEVICE_ID;
	} else if (!strcmp(parent_type, PW_TYPE_INTERFACE_Node) &&
		   !strcmp(child_type, PW_TYPE_INTERFACE_Port)) {
		parent_key = PW_KEY_OBJECT_ID;
		child_key = PW_KEY_NODE_ID;
	} else if (!strcmp(parent_type, PW_TYPE_INTERFACE_Module) &&
		   !strcmp(child_type, PW_TYPE_INTERFACE_Factory)) {
		parent_key = PW_KEY_OBJECT_ID;
		child_key = PW_KEY_MODULE_ID;
	} else if (!strcmp(parent_type, PW_TYPE_INTERFACE_Factory) &&
		   !strcmp(child_type, PW_TYPE_INTERFACE_Device)) {
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

			*children = malloc(sizeof(*children) * count);
			if (!*children)
				return -1;
		}
		i = 0;
		pw_array_for_each(item, &rd->globals.items) {
			if (pw_map_item_is_free(item) || item->data == NULL)
				continue;

			global = item->data;

			if (strcmp(global->type, child_type))
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
			if (strcmp(parent_value, child_value))
				continue;

			if (*children)
				(*children)[i] = global->id;
			i++;

		}
	}

	if (!count)
		return 0;

	return count;
}

#ifndef BIT
#define BIT(x) (1U << (x))
#endif

enum dump_flags {
	is_default = 0,
	is_short = BIT(0),
	is_deep = BIT(1),
	is_resolve = BIT(2),
	is_notype = BIT(3)
};

static const char *dump_types[] = {
	PW_TYPE_INTERFACE_Core,
	PW_TYPE_INTERFACE_Module,
	PW_TYPE_INTERFACE_Device,
	PW_TYPE_INTERFACE_Node,
	PW_TYPE_INTERFACE_Port,
	PW_TYPE_INTERFACE_Factory,
	PW_TYPE_INTERFACE_Client,
	PW_TYPE_INTERFACE_Link,
	PW_TYPE_INTERFACE_Session,
	PW_TYPE_INTERFACE_Endpoint,
	PW_TYPE_INTERFACE_EndpointStream,
	PW_TYPE_INTERFACE_EndpointLink,
};

int dump_type_index(const char *type)
{
	unsigned int i;

	if (!type)
		return -1;

	for (i = 0; i < SPA_N_ELEMENTS(dump_types); i++) {
		if (!strcmp(dump_types[i], type))
			return (int)i;
	}

	return -1;
}

static inline unsigned int dump_type_count(void)
{
	return SPA_N_ELEMENTS(dump_types);
}

static const char *name_to_dump_type(const char *name)
{
	unsigned int i;

	if (!name)
		return NULL;

	for (i = 0; i < SPA_N_ELEMENTS(dump_types); i++) {
		if (!strcmp(name, pw_interface_short(dump_types[i])))
			return dump_types[i];
	}

	return NULL;
}

#define INDENT(_level) \
	({ \
		int __level = (_level); \
		char *_indent = alloca(__level + 1); \
		memset(_indent, '\t', __level); \
		_indent[__level] = '\0'; \
		(const char *)_indent; \
	})

static void
dump(struct data *data, struct global *global,
     enum dump_flags flags, int level);

static void
dump_properties(struct data *data, struct global *global,
		enum dump_flags flags, int level)
{
	struct remote_data *rd = data->current;
	struct spa_dict *props;
	const struct spa_dict_item *item;
	const char *ind;
	int id;
	const char *extra;

	if (!global)
		return;

	props = global_props(global, true);
	if (!props || !props->n_items)
		return;

	ind = INDENT(level + 2);
	spa_dict_for_each(item, props) {
		fprintf(stdout, "%s%s = \"%s\"",
				ind, item->key, item->value);

		extra = NULL;
		id = -1;
		if (!strcmp(global->type, PW_TYPE_INTERFACE_Port) && !strcmp(item->key, PW_KEY_NODE_ID)) {
			id = atoi(item->value);
			if (id >= 0)
				extra = obj_lookup(rd, id, PW_KEY_NODE_NAME);
		} else if (!strcmp(global->type, PW_TYPE_INTERFACE_Factory) && !strcmp(item->key, PW_KEY_MODULE_ID)) {
			id = atoi(item->value);
			if (id >= 0)
				extra = obj_lookup(rd, id, PW_KEY_MODULE_NAME);
		} else if (!strcmp(global->type, PW_TYPE_INTERFACE_Device) && !strcmp(item->key, PW_KEY_FACTORY_ID)) {
			id = atoi(item->value);
			if (id >= 0)
				extra = obj_lookup(rd, id, PW_KEY_FACTORY_NAME);
		} else if (!strcmp(global->type, PW_TYPE_INTERFACE_Device) && !strcmp(item->key, PW_KEY_CLIENT_ID)) {
			id = atoi(item->value);
			if (id >= 0)
				extra = obj_lookup(rd, id, PW_KEY_CLIENT_NAME);
		}

		if (extra)
			fprintf(stdout, " (\"%s\")", extra);

		fprintf(stdout, "\n");
	}
}

static void
dump_params(struct data *data, struct global *global,
	    struct spa_param_info *params, uint32_t n_params,
	    enum dump_flags flags, int level)
{
	uint32_t i;
	const char *ind;
	struct param *p;
	struct param_entry *pe;

	if (params == NULL || n_params == 0)
		return;

	ind = INDENT(level + 1);
	for (i = 0; i < n_params; i++) {
		fprintf(stdout, "%s  %d (%s) %c%c\n", ind,
			params[i].id,
			spa_debug_type_find_name(spa_type_param, params[i].id),
			params[i].flags & SPA_PARAM_INFO_READ ? 'r' : '-',
			params[i].flags & SPA_PARAM_INFO_WRITE ? 'w' : '-');

		/* if params were enumerated display them */
		spa_list_for_each(p, &global->params, link) {
			spa_list_for_each(pe, &p->entries, link) {
				if (p->info.id != params[i].id)
					continue;

				if (spa_pod_is_object_type(pe->param, SPA_TYPE_OBJECT_Format))
					spa_debug_format(level + 12, NULL, pe->param);
				else
					spa_debug_pod(level + 12, NULL, pe->param);
			}
		}
	}
}

static void
dump_global_common(struct data *data, struct global *global,
	    enum dump_flags flags, int level)
{
	const char *ind;

	if (!(flags & is_short)) {
		ind = INDENT(level + 1);
		fprintf(stdout, "%sid: %"PRIu32"\n", ind, global->id);
		fprintf(stdout, "%spermissions: %c%c%c\n", ind,
				global->permissions & PW_PERM_R ? 'r' : '-',
				global->permissions & PW_PERM_W ? 'w' : '-',
				global->permissions & PW_PERM_X ? 'x' : '-');
		fprintf(stdout, "%stype: %s/%d\n", ind,
				global->type, global->version);
	} else {
		ind = INDENT(level);
		fprintf(stdout, "%s%"PRIu32":", ind, global->id);
		if (!(flags & is_notype))
			fprintf(stdout, " %s", pw_interface_short(global->type));
	}
}

static bool
dump_core(struct data *data, struct global *global,
	  enum dump_flags flags, int level)
{
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_core_info *info;
	const char *ind;

	if (!pd->info)
		return false;

	dump_global_common(data, global, flags, level);

	info = pd->info;
	if (!(flags & is_short)) {
		ind = INDENT(level + 1);
		fprintf(stdout, "%scookie: %u\n", ind, info->cookie);
		fprintf(stdout, "%suser-name: \"%s\"\n", ind, info->user_name);
		fprintf(stdout, "%shost-name: \"%s\"\n", ind, info->host_name);
		fprintf(stdout, "%sversion: \"%s\"\n", ind, info->version);
		fprintf(stdout, "%sname: \"%s\"\n", ind, info->name);
		fprintf(stdout, "%sproperties:\n", ind);
		dump_properties(data, global, flags, level);
	} else {
		fprintf(stdout, " u=\"%s\" h=\"%s\" v=\"%s\" n=\"%s\"",
				info->user_name, info->host_name, info->version, info->name);
		fprintf(stdout, "\n");
	}

	return true;
}

static bool
dump_module(struct data *data, struct global *global,
	    enum dump_flags flags, int level)
{
	struct remote_data *rd = global->rd;
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_module_info *info;
	const char *args, *desc;
	const char *ind;
	uint32_t *factories = NULL;
	int i, factory_count;
	struct global *global_factory;

	if (!pd->info)
		return false;

	info = pd->info;

	dump_global_common(data, global, flags, level);

	if (!(flags & is_short)) {
		ind = INDENT(level + 1);
		fprintf(stdout, "%sname: \"%s\"\n", ind, info->name);
		fprintf(stdout, "%sfilename: \"%s\"\n", ind, info->filename);
		fprintf(stdout, "%sargs: \"%s\"\n", ind, info->args);
		fprintf(stdout, "%sproperties:\n", ind);
		dump_properties(data, global, flags, level);
	} else {
		desc = spa_dict_lookup(info->props, PW_KEY_MODULE_DESCRIPTION);
		args = info->args && strcmp(info->args, "(null)") ? info->args : NULL;
		fprintf(stdout, " n=\"%s\" f=\"%s\"" "%s%s%s" "%s%s%s",
				info->name, info->filename,
				args ? " a=\"" : "",
				args ? args : "",
				args ? "\"" : "",
				desc ? " d=\"" : "",
				desc ? desc : "",
				desc ? "\"" : "");
		fprintf(stdout, "\n");
	}

	if (!(flags & is_deep))
		return true;

	factory_count = children_of(rd, global->id, PW_TYPE_INTERFACE_Factory, &factories);
	if (factory_count >= 0) {
		ind = INDENT(level + 1);
		fprintf(stdout, "%sfactories:\n", ind);
		for (i = 0; i < factory_count; i++) {
			global_factory = obj_global(rd, factories[i]);
			if (!global_factory)
				continue;
			dump(data, global_factory, flags | is_notype, level + 1);
		}
		free(factories);
	}

	return true;
}

static bool
dump_device(struct data *data, struct global *global,
	    enum dump_flags flags, int level)
{
	struct remote_data *rd = data->current;
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_device_info *info;
	const char *media_class, *api, *desc, *name;
	const char *alsa_path, *alsa_card_id;
	const char *ind;
	uint32_t *nodes = NULL;
	int i, node_count;
	struct global *global_node;

	if (!pd->info)
		return false;

	info = pd->info;

	dump_global_common(data, global, flags, level);

	if (!(flags & is_short)) {
		ind = INDENT(level + 1);
		fprintf(stdout, "%sproperties:\n", ind);
		dump_properties(data, global, flags, level);
		fprintf(stdout, "%sparams:\n", ind);
		dump_params(data, global, info->params, info->n_params, flags, level);
	} else {
		media_class = spa_dict_lookup(info->props, PW_KEY_MEDIA_CLASS);
		name = spa_dict_lookup(info->props, PW_KEY_DEVICE_NAME);
		desc = spa_dict_lookup(info->props, PW_KEY_DEVICE_DESCRIPTION);
		api = spa_dict_lookup(info->props, PW_KEY_DEVICE_API);

		fprintf(stdout, "%s%s%s" "%s%s%s" "%s%s%s" "%s%s%s",
				media_class ? " c=\"" : "",
				media_class ? media_class : "",
				media_class ? "\"" : "",
				name ? " n=\"" : "",
				name ? name : "",
				name ? "\"" : "",
				desc ? " d=\"" : "",
				desc ? desc : "",
				desc ? "\"" : "",
				api ? " a=\"" : "",
				api ? api : "",
				api ? "\"" : "");

		if (media_class && !strcmp(media_class, "Audio/Device") &&
		    api && !strcmp(api, "alsa:pcm")) {

			alsa_path = spa_dict_lookup(info->props, SPA_KEY_API_ALSA_PATH);
			alsa_card_id = spa_dict_lookup(info->props, SPA_KEY_API_ALSA_CARD_ID);

			fprintf(stdout, "%s%s%s" "%s%s%s",
					alsa_path ? " p=\"" : "",
					alsa_path ? alsa_path : "",
					alsa_path ? "\"" : "",
					alsa_card_id ? " id=\"" : "",
					alsa_card_id ? alsa_card_id : "",
					alsa_card_id ? "\"" : "");
		}

		fprintf(stdout, "\n");
	}

	if (!(flags & is_deep))
		return true;

	node_count = children_of(rd, global->id, PW_TYPE_INTERFACE_Node, &nodes);
	if (node_count >= 0) {
		ind = INDENT(level + 1);
		fprintf(stdout, "%snodes:\n", ind);
		for (i = 0; i < node_count; i++) {
			global_node = obj_global(rd, nodes[i]);
			if (!global_node)
				continue;
			dump(data, global_node, flags | is_notype, level + 1);
		}
		free(nodes);
	}

	return true;
}

static bool
dump_node(struct data *data, struct global *global,
	  enum dump_flags flags, int level)
{
	struct remote_data *rd = data->current;
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_node_info *info;
	const char *name, *path;
	const char *ind;
	uint32_t *ports = NULL;
	int i, port_count;
	struct global *global_port;

	if (!pd->info)
		return false;

	dump_global_common(data, global, flags, level);

	info = pd->info;

	if (!(flags & is_short)) {
		ind = INDENT(level + 1);
		fprintf(stdout, "%sinput ports: %u/%u\n", ind, info->n_input_ports, info->max_input_ports);
		fprintf(stdout, "%soutput ports: %u/%u\n", ind, info->n_output_ports, info->max_output_ports);
		fprintf(stdout, "%sstate: \"%s\"", ind, pw_node_state_as_string(info->state));
		if (info->state == PW_NODE_STATE_ERROR && info->error)
			fprintf(stdout, " \"%s\"\n", info->error);
		else
			fprintf(stdout, "\n");
		fprintf(stdout, "%sproperties:\n", ind);
		dump_properties(data, global, flags, level);
		fprintf(stdout, "%sparams:\n", ind);
		dump_params(data, global, info->params, info->n_params, flags, level);
	} else {
		name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME);
		path = spa_dict_lookup(info->props, SPA_KEY_OBJECT_PATH);

		fprintf(stdout, " s=\"%s\"", pw_node_state_as_string(info->state));

		if (info->max_input_ports) {
			fprintf(stdout, " i=%u/%u", info->n_input_ports, info->max_input_ports);
		}
		if (info->max_output_ports) {
			fprintf(stdout, " o=%u/%u", info->n_output_ports, info->max_output_ports);
		}

		fprintf(stdout, "%s%s%s" "%s%s%s",
				name ? " n=\"" : "",
				name ? name : "",
				name ? "\"" : "",
				path ? " p=\"" : "",
				path ? path : "",
				path ? "\"" : "");

		fprintf(stdout, "\n");
	}

	if (!(flags & is_deep))
		return true;

	port_count = children_of(rd, global->id, PW_TYPE_INTERFACE_Port, &ports);
	if (port_count >= 0) {
		ind = INDENT(level + 1);
		fprintf(stdout, "%sports:\n", ind);
		for (i = 0; i < port_count; i++) {
			global_port = obj_global(rd, ports[i]);
			if (!global_port)
				continue;
			dump(data, global_port, flags | is_notype, level + 1);
		}
		free(ports);
	}
	return true;
}

static bool
dump_port(struct data *data, struct global *global,
	  enum dump_flags flags, int level)
{
	struct remote_data *rd = data->current;
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_port_info *info;
	const char *ind;
	const char *name, *format;

	if (!pd->info)
		return false;

	dump_global_common(data, global, flags, level);

	info = pd->info;

	if (!(flags & is_short)) {
		ind = INDENT(level + 1);
		fprintf(stdout, "%sdirection: \"%s\"\n", ind,
				pw_direction_as_string(info->direction));
		fprintf(stdout, "%sproperties:\n", ind);
		dump_properties(data, global, flags, level);
		fprintf(stdout, "%sparams:\n", ind);
		dump_params(data, global, info->params, info->n_params, flags, level);
	} else {
		fprintf(stdout, " d=\"%s\"", pw_direction_as_string(info->direction));

		name = spa_dict_lookup(info->props, PW_KEY_PORT_NAME);
		format = spa_dict_lookup(info->props, PW_KEY_FORMAT_DSP);

		fprintf(stdout, "%s%s%s" "%s%s%s",
				name ? " n=\"" : "",
				name ? name : "",
				name ? "\"" : "",
				format ? " f=\"" : "",
				format ? format : "",
				format ? "\"" : "");

		fprintf(stdout, "\n");
	}

	(void)rd;

	return true;
}

static bool
dump_factory(struct data *data, struct global *global,
	     enum dump_flags flags, int level)
{
	struct remote_data *rd = data->current;
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_factory_info *info;
	const char *ind;
	const char *module_id, *module_name;

	if (!pd->info)
		return false;

	dump_global_common(data, global, flags, level);

	info = pd->info;

	if (!(flags & is_short)) {
		ind = INDENT(level + 1);
		fprintf(stdout, "%sname: \"%s\"\n", ind, info->name);
		fprintf(stdout, "%sproperties:\n", ind);
		dump_properties(data, global, flags, level);
	} else {
		fprintf(stdout, " n=\"%s\"", info->name);

		module_id = spa_dict_lookup(info->props, PW_KEY_MODULE_ID);
		module_name = module_id ? obj_lookup(rd, atoi(module_id), PW_KEY_MODULE_NAME) : NULL;

		fprintf(stdout, "%s%s%s",
				module_name ? " m=\"" : "",
				module_name ? module_name : "",
				module_name ? "\"" : "");

		fprintf(stdout, "\n");
	}

	return true;
}

static bool
dump_client(struct data *data, struct global *global,
	    enum dump_flags flags, int level)
{
	struct remote_data *rd = data->current;
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_client_info *info;
	const char *ind;
	const char *app_name, *app_pid;

	if (!pd->info)
		return false;

	dump_global_common(data, global, flags, level);

	info = pd->info;

	if (!(flags & is_short)) {
		ind = INDENT(level + 1);
		fprintf(stdout, "%sproperties:\n", ind);
		dump_properties(data, global, flags, level);
	} else {
		app_name = spa_dict_lookup(info->props, PW_KEY_APP_NAME);
		app_pid = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_ID);

		fprintf(stdout, "%s%s%s" "%s%s%s",
				app_name ? " ap=\"" : "",
				app_name ? app_name : "",
				app_name ? "\"" : "",
				app_pid ? " ai=\"" : "",
				app_pid ? app_pid : "",
				app_pid ? "\"" : "");

		fprintf(stdout, "\n");
	}

	(void)rd;

	return true;
}

static bool
dump_link(struct data *data, struct global *global,
	  enum dump_flags flags, int level)
{
	struct remote_data *rd = data->current;
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_link_info *info;
	const char *ind;
	const char *in_node_name, *in_port_name;
	const char *out_node_name, *out_port_name;

	if (!pd->info)
		return false;

	dump_global_common(data, global, flags, level);

	info = pd->info;

	if (!(flags & is_short)) {
		ind = INDENT(level + 1);
		fprintf(stdout, "%soutput-node-id: %u\n", ind, info->output_node_id);
		fprintf(stdout, "%soutput-port-id: %u\n", ind, info->output_port_id);
		fprintf(stdout, "%sinput-node-id: %u\n", ind, info->input_node_id);
		fprintf(stdout, "%sinput-port-id: %u\n", ind, info->input_port_id);

		fprintf(stdout, "%sstate: \"%s\"", ind,
				pw_link_state_as_string(info->state));
		if (info->state == PW_LINK_STATE_ERROR && info->error)
			printf(" \"%s\"\n", info->error);
		else
			printf("\n");
		fprintf(stdout, "%sformat:\n", ind);
		if (info->format)
			spa_debug_format(8 * (level + 1) + 2, NULL, info->format);
		else
			fprintf(stdout, "%s\tnone\n", ind);

		fprintf(stdout, "%sproperties:\n", ind);
		dump_properties(data, global, flags, level);
	} else {
		out_node_name = obj_lookup(rd, info->output_node_id, PW_KEY_NODE_NAME);
		in_node_name = obj_lookup(rd, info->input_node_id, PW_KEY_NODE_NAME);
		out_port_name = obj_lookup(rd, info->output_port_id, PW_KEY_PORT_NAME);
		in_port_name = obj_lookup(rd, info->input_port_id, PW_KEY_PORT_NAME);

		fprintf(stdout, " s=\"%s\"", pw_link_state_as_string(info->state));

		if (out_node_name && out_port_name)
			fprintf(stdout, " on=\"%s\"" " op=\"%s\"",
					out_node_name, out_port_name);
		if (in_node_name && in_port_name)
			fprintf(stdout, " in=\"%s\"" " ip=\"%s\"",
					in_node_name, in_port_name);

		fprintf(stdout, "\n");
	}

	(void)rd;

	return true;
}

static bool
dump_session(struct data *data, struct global *global,
	     enum dump_flags flags, int level)
{
	struct remote_data *rd = data->current;
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_session_info *info;
	const char *ind;

	if (!pd->info)
		return false;

	dump_global_common(data, global, flags, level);

	info = pd->info;

	if (!(flags & is_short)) {
		ind = INDENT(level + 1);
		fprintf(stdout, "%sproperties:\n", ind);
		dump_properties(data, global, flags, level);
		fprintf(stdout, "%sparams:\n", ind);
		dump_params(data, global, info->params, info->n_params, flags, level);
	} else {
		fprintf(stdout, "\n");
	}

	(void)rd;

	return true;
}

static bool
dump_endpoint(struct data *data, struct global *global,
	      enum dump_flags flags, int level)
{
	struct remote_data *rd = data->current;
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_endpoint_info *info;
	const char *ind;
	const char *direction;

	if (!pd->info)
		return false;

	dump_global_common(data, global, flags, level);

	info = pd->info;

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

	if (!(flags & is_short)) {
		ind = INDENT(level + 1);
		fprintf(stdout, "%sname: %s\n", ind, info->name);
		fprintf(stdout, "%smedia-class: %s\n", ind, info->media_class);
		fprintf(stdout, "%sdirection: %s\n", ind, direction);
		fprintf(stdout, "%sflags: 0x%x\n", ind, info->flags);
		fprintf(stdout, "%sstreams: %u\n", ind, info->n_streams);
		fprintf(stdout, "%ssession: %u\n", ind, info->session_id);
		fprintf(stdout, "%sproperties:\n", ind);
		dump_properties(data, global, flags, level);
		fprintf(stdout, "%sparams:\n", ind);
		dump_params(data, global, info->params, info->n_params, flags, level);
	} else {
		fprintf(stdout, " n=\"%s\" c=\"%s\" d=\"%s\" s=%u si=%"PRIu32"",
				info->name, info->media_class, direction,
				info->n_streams, info->session_id);
		fprintf(stdout, "\n");
	}

	(void)rd;

	return true;
}

static bool
dump_endpoint_stream(struct data *data, struct global *global,
		     enum dump_flags flags, int level)
{
	struct remote_data *rd = data->current;
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_endpoint_stream_info *info;
	const char *ind;

	if (!pd->info)
		return false;

	dump_global_common(data, global, flags, level);

	info = pd->info;

	if (!(flags & is_short)) {
		ind = INDENT(level + 1);
		fprintf(stdout, "%sid: %u\n", ind, info->id);
		fprintf(stdout, "%sendpoint-id: %u\n", ind, info->endpoint_id);
		fprintf(stdout, "%sname: %s\n", ind, info->name);
		fprintf(stdout, "%sproperties:\n", ind);
		dump_properties(data, global, flags, level);
		fprintf(stdout, "%sparams:\n", ind);
		dump_params(data, global, info->params, info->n_params, flags, level);
	} else {
		fprintf(stdout, " n=\"%s\" i=%"PRIu32" ei=%"PRIu32"",
				info->name, info->id, info->endpoint_id);
		fprintf(stdout, "\n");
	}

	(void)rd;

	return true;
}

static bool
dump_endpoint_link(struct data *data, struct global *global,
		     enum dump_flags flags, int level)
{
	struct remote_data *rd = data->current;
	struct proxy_data *pd = pw_proxy_get_user_data(global->proxy);
	struct pw_endpoint_link_info *info;
	const char *ind;

	if (!pd->info)
		return false;

	dump_global_common(data, global, flags, level);

	info = pd->info;

	if (!(flags & is_short)) {
		ind = INDENT(level + 1);
		fprintf(stdout, "%sid: %u\n", ind, info->id);
		fprintf(stdout, "%ssession-id: %u\n", ind, info->session_id);
		fprintf(stdout, "%soutput-endpoint-id: %u\n", ind, info->output_endpoint_id);
		fprintf(stdout, "%soutput-stream-id: %u\n", ind, info->output_stream_id);
		fprintf(stdout, "%sinput-endpoint-id: %u\n", ind, info->input_endpoint_id);
		fprintf(stdout, "%sinput-stream-id: %u\n", ind, info->input_stream_id);
		fprintf(stdout, "%sstate: \"%s\"", ind,
				_pw_endpoint_link_state_as_string(info->state));
		if (info->state == PW_ENDPOINT_LINK_STATE_ERROR && info->error)
			fprintf(stdout, " \"%s\"\n", info->error);
		else
			fprintf(stdout, "\n");
		fprintf(stdout, "%sproperties:\n", ind);
		dump_properties(data, global, flags, level);
		fprintf(stdout, "%sparams:\n", ind);
		dump_params(data, global, info->params, info->n_params, flags, level);
	} else {
		fprintf(stdout, " i=%"PRIu32" ei=%"PRIu32" s=%s",
				info->id, info->session_id,
				_pw_endpoint_link_state_as_string(info->state));
		fprintf(stdout, "\n");
	}

	(void)rd;

	return true;
}

static void
dump(struct data *data, struct global *global,
     enum dump_flags flags, int level)
{
	if (!global)
		return;

	if (!strcmp(global->type, PW_TYPE_INTERFACE_Core))
		dump_core(data, global, flags, level);

	if (!strcmp(global->type, PW_TYPE_INTERFACE_Module))
		dump_module(data, global, flags, level);

	if (!strcmp(global->type, PW_TYPE_INTERFACE_Device))
		dump_device(data, global, flags, level);

	if (!strcmp(global->type, PW_TYPE_INTERFACE_Node))
		dump_node(data, global, flags, level);

	if (!strcmp(global->type, PW_TYPE_INTERFACE_Port))
		dump_port(data, global, flags, level);

	if (!strcmp(global->type, PW_TYPE_INTERFACE_Factory))
		dump_factory(data, global, flags, level);

	if (!strcmp(global->type, PW_TYPE_INTERFACE_Client))
		dump_client(data, global, flags, level);

	if (!strcmp(global->type, PW_TYPE_INTERFACE_Link))
		dump_link(data, global, flags, level);

	if (!strcmp(global->type, PW_TYPE_INTERFACE_Session))
		dump_session(data, global, flags, level);

	if (!strcmp(global->type, PW_TYPE_INTERFACE_Endpoint))
		dump_endpoint(data, global, flags, level);

	if (!strcmp(global->type, PW_TYPE_INTERFACE_EndpointStream))
		dump_endpoint_stream(data, global, flags, level);

	if (!strcmp(global->type, PW_TYPE_INTERFACE_EndpointLink))
		dump_endpoint_link(data, global, flags, level);
}

static bool do_dump(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	union pw_map_item *item;
	struct global *global;
	char *aa[32], **a;
	char c;
	int i, n, idx;
	enum dump_flags flags = is_default;
	bool match;
	unsigned int type_mask;

	n = pw_split_ip(args, WHITESPACE, SPA_N_ELEMENTS(aa), aa);
	if (n < 0)
		goto usage;

	a = aa;
	while (n > 0 &&
		(!strcmp(a[0], "short") ||
		 !strcmp(a[0], "deep") ||
		 !strcmp(a[0], "resolve") ||
		 !strcmp(a[0], "notype"))) {
		if (!strcmp(a[0], "short"))
			flags |= is_short;
		else if (!strcmp(a[0], "deep"))
			flags |= is_deep;
		else if (!strcmp(a[0], "resolve"))
			flags |= is_resolve;
		else if (!strcmp(a[0], "notype"))
			flags |= is_notype;
		n--;
		a++;
	}

	while (n > 0 && a[0][0] == '-') {
		for (i = 1; (c = a[0][i]) != '\0'; i++) {
			if (c == 's')
				flags |= is_short;
			else if (c == 'd')
				flags |= is_deep;
			else if (c == 'r')
				flags |= is_resolve;
			else if (c == 't')
				flags |= is_notype;
			else
				goto usage;
		}
		n--;
		a++;
	}

	if (n == 0 || !strcmp(a[0], "all")) {
		type_mask = (1U << dump_type_count()) - 1;
		flags &= ~is_notype;
	} else {
		type_mask = 0;
		for (i = 0; i < n; i++) {
			/* skip direct IDs */
			if (isdigit(a[i][0]))
				continue;
			idx = dump_type_index(name_to_dump_type(a[i]));
			if (idx < 0)
				goto usage;
			type_mask |= 1U << idx;
		}

		/* single bit set? disable type */
		if ((type_mask & (type_mask - 1)) == 0)
			flags |= is_notype;
	}

	pw_array_for_each(item, &rd->globals.items) {
		if (pw_map_item_is_free(item) || item->data == NULL)
			continue;

		global = item->data;

		/* unknown type, ignore completely */
		idx = dump_type_index(global->type);
		if (idx < 0)
			continue;

		match = false;

		/* first check direct ids */
		for (i = 0; i < n; i++) {
			/* skip non direct IDs */
			if (!isdigit(a[i][0]))
				continue;
			if (atoi(a[i]) == (int)global->id) {
				match = true;
				break;
			}
		}

		/* if type match */
		if (!match && (type_mask & (1U << idx)))
			match = true;

		if (!match)
			continue;

		dump(data, global, flags, 0);
	}

	return true;
usage:
	*error = spa_aprintf("%s [short|deep|resolve|notype] [-sdrt] [all|%s|<id>]",
			cmd, DUMP_NAMES);
	return false;
}

static bool do_graph(struct data *data, const char *cmd, char *args, char **error)
{
	struct remote_data *rd = data->current;
	char *aa[16], **a, c;
        int i, n;
	const char *prop, *value;
	struct var_ctx v;
	char buf[256 * 1024];
	enum var_format fmt = var_yaml;
	unsigned int flags = 0;

	n = pw_split_ip(args, WHITESPACE, SPA_N_ELEMENTS(aa), aa);
	if (n < 1) {
		*error = spa_aprintf("%s <path>", cmd);
		return false;
	}

	a = aa;
	while (n > 0 && a[0][0] == '-') {
		for (i = 1; (c = a[0][i]) != '\0'; i++) {
			switch (c) {
			case 'j':
				fmt = var_json;
				break;
			case 'y':
				fmt = var_yaml;
				break;
			case 'n':
				flags |= VF_TYPE_NUMERIC;
				break;
			case 'f':
				flags |= VF_TYPE_FULL;
				break;
			default:
				goto usage;
			}
		}
		n--;
		a++;
	}

	prop = n >= 1 ? a[0] : NULL;

	var_init(&v, buf, sizeof(buf), fmt, flags, 0, 0);

	value = remote_graph_get(rd, &v, prop ? prop : ".");
	if (!value) {
		*error = spa_aprintf("%s remote_graph_get() failed", cmd);
		return false;
	}

	printf("%s\n", value);

	return true;
usage:
	*error = spa_aprintf("%s [-jynf] <path>", cmd);
	return false;
}

static bool parse(struct data *data, char *buf, size_t size, char **error)
{
	char *a[2];
	int n;
	size_t i;
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

	for (i = 0; i < SPA_N_ELEMENTS(command_list); i++) {
		if (!strcmp(command_list[i].name, cmd) ||
		    !strcmp(command_list[i].alias, cmd)) {
			return command_list[i].func(data, cmd, args, error);
		}
	}
        *error = spa_aprintf("Command \"%s\" does not exist. Type 'help' for usage.", cmd);
	return false;
}

static void do_input(void *data, int fd, uint32_t mask)
{
	struct data *d = data;
	char buf[4096], *error;
	ssize_t r;

	if (mask & SPA_IO_IN) {
		while (true) {
			r = read(fd, buf, sizeof(buf));
			if (r < 0) {
				if (errno == EAGAIN)
					continue;
				perror("read");
				r = 0;
				break;
			}
			break;
		}
		if (r == 0) {
			fprintf(stdout, "\n");
			pw_main_loop_quit(d->loop);
			return;
		}
		buf[r] = '\0';

		if (!parse(d, buf, r, &error)) {
			fprintf(stdout, "Error: \"%s\"\n", error);
			free(error);
		}
		if (d->current == NULL)
			pw_main_loop_quit(d->loop);
		else  {
			struct remote_data *rd = d->current;
			if (rd->core)
				rd->prompt_pending = pw_core_sync(rd->core, 0, 0);
		}
	}
}

static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	struct pw_loop *l;
	char *error;

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);
	l = pw_main_loop_get_loop(data.loop);
	pw_loop_add_signal(l, SIGINT, do_quit, &data);
	pw_loop_add_signal(l, SIGTERM, do_quit, &data);

	spa_list_init(&data.remotes);
	pw_map_init(&data.vars, 64, 16);

	data.context = pw_context_new(l, pw_properties_new(PW_KEY_CORE_DAEMON, "true", NULL), 0);

	pw_context_load_module(data.context, "libpipewire-module-link-factory", NULL, NULL);

	pw_loop_add_io(l, STDIN_FILENO, SPA_IO_IN|SPA_IO_HUP, false, do_input, &data);

	fprintf(stdout, "Welcome to PipeWire version %s. Type 'help' for usage.\n",
			pw_get_library_version());

	do_connect(&data, "connect", "internal", &error);

	pw_main_loop_run(data.loop);

	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);

	return 0;
}
