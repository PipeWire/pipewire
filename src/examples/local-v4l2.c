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
	struct spa_source *timer;

	struct pw_core *core;
	struct pw_type *t;
	struct pw_node *node;
	struct pw_port *port;
	struct spa_port_info port_info;

	struct pw_node *v4l2;

	struct pw_link *link;

	struct spa_port_io *io;

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

static int impl_port_set_io(void *data, struct spa_port_io *io)
{
	struct data *d = data;
	d->io = io;
	return SPA_RESULT_OK;
}

static int impl_port_enum_formats(void *data,
				  struct spa_format **format,
				  const struct spa_format *filter,
				  int32_t index)
{
	struct data *d = data;
	const struct spa_format *formats[1];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(d->buffer, sizeof(d->buffer));
	struct spa_pod_frame f[2];
	SDL_RendererInfo info;
	int i, c;

	if (index != 0)
		return SPA_RESULT_ENUM_END;

	SDL_GetRendererInfo(d->renderer, &info);

	spa_pod_builder_push_format(&b, &f[0], d->type.format,
				    d->type.media_type.video,
				    d->type.media_subtype.raw);

	spa_pod_builder_push_prop(&b, &f[1], d->type.format_video.format,
				  SPA_POD_PROP_FLAG_UNSET |
				  SPA_POD_PROP_RANGE_ENUM);
	for (i = 0, c = 0; i < info.num_texture_formats; i++) {
		uint32_t id = sdl_format_to_id(d, info.texture_formats[i]);
		if (id == 0)
			continue;
		if (c++ == 0)
			spa_pod_builder_id(&b, id);
		spa_pod_builder_id(&b, id);
	}
	for (i = 0; i < SPA_N_ELEMENTS(video_formats); i++) {
		uint32_t id =
		    *SPA_MEMBER(&d->type.video_format, video_formats[i].id,
				uint32_t);
		if (id != d->type.video_format.UNKNOWN)
			spa_pod_builder_id(&b, id);
	}
	spa_pod_builder_pop(&b, &f[1]);
	spa_pod_builder_add(&b,
		PROP_U_MM(&f[1], d->type.format_video.size, SPA_POD_TYPE_RECTANGLE,
			WIDTH, HEIGHT,
			1, 1, info.max_texture_width, info.max_texture_height),
		PROP_U_MM(&f[1], d->type.format_video.framerate, SPA_POD_TYPE_FRACTION,
			25, 1,
			0, 1, 30, 1),
		0);
	spa_pod_builder_pop(&b, &f[0]);
	formats[0] = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_format);

	spa_debug_format(formats[0]);

	*format = (struct spa_format *)formats[0];

	return SPA_RESULT_OK;
}

static int impl_port_set_format(void *data, uint32_t flags, const struct spa_format *format)
{
	struct data *d = data;
	struct pw_type *t = d->t;
	struct spa_pod_builder b = { NULL };
	struct spa_pod_frame f[2];
	Uint32 sdl_format;
	void *dest;

	if (format == NULL) {
		return SPA_RESULT_OK;
	}

	spa_debug_format(format);

	spa_format_video_raw_parse(format, &d->format, &d->type.format_video);

	sdl_format = id_to_sdl_format(d, d->format.format);
	if (sdl_format == SDL_PIXELFORMAT_UNKNOWN)
		return SPA_RESULT_ERROR;

	d->texture = SDL_CreateTexture(d->renderer,
					  sdl_format,
					  SDL_TEXTUREACCESS_STREAMING,
					  d->format.size.width,
					  d->format.size.height);
	SDL_LockTexture(d->texture, NULL, &dest, &d->stride);
	SDL_UnlockTexture(d->texture);

	spa_pod_builder_init(&b, d->params_buffer, sizeof(d->params_buffer));
	spa_pod_builder_object(&b, &f[0], 0, t->param_alloc_buffers.Buffers,
		PROP(&f[1], t->param_alloc_buffers.size, SPA_POD_TYPE_INT,
			d->stride * d->format.size.height),
		PROP(&f[1], t->param_alloc_buffers.stride, SPA_POD_TYPE_INT,
			d->stride),
		PROP_U_MM(&f[1], t->param_alloc_buffers.buffers, SPA_POD_TYPE_INT,
			32,
			2, 32),
		PROP(&f[1], t->param_alloc_buffers.align, SPA_POD_TYPE_INT,
			16));
	d->params[0] = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_param);

	spa_pod_builder_object(&b, &f[0], 0, t->param_alloc_meta_enable.MetaEnable,
		PROP(&f[1], t->param_alloc_meta_enable.type, SPA_POD_TYPE_ID,
			t->meta.Header),
		PROP(&f[1], t->param_alloc_meta_enable.size, SPA_POD_TYPE_INT,
			sizeof(struct spa_meta_header)));
	d->params[1] = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_param);

	return SPA_RESULT_OK;
}

static int impl_port_get_info(void *data, const struct spa_port_info **info)
{
	struct data *d = data;

	d->port_info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
	d->port_info.rate = 0;
	d->port_info.props = NULL;

	*info = &d->port_info;

	return SPA_RESULT_OK;
}

