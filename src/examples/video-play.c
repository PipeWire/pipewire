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

#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/format.h>

#include <pipewire/pipewire.h>

#define WIDTH   640
#define HEIGHT  480
#define BPP    3

#include "sdl.h"

struct data {
	const char *path;

	SDL_Renderer *renderer;
	SDL_Window *window;
	SDL_Texture *texture;

	struct pw_main_loop *loop;

	struct pw_core *core;
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

static void
on_process(void *_data)
{
	struct data *data = _data;
	struct pw_stream *stream = data->stream;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	void *sdata, *ddata;
	int sstride, dstride, ostride;
	uint32_t i;
	uint8_t *src, *dst;

	b = pw_stream_dequeue_buffer(stream);
	if (b == NULL)
		return;

	buf = b->buffer;

	pw_log_trace("new buffer %d", buf->id);

	handle_events(data);

	if ((sdata = buf->datas[0].data) == NULL)
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


	pw_stream_queue_buffer(stream, b);
}

static void on_stream_state_changed(void *_data, enum pw_stream_state old,
				    enum pw_stream_state state, const char *error)
{
	struct data *data = _data;
	fprintf(stderr, "stream state: \"%s\"\n", pw_stream_state_as_string(state));
	switch (state) {
	case PW_STREAM_STATE_UNCONNECTED:
		pw_main_loop_quit(data->loop);
		break;
	case PW_STREAM_STATE_CONFIGURE:
		pw_stream_set_active(data->stream, true);
		break;
	default:
		break;
	}
}

static void
on_stream_format_changed(void *_data, const struct spa_pod *format)
{
	struct data *data = _data;
	struct pw_stream *stream = data->stream;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[2];
	Uint32 sdl_format;
	void *d;

	if (format == NULL) {
		pw_stream_finish_format(stream, 0, NULL, 0);
		return;
	}

	fprintf(stderr, "got format:\n");
	spa_debug_format(2, NULL, format);

	spa_format_video_raw_parse(format, &data->format);

	sdl_format = id_to_sdl_format(data->format.format);
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
		SPA_ID_PARAM_Buffers, SPA_ID_OBJECT_ParamBuffers,
		":", SPA_PARAM_BUFFERS_buffers, "iru", 8,
			SPA_POD_PROP_MIN_MAX(2, 16),
		":", SPA_PARAM_BUFFERS_blocks,  "i", 1,
		":", SPA_PARAM_BUFFERS_size,    "i", data->stride * data->format.size.height,
		":", SPA_PARAM_BUFFERS_stride,  "i", data->stride,
		":", SPA_PARAM_BUFFERS_align,   "i", 16);

	params[1] = spa_pod_builder_object(&b,
		SPA_ID_PARAM_Meta, SPA_ID_OBJECT_ParamMeta,
		":", SPA_PARAM_META_type, "I", SPA_META_Header,
		":", SPA_PARAM_META_size, "i", sizeof(struct spa_meta_header));

	pw_stream_finish_format(stream, 0, params, 2);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.format_changed = on_stream_format_changed,
	.process = on_process,
};

static int build_format(struct data *data, struct spa_pod_builder *b, const struct spa_pod **params)
{
	SDL_RendererInfo info;

	SDL_GetRendererInfo(data->renderer, &info);
	params[0] = sdl_build_formats(&info, b);

	fprintf(stderr, "supported formats:\n");
	spa_debug_format(2, NULL, params[0]);

	return 0;
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);

	data.stream = pw_stream_new_simple(
			pw_main_loop_get_loop(data.loop),
			"video-play",
			pw_properties_new(
				PW_NODE_PROP_MEDIA, "Video",
				PW_NODE_PROP_CATEGORY, "Capture",
				PW_NODE_PROP_ROLE, "Camera",
				NULL),
			&stream_events,
			&data);

	data.remote = pw_stream_get_remote(data.stream);
	data.core = pw_remote_get_core(data.remote);
	data.path = argc > 1 ? argv[1] : NULL;

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		fprintf(stderr, "can't initialize SDL: %s\n", SDL_GetError());
		return -1;
	}

	if (SDL_CreateWindowAndRenderer
	    (WIDTH, HEIGHT, SDL_WINDOW_RESIZABLE, &data.window, &data.renderer)) {
		fprintf(stderr, "can't create window: %s\n", SDL_GetError());
		return -1;
	}

	build_format(&data, &b, params);

	pw_stream_connect(data.stream,
			  PW_DIRECTION_INPUT,
			  data.path,
			  PW_STREAM_FLAG_AUTOCONNECT |
			  PW_STREAM_FLAG_INACTIVE |
			  PW_STREAM_FLAG_EXCLUSIVE |
			  PW_STREAM_FLAG_MAP_BUFFERS,
			  params, 1);

	pw_main_loop_run(data.loop);

	pw_stream_destroy(data.stream);
	pw_main_loop_destroy(data.loop);

	return 0;
}
