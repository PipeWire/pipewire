/* PipeWire
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

#include <stdio.h>
#include <sys/mman.h>

#include <SDL2/SDL.h>

#include <spa/type-map.h>
#include <spa/format-utils.h>
#include <spa/video/format-utils.h>
#include <spa/format-builder.h>
#include <spa/props.h>
#include <spa/lib/debug.h>

#include <pipewire/pipewire.h>
#include <pipewire/sig.h>
#include <pipewire/module.h>
#include <pipewire/node-factory.h>

struct type {
	uint32_t format;
	uint32_t props;
	struct spa_type_meta meta;
	struct spa_type_data data;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_video format_video;
	struct spa_type_video_format video_format;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
	type->props = spa_type_map_get_id(map, SPA_TYPE__Props);
	spa_type_meta_map(map, &type->meta);
	spa_type_data_map(map, &type->data);
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_video_map(map, &type->format_video);
	spa_type_video_format_map(map, &type->video_format);
}

#define WIDTH   640
#define HEIGHT  480
#define BPP    3

struct data {
	struct type type;

	const char *path;

	SDL_Renderer *renderer;
	SDL_Window *window;
	SDL_Texture *texture;

	bool running;
	struct pw_loop *loop;

	struct pw_core *core;

	struct pw_remote *remote;
	struct pw_listener on_state_changed;

	struct pw_node *node;
	struct pw_port *port;
	struct spa_port_info port_info;

	uint8_t buffer[1024];

	struct spa_video_info_raw format;
	int32_t stride;

	uint8_t params_buffer[1024];
	struct spa_param *params[2];

	struct spa_buffer *buffers[32];
	int n_buffers;
};

static void handle_events(struct data *data)
{
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_QUIT:
			exit(0);
			break;
		}
	}
}

static struct {
	Uint32 format;
	uint32_t id;
} video_formats[] = {
	{
	SDL_PIXELFORMAT_UNKNOWN, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_INDEX1LSB, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_UNKNOWN, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_INDEX1LSB, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_INDEX1MSB, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_INDEX4LSB, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_INDEX4MSB, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_INDEX8, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_RGB332, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_RGB444, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_RGB555, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_BGR555, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_ARGB4444, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_RGBA4444, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_ABGR4444, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_BGRA4444, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_ARGB1555, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_RGBA5551, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_ABGR1555, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_BGRA5551, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_RGB565, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_BGR565, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_RGB24, offsetof(struct spa_type_video_format, RGB),}, {
	SDL_PIXELFORMAT_RGB888, offsetof(struct spa_type_video_format, RGB),}, {
	SDL_PIXELFORMAT_RGBX8888, offsetof(struct spa_type_video_format, RGBx),}, {
	SDL_PIXELFORMAT_BGR24, offsetof(struct spa_type_video_format, BGR),}, {
	SDL_PIXELFORMAT_BGR888, offsetof(struct spa_type_video_format, BGR),}, {
	SDL_PIXELFORMAT_BGRX8888, offsetof(struct spa_type_video_format, BGRx),}, {
	SDL_PIXELFORMAT_ARGB2101010, offsetof(struct spa_type_video_format, UNKNOWN),}, {
	SDL_PIXELFORMAT_RGBA8888, offsetof(struct spa_type_video_format, RGBA),}, {
	SDL_PIXELFORMAT_ARGB8888, offsetof(struct spa_type_video_format, ARGB),}, {
	SDL_PIXELFORMAT_BGRA8888, offsetof(struct spa_type_video_format, BGRA),}, {
	SDL_PIXELFORMAT_ABGR8888, offsetof(struct spa_type_video_format, ABGR),}, {
	SDL_PIXELFORMAT_YV12, offsetof(struct spa_type_video_format, YV12),}, {
	SDL_PIXELFORMAT_IYUV, offsetof(struct spa_type_video_format, I420),}, {
	SDL_PIXELFORMAT_YUY2, offsetof(struct spa_type_video_format, YUY2),}, {
	SDL_PIXELFORMAT_UYVY, offsetof(struct spa_type_video_format, UYVY),}, {
	SDL_PIXELFORMAT_YVYU, offsetof(struct spa_type_video_format, YVYU),}, {
	SDL_PIXELFORMAT_NV12, offsetof(struct spa_type_video_format, NV12),}, {
SDL_PIXELFORMAT_NV21, offsetof(struct spa_type_video_format, NV21),}};

static uint32_t sdl_format_to_id(struct data *data, Uint32 format)
{
	int i;

	for (i = 0; i < SPA_N_ELEMENTS(video_formats); i++) {
		if (video_formats[i].format == format)
			return *SPA_MEMBER(&data->type.video_format, video_formats[i].id, uint32_t);
	}
	return data->type.video_format.UNKNOWN;
}

static Uint32 id_to_sdl_format(struct data *data, uint32_t id)
{
	int i;

	for (i = 0; i < SPA_N_ELEMENTS(video_formats); i++) {
		if (*SPA_MEMBER(&data->type.video_format, video_formats[i].id, uint32_t) == id)
			return video_formats[i].format;
	}
	return SDL_PIXELFORMAT_UNKNOWN;
}

#define PROP(f,key,type,...)							\
	SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
#define PROP_U_MM(f,key,type,...)						\
	SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |				\
			SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)

static int impl_port_enum_formats(struct pw_port *port,
				  struct spa_format **format,
				  const struct spa_format *filter,
				  int32_t index)
{
	struct data *data = port->user_data;
	const struct spa_format *formats[1];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(data->buffer, sizeof(data->buffer));
	struct spa_pod_frame f[2];
	SDL_RendererInfo info;
	int i, c;

	if (index != 0)
		return SPA_RESULT_ENUM_END;

	SDL_GetRendererInfo(data->renderer, &info);

	spa_pod_builder_push_format(&b, &f[0], data->type.format,
				    data->type.media_type.video,
				    data->type.media_subtype.raw);

	spa_pod_builder_push_prop(&b, &f[1], data->type.format_video.format,
				  SPA_POD_PROP_FLAG_UNSET |
				  SPA_POD_PROP_RANGE_ENUM);
	for (i = 0, c = 0; i < info.num_texture_formats; i++) {
		uint32_t id = sdl_format_to_id(data, info.texture_formats[i]);
		if (id == 0)
			continue;
		if (c++ == 0)
			spa_pod_builder_id(&b, id);
		spa_pod_builder_id(&b, id);
	}
	for (i = 0; i < SPA_N_ELEMENTS(video_formats); i++) {
		uint32_t id =
		    *SPA_MEMBER(&data->type.video_format, video_formats[i].id,
				uint32_t);
		if (id != data->type.video_format.UNKNOWN)
			spa_pod_builder_id(&b, id);
	}
	spa_pod_builder_pop(&b, &f[1]);
	spa_pod_builder_add(&b,
		PROP_U_MM(&f[1], data->type.format_video.size, SPA_POD_TYPE_RECTANGLE,
			WIDTH, HEIGHT,
			1, 1, info.max_texture_width, info.max_texture_height),
		PROP_U_MM(&f[1], data->type.format_video.framerate, SPA_POD_TYPE_FRACTION,
			25, 1,
			0, 1, 30, 1),
		0);
	spa_pod_builder_pop(&b, &f[0]);
	formats[0] = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_format);

	spa_debug_format(formats[0]);

	*format = (struct spa_format *)formats[0];

	return SPA_RESULT_OK;
}

static int impl_port_set_format(struct pw_port *port, uint32_t flags, const struct spa_format *format)
{
	struct data *data = port->user_data;
	struct pw_core *core = data->core;
	struct spa_pod_builder b = { NULL };
	struct spa_pod_frame f[2];
	Uint32 sdl_format;
	void *d;

	if (format == NULL)
		return SPA_RESULT_OK;

	spa_debug_format(format);

	spa_format_video_raw_parse(format, &data->format, &data->type.format_video);

	sdl_format = id_to_sdl_format(data, data->format.format);
	if (sdl_format == SDL_PIXELFORMAT_UNKNOWN)
		return SPA_RESULT_ERROR;

	data->texture = SDL_CreateTexture(data->renderer,
					  sdl_format,
					  SDL_TEXTUREACCESS_STREAMING,
					  data->format.size.width,
					  data->format.size.height);
	SDL_LockTexture(data->texture, NULL, &d, &data->stride);
	SDL_UnlockTexture(data->texture);

	spa_pod_builder_init(&b, data->params_buffer, sizeof(data->params_buffer));
	spa_pod_builder_object(&b, &f[0], 0, core->type.param_alloc_buffers.Buffers,
		PROP(&f[1], core->type.param_alloc_buffers.size, SPA_POD_TYPE_INT,
			data->stride * data->format.size.height),
		PROP(&f[1], core->type.param_alloc_buffers.stride, SPA_POD_TYPE_INT,
			data->stride),
		PROP_U_MM(&f[1], core->type.param_alloc_buffers.buffers, SPA_POD_TYPE_INT,
			32,
			2, 32),
		PROP(&f[1], core->type.param_alloc_buffers.align, SPA_POD_TYPE_INT,
			16));
	data->params[0] = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_param);

	spa_pod_builder_object(&b, &f[0], 0, core->type.param_alloc_meta_enable.MetaEnable,
		PROP(&f[1], core->type.param_alloc_meta_enable.type, SPA_POD_TYPE_ID,
			core->type.meta.Header),
		PROP(&f[1], core->type.param_alloc_meta_enable.size, SPA_POD_TYPE_INT,
			sizeof(struct spa_meta_header)));
	data->params[1] = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_param);

	return SPA_RESULT_OK;
}

static int impl_port_get_format(struct pw_port *port, const struct spa_format **format)
{
	struct data *data = port->user_data;
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(data->buffer, sizeof(data->buffer));
	struct spa_pod_frame f[2];

	spa_pod_builder_push_format(&b, &f[0], data->type.format,
				    data->type.media_type.video,
				    data->type.media_subtype.raw);
	spa_pod_builder_add(&b,
		PROP(&f[1], data->type.format_video.format, SPA_POD_TYPE_ID, data->format.format),
		PROP(&f[1], data->type.format_video.size, SPA_POD_TYPE_RECTANGLE, &data->format.size),
		PROP(&f[1], data->type.format_video.framerate, SPA_POD_TYPE_FRACTION, &data->format.framerate),
		0);
	spa_pod_builder_pop(&b, &f[0]);
	*format = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_format);

	return SPA_RESULT_OK;
}

static int impl_port_get_info(struct pw_port *port, const struct spa_port_info **info)
{
	struct data *data = port->user_data;

	data->port_info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
	data->port_info.rate = 0;
	data->port_info.props = NULL;

	*info = &data->port_info;

	return SPA_RESULT_OK;
}

static int impl_port_enum_params(struct pw_port *port, uint32_t index, struct spa_param **param)
{
	struct data *data = port->user_data;

	if (index >= 2)
		return SPA_RESULT_ENUM_END;

	*param = data->params[index];

	return SPA_RESULT_OK;
}

static int impl_port_set_param(struct pw_port *port, struct spa_param *param)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int impl_port_use_buffers(struct pw_port *port, struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct data *data = port->user_data;
	int i;
	for (i = 0; i < n_buffers; i++)
		data->buffers[i] = buffers[i];
	data->n_buffers = n_buffers;
	return SPA_RESULT_OK;
}

static int impl_port_alloc_buffers(struct pw_port *port,
                              struct spa_param **params, uint32_t n_params,
                              struct spa_buffer **buffers, uint32_t *n_buffers)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int impl_port_reuse_buffer(struct pw_port *port, uint32_t buffer_id)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int impl_port_send_command(struct pw_port *port, struct spa_command *command)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static const struct pw_port_implementation impl_port = {
	PW_VERSION_PORT_IMPLEMENTATION,
	impl_port_enum_formats,
	impl_port_set_format,
	impl_port_get_format,
	impl_port_get_info,
	impl_port_enum_params,
	impl_port_set_param,
	impl_port_use_buffers,
	impl_port_alloc_buffers,
	impl_port_reuse_buffer,
	impl_port_send_command,
};

static int impl_node_get_props(struct pw_node *node, struct spa_props **props)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int impl_node_set_props(struct pw_node *node, const struct spa_props *props)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int impl_node_send_command(struct pw_node *node,
                                  const struct spa_command *command)
{
	return SPA_RESULT_OK;
}

static struct pw_port* impl_node_add_port(struct pw_node *node,
                                     enum pw_direction direction,
                                     uint32_t port_id)
{
	return NULL;
}

static int impl_node_process_input(struct pw_node *node)
{
	struct data *data = node->user_data;
	struct pw_port *port = data->port;
	struct spa_buffer *buf;
	uint8_t *map;
	void *sdata, *ddata;
	int sstride, dstride, ostride;
	int i;
	uint8_t *src, *dst;

	buf = port->buffers[port->io.buffer_id];

	if (buf->datas[0].type == data->type.data.MemFd ||
	    buf->datas[0].type == data->type.data.DmaBuf) {
		map = mmap(NULL, buf->datas[0].maxsize + buf->datas[0].mapoffset, PROT_READ,
			   MAP_PRIVATE, buf->datas[0].fd, 0);
		sdata = SPA_MEMBER(map, buf->datas[0].mapoffset, uint8_t);
	} else if (buf->datas[0].type == data->type.data.MemPtr) {
		map = NULL;
		sdata = buf->datas[0].data;
	} else
		return SPA_RESULT_ERROR;

	if (SDL_LockTexture(data->texture, NULL, &ddata, &dstride) < 0) {
		fprintf(stderr, "Couldn't lock texture: %s\n", SDL_GetError());
		return SPA_RESULT_ERROR;
	}
	sstride = buf->datas[0].chunk->stride;
	ostride = SPA_MIN(sstride, dstride);

	src = sdata;
	dst = ddata;
	for (i = 0; i < data->format.size.height; i++) {
		memcpy(dst, src, ostride);
		src += sstride;
		dst += dstride;
	}
	SDL_UnlockTexture(data->texture);

	SDL_RenderClear(data->renderer);
	SDL_RenderCopy(data->renderer, data->texture, NULL, NULL);
	SDL_RenderPresent(data->renderer);

	if (map)
		munmap(map, buf->datas[0].maxsize);

	handle_events(data);

	port->io.status = SPA_RESULT_NEED_BUFFER;

	return SPA_RESULT_NEED_BUFFER;
}

static int impl_node_process_output(struct pw_node *node)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static const struct pw_node_implementation impl_node = {
	PW_VERSION_NODE_IMPLEMENTATION,
	impl_node_get_props,
	impl_node_set_props,
	impl_node_send_command,
	impl_node_add_port,
	impl_node_process_input,
	impl_node_process_output,
};

static void make_node(struct data *data)
{
	struct pw_properties *props;

	props = pw_properties_new(
			//"pipewire.target.node", port_path,
			"pipewire.autoconnect", "1",
			NULL);

	data->node = pw_node_new(data->core, NULL, NULL, "SDL-sink", props, 0);
	data->node->user_data = data;
	data->node->implementation = &impl_node;

	data->port = pw_port_new(PW_DIRECTION_INPUT, 0, 0);
	data->port->user_data = data;
	data->port->implementation = &impl_port;
	pw_port_add(data->port, data->node);
	pw_node_register(data->node);

	pw_remote_export(data->remote, data->node);
}

static void on_state_changed(struct pw_listener *listener, struct pw_remote *remote)
{
	struct data *data = SPA_CONTAINER_OF(listener, struct data, on_state_changed);

	switch (remote->state) {
	case PW_REMOTE_STATE_ERROR:
		printf("remote error: %s\n", remote->error);
		data->running = false;
		break;

	case PW_REMOTE_STATE_CONNECTED:
		make_node(data);
		break;

	default:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(remote->state));
		break;
	}
}


int main(int argc, char *argv[])
{
	struct data data = { 0, };

	pw_init(&argc, &argv);

	data.loop = pw_loop_new();
	data.running = true;
	data.core = pw_core_new(data.loop, NULL);
        data.remote = pw_remote_new(data.core, NULL);
	data.path = argc > 1 ? argv[1] : NULL;

	pw_module_load(data.core, "libpipewire-module-spa-node-factory", NULL);

	init_type(&data.type, data.core->type.map);

	spa_debug_set_type_map(data.core->type.map);

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		printf("can't initialize SDL: %s\n", SDL_GetError());
		return -1;
	}

	if (SDL_CreateWindowAndRenderer
	    (WIDTH, HEIGHT, SDL_WINDOW_RESIZABLE, &data.window, &data.renderer)) {
		printf("can't create window: %s\n", SDL_GetError());
		return -1;
	}

	pw_signal_add(&data.remote->state_changed, &data.on_state_changed, on_state_changed);

        pw_remote_connect(data.remote);

	pw_loop_enter(data.loop);
	while (data.running) {
		pw_loop_iterate(data.loop, -1);
	}
	pw_loop_leave(data.loop);

	pw_loop_destroy(data.loop);

	return 0;
}