static int impl_port_enum_params(void *data, uint32_t index, struct spa_param **param)
{
	struct data *d = data;

	if (index >= 2)
		return SPA_RESULT_ENUM_END;

	*param = d->params[index];

	return SPA_RESULT_OK;
}

static int impl_port_use_buffers(void *data, struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct data *d = data;
	int i;
	for (i = 0; i < n_buffers; i++)
		d->buffers[i] = buffers[i];
	d->n_buffers = n_buffers;
	return SPA_RESULT_OK;
}

static const struct pw_port_implementation impl_port = {
	PW_VERSION_PORT_IMPLEMENTATION,
	.set_io = impl_port_set_io,
	.enum_formats = impl_port_enum_formats,
	.set_format = impl_port_set_format,
	.get_info = impl_port_get_info,
	.enum_params = impl_port_enum_params,
	.use_buffers = impl_port_use_buffers,
};

static int impl_node_process_input(void *data)
{
	struct data *d = data;
	struct spa_buffer *buf;
	uint8_t *map;
	void *sdata, *ddata;
	int sstride, dstride, ostride;
	int i;
	uint8_t *src, *dst;

	buf = d->buffers[d->io->buffer_id];

	if (buf->datas[0].type == d->type.data.MemFd ||
	    buf->datas[0].type == d->type.data.DmaBuf) {
		map = mmap(NULL, buf->datas[0].maxsize + buf->datas[0].mapoffset, PROT_READ,
			   MAP_PRIVATE, buf->datas[0].fd, 0);
		sdata = SPA_MEMBER(map, buf->datas[0].mapoffset, uint8_t);
	} else if (buf->datas[0].type == d->type.data.MemPtr) {
		map = NULL;
		sdata = buf->datas[0].data;
	} else
		return SPA_RESULT_ERROR;

	if (SDL_LockTexture(d->texture, NULL, &ddata, &dstride) < 0) {
		fprintf(stderr, "Couldn't lock texture: %s\n", SDL_GetError());
		return SPA_RESULT_ERROR;
	}
	sstride = buf->datas[0].chunk->stride;
	ostride = SPA_MIN(sstride, dstride);

	src = sdata;
	dst = ddata;
	for (i = 0; i < d->format.size.height; i++) {
		memcpy(dst, src, ostride);
		src += sstride;
		dst += dstride;
	}
	SDL_UnlockTexture(d->texture);

	SDL_RenderClear(d->renderer);
	SDL_RenderCopy(d->renderer, d->texture, NULL, NULL);
	SDL_RenderPresent(d->renderer);

	if (map)
		munmap(map, buf->datas[0].maxsize);

	handle_events(d);

	d->io->status = SPA_RESULT_NEED_BUFFER;

	return SPA_RESULT_NEED_BUFFER;
}

static const struct pw_node_implementation impl_node = {
	PW_VERSION_NODE_IMPLEMENTATION,
	.process_input = impl_node_process_input,
};

static void make_nodes(struct data *data)
{
	struct pw_node_factory *factory;
	struct pw_properties *props;

	data->node = pw_node_new(data->core, NULL, NULL, "SDL-sink", NULL, 0);
	pw_node_set_implementation(data->node, &impl_node, data);

	data->port = pw_port_new(PW_DIRECTION_INPUT, 0, 0);
	pw_port_set_implementation(data->port, &impl_port, data);
	pw_port_add(data->port, data->node);
	pw_node_register(data->node);

	factory = pw_core_find_node_factory(data->core, "spa-node-factory");
	props = pw_properties_new("spa.library.name", "v4l2/libspa-v4l2",
				  "spa.factory.name", "v4l2-source", NULL);
	data->v4l2 = pw_node_factory_create_node(factory, NULL, "v4l2-source", props);

	data->link = pw_link_new(data->core,
				 NULL,
				 pw_node_get_free_port(data->v4l2, PW_DIRECTION_OUTPUT),
				 data->port,
				 NULL,
				 NULL,
				 NULL);
	pw_link_activate(data->link);
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };

	pw_init(&argc, &argv);

	data.loop = pw_loop_new();
	data.running = true;
	data.core = pw_core_new(data.loop, NULL);
	data.t = pw_core_get_type(data.core);
	data.path = argc > 1 ? argv[1] : NULL;

	pw_module_load(data.core, "libpipewire-module-spa-node-factory", NULL);

	init_type(&data.type, data.t->map);

	spa_debug_set_type_map(data.t->map);

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		printf("can't initialize SDL: %s\n", SDL_GetError());
		return -1;
	}

	if (SDL_CreateWindowAndRenderer
	    (WIDTH, HEIGHT, SDL_WINDOW_RESIZABLE, &data.window, &data.renderer)) {
		printf("can't create window: %s\n", SDL_GetError());
		return -1;
	}

	make_nodes(&data);

	pw_loop_enter(data.loop);
	while (data.running) {
		pw_loop_iterate(data.loop, -1);
	}
	pw_loop_leave(data.loop);

	pw_loop_destroy(data.loop);

	return 0;
}
