/* Spa
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#include <math.h>
#include <error.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>

#include <spa/support/loop.h>
#include <spa/support/log.h>
#include <spa/support/type-map.h>
#include <spa/support/dbus.h>
#include <spa/monitor/monitor.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/format-utils.h>
#include <spa/param/io.h>

#define M_PI_M2 ( M_PI + M_PI )

static struct spa_log *logger;

#define spa_debug(f,...) spa_log_trace(logger, f, __VA_ARGS__)

#include <spa/graph/graph.h>
#include <spa/graph/graph-scheduler2.h>

#include <lib/debug.h>

struct type {
	uint32_t log;
	uint32_t node;
	uint32_t props;
	uint32_t format;
	struct spa_type_monitor monitor;
	struct spa_type_io io;
	struct spa_type_param param;
	struct spa_type_meta meta;
	struct spa_type_data data;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_audio format_audio;
	struct spa_type_audio_format audio_format;
	struct spa_type_event_node event_node;
	struct spa_type_command_node command_node;
	struct spa_type_param_io param_io;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->log = spa_type_map_get_id(map, SPA_TYPE__Log);
	type->node = spa_type_map_get_id(map, SPA_TYPE__Node);
	type->props = spa_type_map_get_id(map, SPA_TYPE__Props);
	type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
	spa_type_monitor_map(map, &type->monitor);
	spa_type_io_map(map, &type->io);
	spa_type_param_map(map, &type->param);
	spa_type_meta_map(map, &type->meta);
	spa_type_data_map(map, &type->data);
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_audio_map(map, &type->format_audio);
	spa_type_audio_format_map(map, &type->audio_format);
	spa_type_event_node_map(map, &type->event_node);
	spa_type_command_node_map(map, &type->command_node);
	spa_type_param_io_map(map, &type->param_io);
}

struct buffer {
	struct spa_buffer buffer;
	struct spa_meta metas[1];
	struct spa_meta_header header;
	struct spa_data datas[1];
	struct spa_chunk chunks[1];
};

struct data {
	struct type type;
	struct spa_type_map *map;
	struct spa_log *log;

	struct spa_loop *loop;
	struct spa_loop_control *loop_control;
	struct spa_loop_utils *loop_utils;
	bool running;

	struct spa_dbus *dbus;

	struct spa_support support[7];
	uint32_t n_support;

	struct spa_monitor *monitor;

	struct spa_graph graph;
	struct spa_graph_state graph_state;
	struct spa_graph_data graph_data;
	struct spa_graph_node source_node;
	struct spa_graph_port source_out;
	struct spa_graph_port sink_in;
	struct spa_graph_node sink_node;

	struct spa_node *sink;
	struct spa_node *source;

	struct spa_io_buffers source_sink_io[1];
	struct spa_buffer *source_buffers[1];
	struct buffer source_buffer[1];
};

static void inspect_item(struct data *data, struct spa_pod *item)
{
        spa_debug_pod(item, 0);
}

static void monitor_event(void *_data, struct spa_event *event)
{
        struct data *data = _data;

        if (SPA_EVENT_TYPE(event) == data->type.monitor.Added) {
                fprintf(stderr, "added:\n");
                inspect_item(data, SPA_POD_CONTENTS(struct spa_event, event));
        } else if (SPA_EVENT_TYPE(event) == data->type.monitor.Removed) {
                fprintf(stderr, "removed:\n");
                inspect_item(data, SPA_POD_CONTENTS(struct spa_event, event));
        } else if (SPA_EVENT_TYPE(event) == data->type.monitor.Changed) {
                fprintf(stderr, "changed:\n");
                inspect_item(data, SPA_POD_CONTENTS(struct spa_event, event));
        }
}

static struct spa_monitor_callbacks monitor_callbacks = {
	SPA_VERSION_MONITOR_CALLBACKS,
	monitor_event,
};

static int get_handle(struct data *data,
		      struct spa_handle **handle,
		      const char *lib,
		      const char *name)
{
	int res;
	void *hnd;
	spa_handle_factory_enum_func_t enum_func;
	uint32_t i;

	if ((hnd = dlopen(lib, RTLD_NOW)) == NULL) {
		printf("can't load %s: %s\n", lib, dlerror());
		return -errno;
	}
	if ((enum_func = dlsym(hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		printf("can't find enum function\n");
		return -errno;
	}

	for (i = 0;;) {
		const struct spa_handle_factory *factory;

		if ((res = enum_func(&factory, &i)) <= 0) {
			if (res != 0)
				printf("can't enumerate factories: %s\n", spa_strerror(res));
			break;
		}
		if (strcmp(factory->name, name))
			continue;

		*handle = calloc(1, factory->size);
		if ((res = spa_handle_factory_init(factory, *handle, NULL,
						   data->support,
						   data->n_support)) < 0) {
			printf("can't make factory instance: %d\n", res);
			free(*handle);
			return res;
		}
		return 0;
	}
	return -ENOENT;
}

int main(int argc, char *argv[])
{
	struct data data;
	int res;
	const char *str;
	struct spa_handle *handle;
	void *iface;

	spa_zero(data);
	if ((res = get_handle(&data, &handle,
			     "build/spa/plugins/support/libspa-support.so",
			     "mapper")) < 0) {
		error(-1, res, "can't create mapper");
	}
	if ((res = spa_handle_get_interface(handle, 0, &iface)) < 0)
		error(-1, res, "can't get mapper interface");

	data.map = iface;
	data.support[0].type = SPA_TYPE__TypeMap;
	data.support[0].data = data.map;
	data.n_support = 1;
	init_type(&data.type, data.map);
	spa_debug_set_type_map(data.map);

	if ((res = get_handle(&data, &handle,
			     "build/spa/plugins/support/libspa-support.so",
			     "logger")) < 0) {
		error(-1, res, "can't create logger");
	}

	if ((res = spa_handle_get_interface(handle,
					    spa_type_map_get_id(data.map, SPA_TYPE__Log),
					    &iface)) < 0)
		error(-1, res, "can't get log interface");

	data.log = iface;
	data.support[1].type = SPA_TYPE__Log;
	data.support[1].data = data.log;
	data.n_support = 2;

	if ((str = getenv("SPA_DEBUG")))
		data.log->level = atoi(str);

	if ((res = get_handle(&data, &handle,
			     "build/spa/plugins/support/libspa-support.so",
			     "loop")) < 0) {
		error(-1, res, "can't create loop");
	}
	if ((res = spa_handle_get_interface(handle,
					    spa_type_map_get_id(data.map, SPA_TYPE__Loop),
					    &iface)) < 0)
		error(-1, res, "can't get loop interface");
	data.loop = iface;

	if ((res = spa_handle_get_interface(handle,
					    spa_type_map_get_id(data.map, SPA_TYPE__LoopControl),
					    &iface)) < 0)
		error(-1, res, "can't get loopcontrol interface");
	data.loop_control = iface;

	if ((res = spa_handle_get_interface(handle,
					    spa_type_map_get_id(data.map, SPA_TYPE__LoopUtils),
					    &iface)) < 0)
		error(-1, res, "can't get looputils interface");
	data.loop_utils = iface;

	data.support[2].type = SPA_TYPE_LOOP__DataLoop;
	data.support[2].data = data.loop;
	data.support[3].type = SPA_TYPE_LOOP__MainLoop;
	data.support[3].data = data.loop;
	data.support[4].type = SPA_TYPE__LoopControl;
	data.support[4].data = data.loop_control;
	data.support[5].type = SPA_TYPE__LoopUtils;
	data.support[5].data = data.loop_utils;
	data.n_support = 6;

	if ((res = get_handle(&data, &handle,
			     "build/spa/plugins/support/libspa-dbus.so",
			     "dbus")) < 0) {
		error(-1, res, "can't create dbus");
	}

	if ((res = spa_handle_get_interface(handle,
					    spa_type_map_get_id(data.map, SPA_TYPE__DBus),
					    &iface)) < 0)
		error(-1, res, "can't get dbus interface");

	data.dbus = iface;
	data.support[6].type = SPA_TYPE__DBus;
	data.support[6].data = data.dbus;
	data.n_support = 7;

	if ((res = get_handle(&data, &handle,
			     "build/spa/plugins/bluez5/libspa-bluez5.so",
			     "bluez5-monitor")) < 0) {
		error(-1, res, "can't create bluez5-monitor");
	}

	if ((res = spa_handle_get_interface(handle,
					    spa_type_map_get_id(data.map, SPA_TYPE__Monitor),
					    &iface)) < 0)
		error(-1, res, "can't get monitor interface");

	data.monitor = iface;

	spa_graph_init(&data.graph, &data.graph_state);
	spa_graph_data_init(&data.graph_data, &data.graph);
	spa_graph_set_callbacks(&data.graph, &spa_graph_impl_default, &data.graph_data);

	spa_monitor_set_callbacks(data.monitor, &monitor_callbacks, &data);

	data.running = true;
	spa_loop_control_enter(data.loop_control);
	while (data.running) {
		spa_loop_control_iterate(data.loop_control, -1);
	}
	spa_loop_control_leave(data.loop_control);

	return -1;
}
