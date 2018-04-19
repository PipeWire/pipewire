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
#include <spa/buffer/alloc.h>
#include <spa/param/param.h>
#include <spa/param/buffers.h>
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
	struct spa_type_io io;
	struct spa_type_param param;
	struct spa_type_param_buffers param_buffers;
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
	spa_type_io_map(map, &type->io);
	spa_type_param_map(map, &type->param);
	spa_type_param_buffers_map(map, &type->param_buffers);
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

struct node {
	struct spa_node *node;
};

struct link {
	struct node *out_node;
	uint32_t out_port;
	const struct spa_port_info *out_info;
	struct node *in_node;
	uint32_t in_port;
	const struct spa_port_info *in_info;
	struct spa_io_buffers io;
	uint32_t n_buffers;
	struct spa_buffer **buffers;
};

struct data {
	struct spa_type_map *map;
	struct spa_log *log;
	struct type type;

	struct spa_support support[4];
	uint32_t n_support;

	struct node nodes[4];
	struct link links[5];
};

static int make_node(struct data *data, struct spa_node **node, const char *lib, const char *name)
{
	struct type *t = &data->type;
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

		handle = calloc(1, spa_handle_factory_get_size(factory, NULL));
		if ((res =
		     spa_handle_factory_init(factory, handle, NULL, data->support,
					     data->n_support)) < 0) {
			printf("can't make factory instance: %d\n", res);
			return res;
		}
		if ((res = spa_handle_get_interface(handle, t->node, &iface)) < 0) {
			printf("can't get interface %d\n", res);
			return res;
		}
		*node = iface;
		return 0;
	}
	return -EBADF;
}

static int make_nodes(struct data *data, const char *device)
{
	int res;

	if ((res = make_node(data, &data->nodes[0].node,
			     "build/spa/plugins/audioconvert/libspa-audioconvert.so",
			     "fmtconvert")) < 0) {
		printf("can't create fmtconvert: %d\n", res);
		return res;
	}
	if ((res = make_node(data, &data->nodes[1].node,
			     "build/spa/plugins/audioconvert/libspa-audioconvert.so",
			     "channelmix")) < 0) {
		printf("can't create channelmix: %d\n", res);
		return res;
	}
	if ((res = make_node(data, &data->nodes[2].node,
			     "build/spa/plugins/audioconvert/libspa-audioconvert.so",
			     "resample")) < 0) {
		printf("can't create resample: %d\n", res);
		return res;
	}
	if ((res = make_node(data, &data->nodes[3].node,
			     "build/spa/plugins/audioconvert/libspa-audioconvert.so",
			     "fmtconvert")) < 0) {
		printf("can't create fmtconvert: %d\n", res);
		return res;
	}

	return res;
}

static int make_link(struct data *data, struct link *link,
		     struct node *out_node, uint32_t out_port,
		     struct node *in_node, uint32_t in_port)
{
	struct type *t = &data->type;

	link->out_node = out_node;
	link->out_port = out_port;
	link->in_node = in_node;
	link->in_port = in_port;
	link->io = SPA_IO_BUFFERS_INIT;
	link->n_buffers = 0;
	link->buffers = NULL;
	link->in_info = NULL;
	link->out_info = NULL;

	if (out_node != NULL) {
		spa_node_port_get_info(out_node->node,
				       SPA_DIRECTION_OUTPUT, out_port,
				       &link->out_info);
		spa_node_port_set_io(out_node->node,
				     SPA_DIRECTION_OUTPUT, out_port,
				     t->io.Buffers,
				     &link->io, sizeof(link->io));
	}
	if (in_node != NULL) {
		spa_node_port_get_info(in_node->node,
				       SPA_DIRECTION_INPUT, in_port,
				       &link->in_info);
		spa_node_port_set_io(in_node->node,
				     SPA_DIRECTION_INPUT, in_port,
				     t->io.Buffers,
				     &link->io, sizeof(link->io));
	}
	return 0;
}

static int link_nodes(struct data *data)
{
	make_link(data, &data->links[0], NULL, 0, &data->nodes[0], 0);
	make_link(data, &data->links[1], &data->nodes[0], 0, &data->nodes[1], 0);
	make_link(data, &data->links[2], &data->nodes[1], 0, &data->nodes[2], 0);
	make_link(data, &data->links[3], &data->nodes[2], 0, &data->nodes[3], 0);
	make_link(data, &data->links[4], &data->nodes[3], 0, NULL, 0);
	return 0;
}

