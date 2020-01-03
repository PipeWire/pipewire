/* PipeWire
 *
 * Copyright © 2019 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>

#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/format.h>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

#define WIDTH   640
#define HEIGHT  480
#define BPP    3

#define MAX_BUFFERS	64

#include "sdl.h"

struct pixel {
	float r, g, b, a;
};

struct data {
	const char *path;

	SDL_Renderer *renderer;
	SDL_Window *window;
	SDL_Texture *texture;
	SDL_Texture *cursor;

	struct pw_main_loop *loop;

	struct pw_filter *filter;
	struct spa_hook filter_listener;

	void *in_port;

	struct spa_video_info_raw format;
	int32_t stride;

	int counter;
	SDL_Rect rect;
	SDL_Rect cursor_rect;
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

/* our data processing function is in general:
 *
 *  struct pw_buffer *b;
 *  b = pw_filter_dequeue_buffer(port);
 *
 *  .. do stuff with buffer ...
 *
 *  pw_filter_queue_buffer(port, b);
 */
static void
on_process(void *_data, struct spa_io_position *position)
{
	struct data *data = _data;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	void *sdata, *ddata;
	int sstride, dstride;
	uint32_t i, j;
	uint8_t *src, *dst;

	b = pw_filter_dequeue_buffer(data->in_port);
	if (b == NULL)
		return;

	buf = b->buffer;

	pw_log_trace("new buffer %p %dx%d", buf, data->format.size.width, data->format.size.height);

	handle_events(data);

	if ((sdata = buf->datas[0].data) == NULL) {
		pw_log_error("no buffer data");
		goto done;
	}

	if (SDL_LockTexture(data->texture, NULL, &ddata, &dstride) < 0) {
		pw_log_error("Couldn't lock texture: %s", SDL_GetError());
		goto done;
	}

	/* copy video image in texture */
	sstride = buf->datas[0].chunk->stride;

	src = sdata;
	dst = ddata;

	for (i = 0; i < data->format.size.height; i++) {
		struct pixel *p = (struct pixel *) src;
		for (j = 0; j < data->format.size.width; j++) {
			dst[j * 4 + 0] = SPA_CLAMP(lrintf(p[j].r * 255.0f), 0, 255);
			dst[j * 4 + 1] = SPA_CLAMP(lrintf(p[j].g * 255.0f), 0, 255);
			dst[j * 4 + 2] = SPA_CLAMP(lrintf(p[j].b * 255.0f), 0, 255);
			dst[j * 4 + 3] = SPA_CLAMP(lrintf(p[j].a * 255.0f), 0, 255);
		}
		src += sstride;
		dst += dstride;
	}
	SDL_UnlockTexture(data->texture);

	SDL_RenderClear(data->renderer);
	SDL_RenderCopy(data->renderer, data->texture, &data->rect, NULL);
	SDL_RenderPresent(data->renderer);

      done:
	pw_filter_queue_buffer(data->in_port, b);
}

static void on_filter_state_changed(void *_data, enum pw_filter_state old,
				    enum pw_filter_state state, const char *error)
{
	struct data *data = _data;
	fprintf(stderr, "filter state: \"%s\"\n", pw_filter_state_as_string(state));
	switch (state) {
	case PW_FILTER_STATE_UNCONNECTED:
		pw_main_loop_quit(data->loop);
		break;
	case PW_FILTER_STATE_PAUSED:
		/* because we started inactive, activate ourselves now */
		pw_filter_set_active(data->filter, true);
		break;
	default:
		break;
	}
}

static void
on_filter_param_changed(void *_data, void *port_data, uint32_t id, const struct spa_pod *param)
{
	struct data *data = _data;
	struct pw_filter *filter = data->filter;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[5];
	Uint32 sdl_format;
	void *d;

	/* NULL means to clear the format */
	if (param == NULL || id != SPA_PARAM_Format)
		return;

	fprintf(stderr, "got format:\n");
	spa_debug_format(2, NULL, param);

	/* call a helper function to parse the format for us. */
	spa_format_video_raw_parse(param, &data->format);

	if (data->format.format == SPA_VIDEO_FORMAT_RGBA_F32)
		sdl_format = SDL_PIXELFORMAT_RGBA32;
	else
		sdl_format = SDL_PIXELFORMAT_UNKNOWN;

	if (sdl_format == SDL_PIXELFORMAT_UNKNOWN) {
		pw_filter_set_error(filter, -EINVAL, "unknown format");
		return;
	}

	data->texture = SDL_CreateTexture(data->renderer,
					  sdl_format,
					  SDL_TEXTUREACCESS_STREAMING,
					  data->format.size.width,
					  data->format.size.height);
	SDL_LockTexture(data->texture, NULL, &d, &data->stride);
	SDL_UnlockTexture(data->texture);

	data->rect.x = 0;
	data->rect.y = 0;
	data->rect.w = data->format.size.width;
	data->rect.h = data->format.size.height;

	/* a SPA_TYPE_OBJECT_ParamBuffers object defines the acceptable size,
	 * number, stride etc of the buffers */
	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, MAX_BUFFERS),
		SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size,    SPA_POD_Int(data->stride * data->format.size.height),
		SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(data->stride),
		SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));

	/* we are done */
	pw_filter_update_params(filter, port_data, params, 1);
}

/* these are the filter events we listen for */
static const struct pw_filter_events filter_events = {
	PW_VERSION_FILTER_EVENTS,
	.state_changed = on_filter_state_changed,
	.param_changed = on_filter_param_changed,
	.process = on_process,
};

int main(int argc, char *argv[])
{
	struct data data = { 0, };

	pw_init(&argc, &argv);

	/* create a main loop */
	data.loop = pw_main_loop_new(NULL);

	/* create a simple filter, the simple filter manages to core and remote
	 * objects for you if you don't need to deal with them
	 *
	 * If you plan to autoconnect your filter, you need to provide at least
	 * media, category and role properties
	 *
	 * Pass your events and a user_data pointer as the last arguments. This
	 * will inform you about the filter state. The most important event
	 * you need to listen to is the process event where you need to consume
	 * the data provided to you.
	 */
	data.filter = pw_filter_new_simple(
			pw_main_loop_get_loop(data.loop),
			"video-dsp-play",
			pw_properties_new(
				PW_KEY_MEDIA_TYPE, "Video",
				PW_KEY_MEDIA_CATEGORY, "Capture",
				PW_KEY_MEDIA_ROLE, "DSP",
				NULL),
			&filter_events,
			&data);

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

	/* Make a new DSP port. This will automatically set up the right
	 * parameters for the port */
	data.in_port = pw_filter_add_port(data.filter,
			PW_DIRECTION_INPUT,
			PW_FILTER_PORT_FLAG_MAP_BUFFERS,
			0,
			pw_properties_new(
				PW_KEY_FORMAT_DSP, "32 bit float RGBA video",
				PW_KEY_PORT_NAME, "input",
				NULL),
			NULL, 0);

	pw_filter_connect(data.filter,
			0,
			//PW_FILTER_FLAG_RT_PROCESS,
			NULL, 0);

	/* do things until we quit the mainloop */
	pw_main_loop_run(data.loop);

	pw_filter_destroy(data.filter);
	pw_main_loop_destroy(data.loop);

	SDL_DestroyTexture(data.texture);
	if (data.cursor)
		SDL_DestroyTexture(data.cursor);
	SDL_DestroyRenderer(data.renderer);
	SDL_DestroyWindow(data.window);

	return 0;
}
