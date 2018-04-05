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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>

#include <spa/support/log-impl.h>
#include <spa/support/loop.h>
#include <spa/support/type-map-impl.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/format-utils.h>

#include <lib/debug.h>

static SPA_TYPE_MAP_IMPL(default_map, 4096);
static SPA_LOG_IMPL(default_log);

struct type {
	uint32_t node;
	uint32_t props;
	uint32_t format;
	uint32_t props_device;
	uint32_t props_freq;
	uint32_t props_volume;
	uint32_t props_min_latency;
	uint32_t props_live;
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
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->node = spa_type_map_get_id(map, SPA_TYPE__Node);
	type->props = spa_type_map_get_id(map, SPA_TYPE__Props);
	type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
	type->props_device = spa_type_map_get_id(map, SPA_TYPE_PROPS__device);
	type->props_freq = spa_type_map_get_id(map, SPA_TYPE_PROPS__frequency);
	type->props_volume = spa_type_map_get_id(map, SPA_TYPE_PROPS__volume);
	type->props_min_latency = spa_type_map_get_id(map, SPA_TYPE_PROPS__minLatency);
	type->props_live = spa_type_map_get_id(map, SPA_TYPE_PROPS__live);
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
}

struct buffer {
	struct spa_buffer buffer;
	struct spa_meta metas[1];
	struct spa_meta_header header;
	struct spa_data datas[8];
	struct spa_chunk chunks[8];
};

struct data {
	struct spa_type_map *map;
	struct spa_log *log;
	struct type type;

	struct spa_support support[4];
	uint32_t n_support;

	struct spa_node *conv;
	struct spa_io_buffers io_in[1];
	struct spa_io_buffers io_out[1];

	struct spa_buffer *in_buffers[1];
	struct buffer in_buffer[1];
	struct spa_buffer *out_buffers[1];
	struct buffer out_buffer[1];
};

#define BUFFER_SIZE     128

static void
init_buffer(struct data *data, struct spa_buffer **bufs, struct buffer *ba, int n_buffers,
	    size_t size, int n_datas)
{
	int i, j;
	void *d;

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &ba[i];
		bufs[i] = &b->buffer;

		b->buffer.id = i;
		b->buffer.metas = b->metas;
		b->buffer.n_metas = 1;
		b->buffer.datas = b->datas;
		b->buffer.n_datas = n_datas;

		b->header.flags = 0;
		b->header.seq = 0;
		b->header.pts = 0;
		b->header.dts_offset = 0;
		b->metas[0].type = data->type.meta.Header;
		b->metas[0].data = &b->header;
		b->metas[0].size = sizeof(b->header);

		d = malloc(size * n_datas);

		for (j = 0; j < n_datas; j++) {
			b->datas[j].type = data->type.data.MemPtr;
			b->datas[j].flags = 0;
			b->datas[j].fd = -1;
			b->datas[j].mapoffset = 0;
			b->datas[j].maxsize = size;
			b->datas[j].data = SPA_MEMBER(d, size * j, void);
			b->datas[j].chunk = &b->chunks[0];
			b->datas[j].chunk->offset = 0;
			b->datas[j].chunk->size = 0;
			b->datas[j].chunk->stride = 0;
		}
	}
}

static int make_node(struct data *data, struct spa_node **node, const char *lib, const char *name)
{
	struct spa_handle *handle;
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
		void *iface;

		if ((res = enum_func(&factory, &i)) <= 0) {
			if (res != 0)
				printf("can't enumerate factories: %s\n", spa_strerror(res));
			break;
		}
		if (strcmp(factory->name, name))
			continue;

		handle = calloc(1, factory->size);
		if ((res =
		     spa_handle_factory_init(factory, handle, NULL, data->support,
					     data->n_support)) < 0) {
			printf("can't make factory instance: %d\n", res);
			return res;
		}
		if ((res = spa_handle_get_interface(handle, data->type.node, &iface)) < 0) {
			printf("can't get interface %d\n", res);
			return res;
		}
		*node = iface;
		return 0;
	}
	return -EBADF;
}

static void on_conv_done(void *data, int seq, int res)
{
	printf("got done %d %d\n", seq, res);
}

static void on_conv_event(void *data, struct spa_event *event)
{
	printf("got event %d\n", SPA_EVENT_TYPE(event));
}

static void on_conv_process(void *_data, int status)
{
	printf("got process\n");
}

static void
on_conv_reuse_buffer(void *_data,
		     uint32_t port_id,
		     uint32_t buffer_id)
{
	printf("got reuse buffer\n");
}

static const struct spa_node_callbacks conv_callbacks = {
	SPA_VERSION_NODE_CALLBACKS,
	.done = on_conv_done,
	.event = on_conv_event,
	.process = on_conv_process,
	.reuse_buffer = on_conv_reuse_buffer
};

static int make_nodes(struct data *data, const char *device)
{
	int res;

	if ((res = make_node(data, &data->conv,
			     "build/spa/plugins/audioconvert/libspa-audioconvert.so", "fmtconvert")) < 0) {
		printf("can't create fmtconvert: %d\n", res);
		return res;
	}
	spa_node_set_callbacks(data->conv, &conv_callbacks, data);

	return res;
}

