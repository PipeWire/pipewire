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

#include <pipewire/client/pipewire.h>
#include <pipewire/client/sig.h>
#include <spa/lib/debug.h>

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

	struct pw_context *context;
	struct pw_listener on_state_changed;

	struct pw_stream *stream;
	struct pw_listener on_stream_state_changed;
	struct pw_listener on_stream_format_changed;
	struct pw_listener on_stream_new_buffer;

	struct spa_video_info_raw format;
	int32_t stride;

	uint8_t params_buffer[1024];
	int counter;
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

static void
on_stream_new_buffer(struct pw_listener *listener, struct pw_stream *stream, uint32_t id)
{
	struct data *data = SPA_CONTAINER_OF(listener, struct data, on_stream_new_buffer);
	struct spa_buffer *buf;
	uint8_t *map;
	void *sdata, *ddata;
	int sstride, dstride, ostride;
	int i;
	uint8_t *src, *dst;

	buf = pw_stream_peek_buffer(data->stream, id);

	if (buf->datas[0].type == data->type.data.MemFd) {
		map = mmap(NULL, buf->datas[0].maxsize + buf->datas[0].mapoffset, PROT_READ,
			   MAP_PRIVATE, buf->datas[0].fd, 0);
		sdata = SPA_MEMBER(map, buf->datas[0].mapoffset, uint8_t);
	} else if (buf->datas[0].type == data->type.data.MemPtr) {
		map = NULL;
		sdata = buf->datas[0].data;
	} else
		return;

	if (SDL_LockTexture(data->texture, NULL, &ddata, &dstride) < 0) {
		fprintf(stderr, "Couldn't lock texture: %s\n", SDL_GetError());
		return;
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

	pw_stream_recycle_buffer(data->stream, id);

	handle_events(data);
}

static void on_stream_state_changed(struct pw_listener *listener, struct pw_stream *stream)
{
	printf("stream state: \"%s\"\n", pw_stream_state_as_string(stream->state));
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

static void
on_stream_format_changed(struct pw_listener *listener,
			 struct pw_stream *stream, struct spa_format *format)
{
	struct data *data = SPA_CONTAINER_OF(listener, struct data, on_stream_format_changed);
	struct pw_context *ctx = stream->context;
	struct spa_pod_builder b = { NULL };
	struct spa_pod_frame f[2];
	struct spa_param *params[2];
	Uint32 sdl_format;
	void *d;

	if (format == NULL) {
		pw_stream_finish_format(stream, SPA_RESULT_OK, NULL, 0);
		return;
	}

	spa_debug_format(format);

	spa_format_video_raw_parse(format, &data->format, &data->type.format_video);

	sdl_format = id_to_sdl_format(data, data->format.format);
	if (sdl_format == SDL_PIXELFORMAT_UNKNOWN) {
		pw_stream_finish_format(stream, SPA_RESULT_ERROR, NULL, 0);
		return;
	}

	data->texture = SDL_CreateTexture(data->renderer,
					  sdl_format,
					  SDL_TEXTUREACCESS_STREAMING,
					  data->format.size.width,
					  data->format.size.height);
	SDL_LockTexture(data->texture, NULL, &d, &data->stride);
	SDL_UnlockTexture(data->texture);

	spa_pod_builder_init(&b, data->params_buffer, sizeof(data->params_buffer));
	spa_pod_builder_object(&b, &f[0], 0, ctx->type.param_alloc_buffers.Buffers,
		PROP(&f[1], ctx->type.param_alloc_buffers.size, SPA_POD_TYPE_INT,
			data->stride * data->format.size.height),
		PROP(&f[1], ctx->type.param_alloc_buffers.stride, SPA_POD_TYPE_INT,
			data->stride),
		PROP_U_MM(&f[1], ctx->type.param_alloc_buffers.buffers, SPA_POD_TYPE_INT,
			32,
			2, 32),
		PROP(&f[1], ctx->type.param_alloc_buffers.align, SPA_POD_TYPE_INT,
			16));
	params[0] = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_param);

	spa_pod_builder_object(&b, &f[0], 0, ctx->type.param_alloc_meta_enable.MetaEnable,
		PROP(&f[1], ctx->type.param_alloc_meta_enable.type, SPA_POD_TYPE_ID,
			ctx->type.meta.Header),
		PROP(&f[1], ctx->type.param_alloc_meta_enable.size, SPA_POD_TYPE_INT,
			sizeof(struct spa_meta_header)));
	params[1] = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_param);

	pw_stream_finish_format(stream, SPA_RESULT_OK, params, 2);
}

static void on_state_changed(struct pw_listener *listener, struct pw_context *context)
{
	struct data *data = SPA_CONTAINER_OF(listener, struct data, on_state_changed);

	switch (context->state) {
	case PW_CONTEXT_STATE_ERROR:
		printf("context error: %s\n", context->error);
		data->running = false;
		break;

	case PW_CONTEXT_STATE_CONNECTED:
	{
		struct spa_format *formats[1];
		uint8_t buffer[1024];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		struct spa_pod_frame f[2];
		SDL_RendererInfo info;
		int i, c;

		printf("context state: \"%s\"\n",
		       pw_context_state_as_string(context->state));

		data->stream = pw_stream_new(context, "video-play", NULL);

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

		printf("supported formats:\n");
		spa_debug_format(formats[0]);

		pw_signal_add(&data->stream->state_changed,
			      &data->on_stream_state_changed, on_stream_state_changed);
		pw_signal_add(&data->stream->format_changed,
			      &data->on_stream_format_changed, on_stream_format_changed);
		pw_signal_add(&data->stream->new_buffer,
			      &data->on_stream_new_buffer, on_stream_new_buffer);

		pw_stream_connect(data->stream,
				  PW_DIRECTION_INPUT,
				  PW_STREAM_MODE_BUFFER,
				  data->path, PW_STREAM_FLAG_AUTOCONNECT, 1, formats);
		break;
	}
	default:
		printf("context state: \"%s\"\n", pw_context_state_as_string(context->state));
		break;
	}
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };

	pw_init(&argc, &argv);

	data.loop = pw_loop_new();
	data.running = true;
	data.context = pw_context_new(data.loop, "video-play", NULL);
	data.path = argc > 1 ? argv[1] : NULL;

	init_type(&data.type, data.context->type.map);

	spa_debug_set_type_map(data.context->type.map);

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		printf("can't initialize SDL: %s\n", SDL_GetError());
		return -1;
	}

	if (SDL_CreateWindowAndRenderer
	    (WIDTH, HEIGHT, SDL_WINDOW_RESIZABLE, &data.window, &data.renderer)) {
		printf("can't create window: %s\n", SDL_GetError());
		return -1;
	}

	pw_signal_add(&data.context->state_changed, &data.on_state_changed, on_state_changed);

	pw_context_connect(data.context, PW_CONTEXT_FLAG_NO_REGISTRY);

	pw_loop_enter(data.loop);
	while (data.running) {
		pw_loop_iterate(data.loop, -1);
	}
	pw_loop_leave(data.loop);

	pw_context_destroy(data.context);
	pw_loop_destroy(data.loop);

	return 0;
}