static int negotiate_link_format(struct data *data, struct link *link, struct spa_pod *filter)
{
	struct type *t = &data->type;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];
	uint32_t state;
	struct spa_pod *format;
	int res;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (link->out_node != NULL) {
		state = 0;
		if ((res = spa_node_port_enum_params(link->out_node->node,
				       SPA_DIRECTION_OUTPUT, link->out_port,
				       t->param.idEnumFormat, &state,
				       filter, &format, &b)) <= 0)
			return -ENOTSUP;

		filter = format;
	}
	if (link->in_node != NULL) {
		state = 0;
		if ((res = spa_node_port_enum_params(link->in_node->node,
				       SPA_DIRECTION_INPUT, link->in_port,
				       t->param.idEnumFormat, &state,
				       filter, &format, &b)) <= 0)
			return -ENOTSUP;

		filter = format;
	}

	spa_pod_fixate(filter);
	spa_debug_pod(filter, SPA_DEBUG_FLAG_FORMAT);

	if (link->out_node != NULL) {
		if ((res = spa_node_port_set_param(link->out_node->node,
					   SPA_DIRECTION_OUTPUT, link->out_port,
					   t->param.idFormat, 0,
					   filter)) < 0)
			return res;
	}
	if (link->in_node != NULL) {
		if ((res = spa_node_port_set_param(link->in_node->node,
					   SPA_DIRECTION_INPUT, link->in_port,
					   t->param.idFormat, 0,
					   filter)) < 0)
			return res;
	}
	return 0;
}

static int negotiate_formats(struct data *data)
{
	int res;
	struct spa_pod *format;
	struct spa_pod_builder b = { 0 };
	struct type *t = &data->type;
	uint8_t buffer[4096];

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	format = spa_pod_builder_object(&b,
		0, t->format,
		"I", t->media_type.audio,
		"I", t->media_subtype.raw,
		":", t->format_audio.format,   "I", t->audio_format.S16,
		":", t->format_audio.layout,   "i", SPA_AUDIO_LAYOUT_INTERLEAVED,
		":", t->format_audio.rate,     "i", 44100,
		":", t->format_audio.channels, "i", 2);

	if ((res = negotiate_link_format(data, &data->links[0], format)) < 0)
		return res;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	format = spa_pod_builder_object(&b,
		0, t->format,
		"I", t->media_type.audio,
		"I", t->media_subtype.raw,
		":", t->format_audio.format,   "I", t->audio_format.F32,
		":", t->format_audio.layout,   "i", SPA_AUDIO_LAYOUT_NON_INTERLEAVED,
		":", t->format_audio.rate,     "i", 48000,
		":", t->format_audio.channels, "i", 1);

	if ((res = negotiate_link_format(data, &data->links[4], format)) < 0)
		return res;

	if ((res = negotiate_link_format(data, &data->links[3], NULL)) < 0)
		return res;

	if ((res = negotiate_link_format(data, &data->links[1], NULL)) < 0)
		return res;

	if ((res = negotiate_link_format(data, &data->links[2], NULL)) < 0)
		return res;

	return 0;
}

static int negotiate_link_buffers(struct data *data, struct link *link)
{
	struct type *t = &data->type;
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t state;
	struct spa_pod *param = NULL;
	int res, i;
	bool in_alloc, out_alloc;
	int32_t size, buffers, blocks, align, flags;
	uint32_t *aligns;
	struct spa_data *datas;

	if (link->out_node != NULL) {
		state = 0;
		if ((res = spa_node_port_enum_params(link->out_node->node,
				       SPA_DIRECTION_OUTPUT, link->out_port,
				       t->param.idBuffers, &state,
				       param, &param, &b)) <= 0)
			return -ENOTSUP;
	}
	if (link->in_node != NULL) {
		state = 0;
		if ((res = spa_node_port_enum_params(link->in_node->node,
				       SPA_DIRECTION_INPUT, link->in_port,
				       t->param.idBuffers, &state,
				       param, &param, &b)) <= 0)
			return -ENOTSUP;
	}

	spa_pod_fixate(param);
	spa_debug_pod(param, 0);

	if (link->in_info)
		in_alloc = SPA_FLAG_CHECK(link->in_info->flags, SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS);
	else
		in_alloc = false;

	if (link->out_info)
		out_alloc = SPA_FLAG_CHECK(link->out_info->flags, SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS);
	else
		out_alloc = false;

	flags = 0;
	if (out_alloc || in_alloc) {
		flags |= SPA_BUFFER_ALLOC_FLAG_NO_DATA;
		if (out_alloc)
			in_alloc = false;
	}

	if (spa_pod_object_parse(param,
		":", t->param_buffers.buffers, "i", &buffers,
		":", t->param_buffers.blocks, "i", &blocks,
		":", t->param_buffers.size, "i", &size,
		":", t->param_buffers.align, "i", &align,
		NULL) < 0)
		return -EINVAL;

	datas = alloca(sizeof(struct spa_data) * blocks);
	aligns = alloca(sizeof(uint32_t) * blocks);
	for (i = 0; i < blocks; i++) {
		datas[i].type = t->data.MemPtr;
		datas[i].maxsize = size;
		aligns[i] = align;
	}

	link->buffers = spa_buffer_alloc_array(buffers, flags, 0, NULL, blocks, datas, aligns);
	if (link->buffers == NULL)
		return -ENOMEM;

	link->n_buffers = buffers;

	if (link->out_node != NULL) {
		if (out_alloc) {
			if ((res = spa_node_port_alloc_buffers(link->out_node->node,
				       SPA_DIRECTION_OUTPUT, link->out_port,
				       NULL, 0,
				       link->buffers, &link->n_buffers)) < 0)
				return res;
		}
		else {
			if ((res = spa_node_port_use_buffers(link->out_node->node,
				       SPA_DIRECTION_OUTPUT, link->out_port,
				       link->buffers, link->n_buffers)) < 0)
				return res;
		}
	}
	if (link->in_node != NULL) {
		if (in_alloc) {
			if ((res = spa_node_port_alloc_buffers(link->in_node->node,
				       SPA_DIRECTION_INPUT, link->in_port,
				       NULL, 0,
				       link->buffers, &link->n_buffers)) < 0)
				return res;
		}
		else {
			if ((res = spa_node_port_use_buffers(link->in_node->node,
				       SPA_DIRECTION_INPUT, link->in_port,
				       link->buffers, link->n_buffers)) < 0)
				return res;
		}
	}

	return 0;
}

