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
#include <unistd.h>
#include <sys/mman.h>

#include <SDL2/SDL.h>

#include <spa/support/type-map.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/lib/debug.h>

#include <pipewire/pipewire.h>

struct type {
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_video format_video;
	struct spa_type_video_format video_format;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
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

	struct pw_main_loop *loop;

	struct pw_core *core;
	struct pw_type *t;
	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct spa_video_info_raw format;
	int32_t stride;

	int counter;
};

static void handle_events(struct data *data)
{
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_QUIT:
			pw_main_loop_quit(data->loop);
			break;
		}
	}
}

static int
do_render(struct spa_loop *loop, bool async, uint32_t seq,
	  const void *_data, size_t size, void *user_data)
{
	struct data *data = user_data;
	struct spa_buffer *buf = ((struct spa_buffer **) _data)[0];
	void *sdata, *ddata;
	int sstride, dstride, ostride;
	uint32_t i;
	uint8_t *src, *dst;

	handle_events(data);

	if ((sdata = buf->datas[0].data) == NULL)
		return -EINVAL;

	if (SDL_LockTexture(data->texture, NULL, &ddata, &dstride) < 0) {
		fprintf(stderr, "Couldn't lock texture: %s\n", SDL_GetError());
		return -EIO;
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

	return 0;
}

static void
on_stream_new_buffer(void *_data, uint32_t id)
{
	struct data *data = _data;
	struct pw_stream *stream = data->stream;
	struct spa_buffer *buf;

	buf = pw_stream_peek_buffer(stream, id);

	pw_loop_invoke(pw_main_loop_get_loop(data->loop), do_render,
		       SPA_ID_INVALID, &buf, sizeof(struct spa_buffer *),
		       true, data);

	pw_stream_recycle_buffer(stream, id);
}

static void on_stream_state_changed(void *_data, enum pw_stream_state old,
				    enum pw_stream_state state, const char *error)
{
	struct data *data = _data;
	printf("stream state: \"%s\"\n", pw_stream_state_as_string(state));
	switch (state) {
	case PW_STREAM_STATE_CONFIGURE:
		pw_stream_set_active(data->stream, true);
		break;
	default:
		break;
	}
}

static struct {
	Uint32 format;
	uint32_t id;
} video_formats[] = {
	{ SDL_PIXELFORMAT_UNKNOWN, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_INDEX1LSB, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_UNKNOWN, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_INDEX1LSB, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_INDEX1MSB, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_INDEX4LSB, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_INDEX4MSB, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_INDEX8, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_RGB332, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_RGB444, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_RGB555, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_BGR555, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_ARGB4444, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_RGBA4444, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_ABGR4444, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_BGRA4444, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_ARGB1555, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_RGBA5551, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_ABGR1555, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_BGRA5551, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_RGB565, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_BGR565, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_RGB24, offsetof(struct spa_type_video_format, RGB),},
	{ SDL_PIXELFORMAT_RGB888, offsetof(struct spa_type_video_format, RGB),},
	{ SDL_PIXELFORMAT_RGBX8888, offsetof(struct spa_type_video_format, RGBx),},
	{ SDL_PIXELFORMAT_BGR24, offsetof(struct spa_type_video_format, BGR),},
	{ SDL_PIXELFORMAT_BGR888, offsetof(struct spa_type_video_format, BGR),},
	{ SDL_PIXELFORMAT_BGRX8888, offsetof(struct spa_type_video_format, BGRx),},
	{ SDL_PIXELFORMAT_ARGB2101010, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_RGBA8888, offsetof(struct spa_type_video_format, RGBA),},
	{ SDL_PIXELFORMAT_ARGB8888, offsetof(struct spa_type_video_format, ARGB),},
	{ SDL_PIXELFORMAT_BGRA8888, offsetof(struct spa_type_video_format, BGRA),},
	{ SDL_PIXELFORMAT_ABGR8888, offsetof(struct spa_type_video_format, ABGR),},
	{ SDL_PIXELFORMAT_YV12, offsetof(struct spa_type_video_format, YV12),},
	{ SDL_PIXELFORMAT_IYUV, offsetof(struct spa_type_video_format, I420),},
	{ SDL_PIXELFORMAT_YUY2, offsetof(struct spa_type_video_format, YUY2),},
	{ SDL_PIXELFORMAT_UYVY, offsetof(struct spa_type_video_format, UYVY),},
	{ SDL_PIXELFORMAT_YVYU, offsetof(struct spa_type_video_format, YVYU),},
#if SDL_VERSION_ATLEAST(2,0,4)
	{ SDL_PIXELFORMAT_NV12, offsetof(struct spa_type_video_format, NV12),},
	{ SDL_PIXELFORMAT_NV21, offsetof(struct spa_type_video_format, NV21),},
#endif
};

static uint32_t sdl_format_to_id(struct data *data, Uint32 format)
{
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(video_formats); i++) {
		if (video_formats[i].format == format)
			return *SPA_MEMBER(&data->type.video_format, video_formats[i].id, uint32_t);
	}
	return data->type.video_format.UNKNOWN;
}

static Uint32 id_to_sdl_format(struct data *data, uint32_t id)
{
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(video_formats); i++) {
		if (*SPA_MEMBER(&data->type.video_format, video_formats[i].id, uint32_t) == id)
			return video_formats[i].format;
	}
	return SDL_PIXELFORMAT_UNKNOWN;
}

