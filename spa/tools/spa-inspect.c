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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>

#include <spa/type-map-impl.h>
#include <spa/clock.h>
#include <spa/log-impl.h>
#include <spa/node.h>
#include <spa/loop.h>

#include <lib/debug.h>

static SPA_TYPE_MAP_IMPL(default_map, 4096);
static SPA_LOG_IMPL(default_log);

struct type {
	uint32_t node;
	uint32_t clock;
};

struct data {
	struct type type;

	struct spa_support support[4];
	uint32_t n_support;
	struct spa_type_map *map;
	struct spa_log *log;
	struct spa_loop loop;
};

static void
inspect_port(struct data *data, struct spa_node *node, enum spa_direction direction,
	     uint32_t port_id)
{
	int res;
	struct spa_format *format;
	uint32_t index;

	for (index = 0;; index++) {
		if ((res =
		     spa_node_port_enum_formats(node, direction, port_id, &format, NULL,
						index)) < 0) {
			if (res != SPA_RESULT_ENUM_END)
				printf("got error %d\n", res);
			break;
		}
		if (format)
			spa_debug_format(format);
	}


	for (index = 0;; index++) {
		struct spa_param *param;

		if ((res = spa_node_port_enum_params(node, direction, port_id, index, &param)) < 0) {
			if (res != SPA_RESULT_ENUM_END)
				printf("port_enum_params error: %d\n", res);
			break;
		}
		spa_debug_param(param);
	}
}

static void inspect_node(struct data *data, struct spa_node *node)
{
	int res;
	uint32_t i, n_input, max_input, n_output, max_output;
	uint32_t *in_ports, *out_ports;
	struct spa_props *props;

	printf("node info:\n");
	if (node->info)
		spa_debug_dict(node->info);
	else
		printf("  none\n");

	if ((res = spa_node_get_props(node, &props)) < 0)
		printf("can't get properties: %d\n", res);
	else
		spa_debug_props(props);

	if ((res = spa_node_get_n_ports(node, &n_input, &max_input, &n_output, &max_output)) < 0) {
		printf("can't get n_ports: %d\n", res);
		return;
	}
	printf("supported ports:\n");
	printf("input ports:  %d/%d\n", n_input, max_input);
	printf("output ports: %d/%d\n", n_output, max_output);

	in_ports = alloca(n_input * sizeof(uint32_t));
	out_ports = alloca(n_output * sizeof(uint32_t));

	if ((res = spa_node_get_port_ids(node, n_input, in_ports, n_output, out_ports)) < 0)
		printf("can't get port ids: %d\n", res);

	for (i = 0; i < n_input; i++) {
		printf(" input port: %08x\n", in_ports[i]);
		inspect_port(data, node, SPA_DIRECTION_INPUT, in_ports[i]);
	}

	for (i = 0; i < n_output; i++) {
		printf(" output port: %08x\n", out_ports[i]);
		inspect_port(data, node, SPA_DIRECTION_OUTPUT, out_ports[i]);
	}

}

static void inspect_factory(struct data *data, const struct spa_handle_factory *factory)
{
	int res;
	struct spa_handle *handle;
	void *interface;
	uint32_t index = 0;

	printf("factory name:\t\t'%s'\n", factory->name);
	printf("factory info:\n");
	if (factory->info)
		spa_debug_dict(factory->info);
	else
		printf("  none\n");

	handle = calloc(1, factory->size);
	if ((res =
	     spa_handle_factory_init(factory, handle, NULL, data->support, data->n_support)) < 0) {
		printf("can't make factory instance: %d\n", res);
		return;
	}

	printf("factory interfaces:\n");

	while (true) {
		const struct spa_interface_info *info;
		uint32_t interface_id;

		if ((res = spa_handle_factory_enum_interface_info(factory, &info, index)) < 0) {
			if (res == SPA_RESULT_ENUM_END)
				break;
			else
				printf("can't enumerate interfaces: %d\n", res);
		}
		index++;
		printf(" interface: '%s'\n", info->type);

		interface_id = spa_type_map_get_id(data->map, info->type);

		if ((res = spa_handle_get_interface(handle, interface_id, &interface)) < 0) {
			printf("can't get interface: %d\n", res);
			continue;
		}

		if (interface_id == data->type.node)
			inspect_node(data, interface);
		else
			printf("skipping unknown interface\n");
	}
}

static int do_add_source(struct spa_loop *loop, struct spa_source *source)
{
	return SPA_RESULT_OK;
}

static int do_update_source(struct spa_source *source)
{
	return SPA_RESULT_OK;
}

static void do_remove_source(struct spa_source *source)
{
}

int main(int argc, char *argv[])
{
	struct data data;
	int res;
	void *handle;
	spa_handle_factory_enum_func_t enum_func;
	uint32_t index = 0;

	if (argc < 2) {
		printf("usage: %s <plugin.so>\n", argv[0]);
		return -1;
	}

	data.map = &default_map.map;
	data.log = &default_log.log;
	data.loop.version = SPA_VERSION_LOOP;
	data.loop.add_source = do_add_source;
	data.loop.update_source = do_update_source;
	data.loop.remove_source = do_remove_source;

	spa_debug_set_type_map(data.map);

	data.support[0].type = SPA_TYPE__TypeMap;
	data.support[0].data = data.map;
	data.support[1].type = SPA_TYPE__Log;
	data.support[1].data = data.log;
	data.support[2].type = SPA_TYPE_LOOP__MainLoop;
	data.support[2].data = &data.loop;
	data.support[3].type = SPA_TYPE_LOOP__DataLoop;
	data.support[3].data = &data.loop;
	data.n_support = 4;

	data.type.node = spa_type_map_get_id(data.map, SPA_TYPE__Node);
	data.type.clock = spa_type_map_get_id(data.map, SPA_TYPE__Clock);

	if ((handle = dlopen(argv[1], RTLD_NOW)) == NULL) {
		printf("can't load %s\n", argv[1]);
		return -1;
	}
	if ((enum_func = dlsym(handle, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		printf("can't find function\n");
		return -1;
	}

	while (true) {
		const struct spa_handle_factory *factory;

		if ((res = enum_func(&factory, index)) < 0) {
			if (res != SPA_RESULT_ENUM_END)
				printf("can't enumerate factories: %d\n", res);
			break;
		}
		inspect_factory(&data, factory);
		index++;
	}

	return 0;
}
