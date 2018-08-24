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
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/buffer/alloc.h>
#include <spa/param/param.h>
#include <spa/param/buffers.h>
#include <spa/param/props.h>
#include <spa/param/audio/format-utils.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>
#include <spa/debug/mem.h>
#include <spa/debug/types.h>

static SPA_LOG_IMPL(default_log);

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
	struct spa_log *log;

	struct spa_support support[4];
	uint32_t n_support;

	struct node nodes[4];
	struct link links[5];
};

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

		handle = calloc(1, spa_handle_factory_get_size(factory, NULL));
		if ((res =
		     spa_handle_factory_init(factory, handle, NULL, data->support,
					     data->n_support)) < 0) {
			printf("can't make factory instance: %d\n", res);
			return res;
		}
		if ((res = spa_handle_get_interface(handle, SPA_ID_INTERFACE_Node, &iface)) < 0) {
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
				     SPA_ID_IO_Buffers,
				     &link->io, sizeof(link->io));
	}
	if (in_node != NULL) {
		spa_node_port_get_info(in_node->node,
				       SPA_DIRECTION_INPUT, in_port,
				       &link->in_info);
		spa_node_port_set_io(in_node->node,
				     SPA_DIRECTION_INPUT, in_port,
				     SPA_ID_IO_Buffers,
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
				       SPA_ID_PARAM_EnumFormat, &state,
				       filter, &format, &b)) <= 0)
			return -ENOTSUP;

		filter = format;
	}
	if (link->in_node != NULL) {
		state = 0;
		if ((res = spa_node_port_enum_params(link->in_node->node,
				       SPA_DIRECTION_INPUT, link->in_port,
				       SPA_ID_PARAM_EnumFormat, &state,
				       filter, &format, &b)) <= 0)
			return -ENOTSUP;

		filter = format;
	}

	spa_pod_fixate(filter);
	spa_debug_format(0, NULL, filter);

	if (link->out_node != NULL) {
		if ((res = spa_node_port_set_param(link->out_node->node,
					   SPA_DIRECTION_OUTPUT, link->out_port,
					   SPA_ID_PARAM_Format, 0,
					   filter)) < 0)
			return res;
	}
	if (link->in_node != NULL) {
		if ((res = spa_node_port_set_param(link->in_node->node,
					   SPA_DIRECTION_INPUT, link->in_port,
					   SPA_ID_PARAM_Format, 0,
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
	uint8_t buffer[4096];

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	format = spa_pod_builder_object(&b,
		0, SPA_ID_OBJECT_Format,
		"I", SPA_MEDIA_TYPE_audio,
		"I", SPA_MEDIA_SUBTYPE_raw,
		":", SPA_FORMAT_AUDIO_format,   "I", SPA_AUDIO_FORMAT_S16,
		":", SPA_FORMAT_AUDIO_layout,   "i", SPA_AUDIO_LAYOUT_INTERLEAVED,
		":", SPA_FORMAT_AUDIO_rate,     "i", 44100,
		":", SPA_FORMAT_AUDIO_channels, "i", 2);

	if ((res = negotiate_link_format(data, &data->links[0], format)) < 0)
		return res;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	format = spa_pod_builder_object(&b,
		0, SPA_ID_OBJECT_Format,
		"I", SPA_MEDIA_TYPE_audio,
		"I", SPA_MEDIA_SUBTYPE_raw,
		":", SPA_FORMAT_AUDIO_format,   "I", SPA_AUDIO_FORMAT_F32,
		":", SPA_FORMAT_AUDIO_layout,   "i", SPA_AUDIO_LAYOUT_NON_INTERLEAVED,
		":", SPA_FORMAT_AUDIO_rate,     "i", 48000,
		":", SPA_FORMAT_AUDIO_channels, "i", 1);

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
				       SPA_ID_PARAM_Buffers, &state,
				       param, &param, &b)) <= 0)
			return -ENOTSUP;
	}
	if (link->in_node != NULL) {
		state = 0;
		if ((res = spa_node_port_enum_params(link->in_node->node,
				       SPA_DIRECTION_INPUT, link->in_port,
				       SPA_ID_PARAM_Buffers, &state,
				       param, &param, &b)) <= 0)
			return -ENOTSUP;
	}

	spa_pod_fixate(param);
	spa_debug_pod(0, spa_debug_types, param);

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
		":", SPA_PARAM_BUFFERS_buffers, "i", &buffers,
		":", SPA_PARAM_BUFFERS_blocks, "i", &blocks,
		":", SPA_PARAM_BUFFERS_size, "i", &size,
		":", SPA_PARAM_BUFFERS_align, "i", &align,
		NULL) < 0)
		return -EINVAL;

	datas = alloca(sizeof(struct spa_data) * blocks);
	aligns = alloca(sizeof(uint32_t) * blocks);
	for (i = 0; i < blocks; i++) {
		datas[i].type = SPA_DATA_MemPtr;
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
	struct spa_buffer *b;
	int res, i, j;

	{
		struct spa_command cmd = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
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
		spa_debug_mem(0, b->datas[i].data, b->datas[i].maxsize);

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
		spa_debug_mem(0, b->datas[i].data, b->datas[i].maxsize);

	{
		struct spa_command cmd = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
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

	data.log = &default_log.log;

	if ((str = getenv("SPA_DEBUG")))
		data.log->level = atoi(str);

	data.support[0] = SPA_SUPPORT_INIT(SPA_ID_INTERFACE_Log, data.log);
	data.n_support = 1;

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