static int negotiate_buffers(struct data *data)
{
	int res;

	if ((res = negotiate_link_buffers(data, &data->links[0])) < 0)
		return res;
	if ((res = negotiate_link_buffers(data, &data->links[1])) < 0)
		return res;
	if ((res = negotiate_link_buffers(data, &data->links[2])) < 0)
		return res;
	if ((res = negotiate_link_buffers(data, &data->links[3])) < 0)
		return res;
	if ((res = negotiate_link_buffers(data, &data->links[4])) < 0)
		return res;

	return 0;
}

static void fill_buffer(struct data *data, struct spa_buffer *buffers[], int id)
{
	int i;
	struct spa_buffer *b = buffers[id];

	for (i = 0; i < b->datas[0].maxsize; i++) {
		*SPA_MEMBER(b->datas[0].data, i, uint8_t) = i;
	}
	b->datas[0].chunk->size = b->datas[0].maxsize;
}

static void run_convert(struct data *data)
{
	struct type *t = &data->type;
	struct spa_buffer *b;
	int res, i, j;

	{
		struct spa_command cmd = SPA_COMMAND_INIT(t->command_node.Start);
		for (i = 0; i < 4; i++) {
			if ((res = spa_node_send_command(data->nodes[i].node, &cmd)) < 0)
				printf("got command error %d\n", res);
		}
	}

	fill_buffer(data, data->links[0].buffers, 0);

	for (i = 0; i < 5; i++) {
		data->links[i].io.status = SPA_STATUS_NEED_BUFFER;
		data->links[i].io.buffer_id = SPA_ID_INVALID;
	}


	b = data->links[0].buffers[0];
	for (i = 0; i < b->n_datas; i++)
		spa_debug_dump_mem(b->datas[i].data, b->datas[i].maxsize);

	for (j = 0; j < 2; j++) {
		data->links[0].io.status = SPA_STATUS_HAVE_BUFFER;
		data->links[0].io.buffer_id = 0;
		for (i = 0; i < 4; i++) {
			res = spa_node_process(data->nodes[i].node);
			printf("called process %d\n", res);
		}
	}

	b = data->links[3].buffers[0];
	for (i = 0; i < b->n_datas; i++)
		spa_debug_dump_mem(b->datas[i].data, b->datas[i].maxsize);

	{
		struct spa_command cmd = SPA_COMMAND_INIT(t->command_node.Pause);
		for (i = 0; i < 4; i++) {
			if ((res = spa_node_send_command(data->nodes[i].node, &cmd)) < 0)
				printf("got command error %d\n", res);
		}
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
	if ((res = link_nodes(&data)) < 0) {
		printf("can't link nodes: %d\n", res);
		return -1;
	}
	if ((res = negotiate_formats(&data)) < 0) {
		printf("can't negotiate nodes: %d\n", res);
		return -1;
	}
	if ((res = negotiate_buffers(&data)) < 0) {
		printf("can't negotiate buffers: %d\n", res);
		return -1;
	}

	run_convert(&data);
}