static int negotiate_formats(struct data *data)
{
	int res;
	struct spa_pod *format;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	format = spa_pod_builder_object(&b,
		0, data->type.format,
		"I", data->type.media_type.audio,
		"I", data->type.media_subtype.raw,
		":", data->type.format_audio.format,   "I", data->type.audio_format.S16,
		":", data->type.format_audio.layout,   "i", SPA_AUDIO_LAYOUT_INTERLEAVED,
		":", data->type.format_audio.rate,     "i", 44100,
		":", data->type.format_audio.channels, "i", 2);

	if ((res = spa_node_port_set_param(data->conv,
					   SPA_DIRECTION_INPUT, 0,
					   data->type.param.idFormat, 0,
					   format)) < 0)
		return res;

	data->io_in[0] = SPA_IO_BUFFERS_INIT;
	data->io_out[0] = SPA_IO_BUFFERS_INIT;

	spa_node_port_set_io(data->conv,
			     SPA_DIRECTION_INPUT, 0,
			     data->type.io.Buffers,
			     &data->io_in[0], sizeof(data->io_in[0]));
	spa_node_port_set_io(data->conv,
			     SPA_DIRECTION_OUTPUT, 0,
			     data->type.io.Buffers,
			     &data->io_out[0], sizeof(data->io_out[0]));


	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	format = spa_pod_builder_object(&b,
		0, data->type.format,
		"I", data->type.media_type.audio,
		"I", data->type.media_subtype.raw,
		":", data->type.format_audio.format,   "I", data->type.audio_format.F32,
		":", data->type.format_audio.layout,   "i", SPA_AUDIO_LAYOUT_NON_INTERLEAVED,
		":", data->type.format_audio.rate,     "i", 44100,
		":", data->type.format_audio.channels, "i", 2);


	if ((res = spa_node_port_set_param(data->conv,
					   SPA_DIRECTION_OUTPUT, 0,
					   data->type.param.idFormat, 0,
					   format)) < 0)
		return res;

	init_buffer(data, data->in_buffers, data->in_buffer, 1, BUFFER_SIZE, 1);
	if ((res =
	     spa_node_port_use_buffers(data->conv, SPA_DIRECTION_INPUT, 0, data->in_buffers,
				       1)) < 0)
		return res;

	init_buffer(data, data->out_buffers, data->out_buffer, 1, BUFFER_SIZE / 2, 2);
	if ((res =
	     spa_node_port_use_buffers(data->conv, SPA_DIRECTION_OUTPUT, 0, data->out_buffers,
				       1)) < 0)
		return res;

	return 0;
}

static void fill_buffer(struct data *data, struct spa_buffer *buffers[], int id)
{
	int i;

	for (i = 0; i < BUFFER_SIZE; i++) {
		*SPA_MEMBER(buffers[id]->datas[0].data, i, uint8_t) = i;
	}
	buffers[id]->datas[0].chunk->size = BUFFER_SIZE;
}

static void run_convert(struct data *data)
{
	int res;

	{
		struct spa_command cmd = SPA_COMMAND_INIT(data->type.command_node.Start);
		if ((res = spa_node_send_command(data->conv, &cmd)) < 0)
			printf("got convert error %d\n", res);
	}

	fill_buffer(data, data->in_buffers, 0);

	data->io_in[0].status = SPA_STATUS_HAVE_BUFFER;
	data->io_in[0].buffer_id = 0;

	data->io_out[0].status = SPA_STATUS_NEED_BUFFER;
	data->io_out[0].buffer_id = 0;

	spa_debug_dump_mem(data->in_buffers[0]->datas[0].data, BUFFER_SIZE);

	res = spa_node_process(data->conv);
	printf("called process %d\n", res);

	spa_debug_dump_mem(data->out_buffers[0]->datas[0].data, BUFFER_SIZE / 2);
	spa_debug_dump_mem(data->out_buffers[0]->datas[1].data, BUFFER_SIZE / 2);

	{
		struct spa_command cmd = SPA_COMMAND_INIT(data->type.command_node.Pause);
		if ((res = spa_node_send_command(data->conv, &cmd)) < 0)
			printf("got convert error %d\n", res);
	}
}

int main(int argc, char *argv[])
{
	struct data data = { NULL };
	int res;
	const char *str;

	data.map = &default_map.map;
	data.log = &default_log.log;

	if ((str = getenv("SPA_DEBUG")))
		data.log->level = atoi(str);

	data.support[0].type = SPA_TYPE__TypeMap;
	data.support[0].data = data.map;
	data.support[1].type = SPA_TYPE__Log;
	data.support[1].data = data.log;
	data.n_support = 2;

	init_type(&data.type, data.map);
	spa_debug_set_type_map(data.map);

	if ((res = make_nodes(&data, argc > 1 ? argv[1] : NULL)) < 0) {
		printf("can't make nodes: %d\n", res);
		return -1;
	}
	if ((res = negotiate_formats(&data)) < 0) {
		printf("can't negotiate nodes: %d\n", res);
		return -1;
	}

	run_convert(&data);
}
