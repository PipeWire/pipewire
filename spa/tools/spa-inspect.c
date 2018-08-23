/* Simple Plugin API
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <error.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>

#include <spa/support/log-impl.h>
#include <spa/support/loop.h>
#include <spa/node/node.h>
#include <spa/pod/parser.h>
#include <spa/param/param.h>
#include <spa/param/format.h>
#include <spa/debug/dict.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>

static SPA_LOG_IMPL(default_log);

struct data {
	struct spa_support support[4];
	uint32_t n_support;
	struct spa_log *log;
	struct spa_loop loop;
};

static void
inspect_node_params(struct data *data, struct spa_node *node)
{
	int res;
	uint32_t idx1, idx2;

	for (idx1 = 0;;) {
		uint32_t buffer[4096];
		struct spa_pod_builder b = { 0 };
		struct spa_pod *param;
		uint32_t id;

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		if ((res = spa_node_enum_params(node,
						SPA_ID_PARAM_List, &idx1,
						NULL, &param, &b)) <= 0) {
			if (res != 0)
				error(0, -res, "enum_params");
			break;
		}

		spa_pod_object_parse(param,
				":", SPA_PARAM_LIST_id, "I", &id,
				NULL);

		printf("enumerating: %s:\n", spa_debug_type_find_name(spa_debug_types, id));
		for (idx2 = 0;;) {
			spa_pod_builder_init(&b, buffer, sizeof(buffer));
			if ((res = spa_node_enum_params(node,
							id, &idx2,
							NULL, &param, &b)) <= 0) {
				if (res != 0)
					error(0, -res, "enum_params %d", id);
				break;
			}
			spa_debug_pod(0, spa_debug_types, param);
		}
	}
}

static void
inspect_port_params(struct data *data, struct spa_node *node,
		    enum spa_direction direction, uint32_t port_id)
{
	int res;
	uint32_t idx1, idx2;

	for (idx1 = 0;;) {
		uint32_t buffer[4096];
		struct spa_pod_builder b = { 0 };
		struct spa_pod *param;
		uint32_t id;

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		if ((res = spa_node_port_enum_params(node,
						     direction, port_id,
						     SPA_ID_PARAM_List, &idx1,
						     NULL, &param, &b)) <= 0) {
			if (res != 0)
				error(0, -res, "port_enum_params");
			break;
		}
		spa_pod_object_parse(param,
				":", SPA_PARAM_LIST_id, "I", &id,
				NULL);

		printf("enumerating: %s:\n", spa_debug_type_find_name(spa_debug_types, id));
		for (idx2 = 0;;) {
			spa_pod_builder_init(&b, buffer, sizeof(buffer));
			if ((res = spa_node_port_enum_params(node,
							     direction, port_id,
							     id, &idx2,
							     NULL, &param, &b)) <= 0) {
				if (res != 0)
					error(0, -res, "port_enum_params");
				break;
			}

			if (spa_pod_is_object_type(param, SPA_ID_OBJECT_Format))
				spa_debug_format(0, NULL, param);
			else
				spa_debug_pod(0, spa_debug_types, param);
		}
	}
}

static void inspect_node(struct data *data, struct spa_node *node)
{
	int res;
	uint32_t i, n_input, max_input, n_output, max_output;
	uint32_t *in_ports, *out_ports;

	printf("node info:\n");
	if (node->info)
		spa_debug_dict(2, node->info);
	else
		printf("  none\n");

	inspect_node_params(data, node);

	if ((res = spa_node_get_n_ports(node, &n_input, &max_input, &n_output, &max_output)) < 0) {
		printf("can't get n_ports: %d\n", res);
		return;
	}
	printf("supported ports:\n");
	printf("input ports:  %d/%d\n", n_input, max_input);
	printf("output ports: %d/%d\n", n_output, max_output);

	in_ports = alloca(n_input * sizeof(uint32_t));
	out_ports = alloca(n_output * sizeof(uint32_t));

	if ((res = spa_node_get_port_ids(node, in_ports, n_input, out_ports, n_output)) < 0)
		printf("can't get port ids: %d\n", res);

	for (i = 0; i < n_input; i++) {
		printf(" input port: %08x\n", in_ports[i]);
		inspect_port_params(data, node, SPA_DIRECTION_INPUT, in_ports[i]);
	}

	for (i = 0; i < n_output; i++) {
		printf(" output port: %08x\n", out_ports[i]);
		inspect_port_params(data, node, SPA_DIRECTION_OUTPUT, out_ports[i]);
	}

}

static void inspect_factory(struct data *data, const struct spa_handle_factory *factory)
{
	int res;
	struct spa_handle *handle;
	void *interface;
	const struct spa_interface_info *info;
	uint32_t index;

	printf("factory name:\t\t'%s'\n", factory->name);
	printf("factory info:\n");
	if (factory->info)
		spa_debug_dict(2, factory->info);
	else
		printf("  none\n");

	printf("factory interfaces:\n");
	for (index = 0;;) {
		if ((res = spa_handle_factory_enum_interface_info(factory, &info, &index)) <= 0) {
			if (res == 0)
				break;
			else
				error(0, -res, "spa_handle_factory_enum_interface_info");
		}
		printf(" interface: '%d'\n", info->type);
	}

	handle = calloc(1, spa_handle_factory_get_size(factory, NULL));
	if ((res =
	     spa_handle_factory_init(factory, handle, NULL, data->support, data->n_support)) < 0) {
		printf("can't make factory instance: %d\n", res);
		return;
	}

	printf("factory instance:\n");

	for (index = 0;;) {
		if ((res = spa_handle_factory_enum_interface_info(factory, &info, &index)) <= 0) {
			if (res == 0)
				break;
			else
				error(0, -res, "spa_handle_factory_enum_interface_info");
		}
		printf(" interface: '%d'\n", info->type);

		if ((res = spa_handle_get_interface(handle, info->type, &interface)) < 0) {
			printf("can't get interface: %d %d\n", info->type, res);
			continue;
		}

		if (info->type == SPA_ID_INTERFACE_Node)
			inspect_node(data, interface);
		else
			printf("skipping unknown interface\n");
	}
}

static int do_add_source(struct spa_loop *loop, struct spa_source *source)
{
	return 0;
}

static int do_update_source(struct spa_source *source)
{
	return 0;
}

static void do_remove_source(struct spa_source *source)
{
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	int res;
	void *handle;
	spa_handle_factory_enum_func_t enum_func;
	uint32_t index;
	const char *str;

	if (argc < 2) {
		printf("usage: %s <plugin.so>\n", argv[0]);
		return -1;
	}

	data.log = &default_log.log;
	data.loop.version = SPA_VERSION_LOOP;
	data.loop.add_source = do_add_source;
	data.loop.update_source = do_update_source;
	data.loop.remove_source = do_remove_source;

	if ((str = getenv("SPA_DEBUG")))
		data.log->level = atoi(str);

	data.support[0] = SPA_SUPPORT_INIT(SPA_ID_INTERFACE_Log, data.log);
	data.support[1] = SPA_SUPPORT_INIT(SPA_ID_INTERFACE_MainLoop, &data.loop);
	data.support[2] = SPA_SUPPORT_INIT(SPA_ID_INTERFACE_DataLoop, &data.loop);
	data.n_support = 3;

	if ((handle = dlopen(argv[1], RTLD_NOW)) == NULL) {
		printf("can't load %s\n", argv[1]);
		return -1;
	}
	if ((enum_func = dlsym(handle, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		printf("can't find function\n");
		return -1;
	}

	for (index = 0;;) {
		const struct spa_handle_factory *factory;

		if ((res = enum_func(&factory, &index)) <= 0) {
			if (res != 0)
				error(0, -res, "enum_func");
			break;
		}
		inspect_factory(&data, factory);
	}
	return 0;
}