static void
on_stream_format_changed(void *_data, struct spa_pod *format)
{
	struct data *data = _data;
	struct pw_stream *stream = data->stream;
	struct pw_type *t = data->t;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	struct spa_pod *params[2];
	Uint32 sdl_format;
	void *d;

	if (format == NULL) {
		pw_stream_finish_format(stream, 0, NULL, 0);
		return;
	}

	spa_format_video_raw_parse(format, &data->format, &data->type.format_video);

	sdl_format = id_to_sdl_format(data, data->format.format);
	if (sdl_format == SDL_PIXELFORMAT_UNKNOWN) {
		pw_stream_finish_format(stream, -EINVAL, NULL, 0);
		return;
	}

	data->texture = SDL_CreateTexture(data->renderer,
					  sdl_format,
					  SDL_TEXTUREACCESS_STREAMING,
					  data->format.size.width,
					  data->format.size.height);
	SDL_LockTexture(data->texture, NULL, &d, &data->stride);
	SDL_UnlockTexture(data->texture);

	params[0] = spa_pod_builder_object(&b,
		t->param.idBuffers, t->param_buffers.Buffers,
		":", t->param_buffers.size,    "i", data->stride * data->format.size.height,
		":", t->param_buffers.stride,  "i", data->stride,
		":", t->param_buffers.buffers, "iru", 8,
			SPA_POD_PROP_MIN_MAX(2, 32),
		":", t->param_buffers.align,   "i", 16);

	params[1] = spa_pod_builder_object(&b,
		t->param.idMeta, t->param_meta.Meta,
		":", t->param_meta.type, "I", t->meta.Header,
		":", t->param_meta.size, "i", sizeof(struct spa_meta_header));

	pw_stream_finish_format(stream, 0, params, 2);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.format_changed = on_stream_format_changed,
	.new_buffer = on_stream_new_buffer,
};

static void on_state_changed(void *_data, enum pw_remote_state old, enum pw_remote_state state, const char *error)
{
	struct data *data = _data;
	struct pw_remote *remote = data->remote;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		printf("remote error: %s\n", error);
		pw_main_loop_quit(data->loop);
		break;

	case PW_REMOTE_STATE_CONNECTED:
	{
		const struct spa_pod *params[1];
		uint8_t buffer[1024];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		SDL_RendererInfo info;
		uint32_t i, c;

		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));

		data->stream = pw_stream_new(remote, "video-play",
				pw_properties_new("pipewire.client.reuse", "1", NULL));

		SDL_GetRendererInfo(data->renderer, &info);

		spa_pod_builder_push_object(&b,
					    data->t->param.idEnumFormat, data->t->spa_format);
		spa_pod_builder_id(&b, data->type.media_type.video);
		spa_pod_builder_id(&b, data->type.media_subtype.raw);

		spa_pod_builder_push_prop(&b, data->type.format_video.format,
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
		spa_pod_builder_pop(&b);
		spa_pod_builder_add(&b,
			":", data->type.format_video.size,      "Rru", &SPA_RECTANGLE(WIDTH, HEIGHT),
				SPA_POD_PROP_MIN_MAX(&SPA_RECTANGLE(1,1),
						     &SPA_RECTANGLE(info.max_texture_width,
								    info.max_texture_height)),
			":", data->type.format_video.framerate, "Fru", &SPA_FRACTION(25,1),
				SPA_POD_PROP_MIN_MAX(&SPA_RECTANGLE(0,1),
						     &SPA_RECTANGLE(30,1)),
			NULL);
		params[0] = spa_pod_builder_pop(&b);

		printf("supported formats:\n");
		spa_debug_pod(params[0], SPA_DEBUG_FLAG_FORMAT);

		pw_stream_add_listener(data->stream,
				       &data->stream_listener,
				       &stream_events,
				       data);

		pw_stream_connect(data->stream,
				  PW_DIRECTION_INPUT,
				  data->path,
				  PW_STREAM_FLAG_AUTOCONNECT |
				  PW_STREAM_FLAG_INACTIVE |
				  PW_STREAM_FLAG_MAP_BUFFERS,
				  params, 1);
		break;
	}
	default:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));
		break;
	}
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_state_changed,
};


static void connect_state_changed(void *_data, enum pw_remote_state old,
				  enum pw_remote_state state, const char *error)
{
	struct data *data = _data;

	printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
	case PW_REMOTE_STATE_CONNECTED:
		pw_main_loop_quit(data->loop);
		break;
	default:
		break;
	}
}

static int get_fd(struct data *data)
{
	int fd;
	struct pw_remote *remote = pw_remote_new(data->core, NULL, 0);
	struct spa_hook remote_listener;
	const struct pw_remote_events revents = {
		PW_VERSION_REMOTE_EVENTS,
		.state_changed = connect_state_changed,
	};

	pw_remote_add_listener(remote, &remote_listener, &revents, data);

	if (pw_remote_connect(remote) < 0)
		return -1;

	pw_main_loop_run(data->loop);

	fd = pw_remote_steal_fd(remote);

	pw_remote_destroy(remote);

	return fd;
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);
	data.core = pw_core_new(pw_main_loop_get_loop(data.loop), NULL);
	data.t = pw_core_get_type(data.core);
	data.remote = pw_remote_new(data.core, NULL, 0);
	data.path = argc > 1 ? argv[1] : NULL;

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

	pw_remote_add_listener(data.remote, &data.remote_listener, &remote_events, &data);

	pw_remote_connect_fd(data.remote, get_fd(&data));

	pw_main_loop_run(data.loop);

	pw_core_destroy(data.core);
	pw_main_loop_destroy(data.loop);

	return 0;
}
