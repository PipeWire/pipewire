/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2025 Wim Taymans */
/* SPDX-License-Identifier: MIT */

/*
 [title]
 Video input stream using \ref pw_stream "pw_stream" and sync timeline.
 [title]
 */

#include <stdio.h>
#include <unistd.h>
#include <signal.h>

#include <spa/utils/result.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/tag-utils.h>
#include <spa/param/props.h>
#include <spa/param/latency-utils.h>
#include <spa/debug/format.h>
#include <spa/debug/pod.h>

#include <pipewire/pipewire.h>

#define WIDTH   1920
#define HEIGHT  1080
#define RATE 	30

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

	struct pw_main_loop *loop;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct spa_io_position *position;

	struct spa_video_info format;
	int32_t stride;
	struct spa_rectangle size;

	int counter;
	SDL_Rect rect;
	bool is_yuv;

	bool with_synctimeline;
	bool with_synctimeline_release;
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
 *  b = pw_stream_dequeue_buffer(stream);
 *
 *  .. do stuff with buffer ...
 *
 *  pw_stream_queue_buffer(stream, b);
 */
static void
on_process(void *_data)
{
	struct data *data = _data;
	struct pw_stream *stream = data->stream;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	void *sdata, *ddata;
	int sstride, dstride, ostride;
	struct spa_meta_header *h;
	struct spa_meta_sync_timeline *stl = NULL;
	uint32_t i, j;
	uint8_t *src, *dst;
	uint64_t cmd;

	b = NULL;
	while (true) {
		struct pw_buffer *t;
		if ((t = pw_stream_dequeue_buffer(stream)) == NULL)
			break;
		if (b)
			pw_stream_queue_buffer(stream, b);
		b = t;
	}
	if (b == NULL) {
		pw_log_warn("out of buffers: %m");
		return;
	}

	buf = b->buffer;

	pw_log_trace("new buffer %p", buf);

	handle_events(data);

	if ((sdata = buf->datas[0].data) == NULL)
		goto done;

	if ((h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h)))) {
		uint64_t now = pw_stream_get_nsec(stream);
		pw_log_debug("now:%"PRIu64" pts:%"PRIu64" diff:%"PRIi64,
				now, h->pts, now - h->pts);
	}
	if ((stl = spa_buffer_find_meta_data(buf, SPA_META_SyncTimeline, sizeof(*stl))) &&
	    stl->acquire_point) {
		/* wait before we can use the buffer */
		if (read(buf->datas[1].fd, &cmd, sizeof(cmd)) < 0)
			pw_log_warn("acquire_point wait error %m");
		pw_log_debug("acquire_point:%"PRIu64, stl->acquire_point);
	}

	/* copy video image in texture */
	if (data->is_yuv) {
		void *datas[4];
		sstride = data->stride;
		if (buf->n_datas == 1) {
			SDL_UpdateTexture(data->texture, NULL,
					sdata, sstride);
		} else {
			datas[0] = sdata;
			datas[1] = buf->datas[1].data;
			datas[2] = buf->datas[2].data;
			SDL_UpdateYUVTexture(data->texture, NULL,
					datas[0], sstride,
					datas[1], sstride / 2,
					datas[2], sstride / 2);
		}
	}
	else {
		if (SDL_LockTexture(data->texture, NULL, &ddata, &dstride) < 0) {
			fprintf(stderr, "Couldn't lock texture: %s\n", SDL_GetError());
		}

		sstride = buf->datas[0].chunk->stride;
		if (sstride == 0)
			sstride = buf->datas[0].chunk->size / data->size.height;
		ostride = SPA_MIN(sstride, dstride);

		src = sdata;
		dst = ddata;

		if (data->format.media_subtype == SPA_MEDIA_SUBTYPE_dsp) {
			for (i = 0; i < data->size.height; i++) {
				struct pixel *p = (struct pixel *) src;
				for (j = 0; j < data->size.width; j++) {
					dst[j * 4 + 0] = SPA_CLAMP((uint8_t)(p[j].r * 255.0f), 0u, 255u);
					dst[j * 4 + 1] = SPA_CLAMP((uint8_t)(p[j].g * 255.0f), 0u, 255u);
					dst[j * 4 + 2] = SPA_CLAMP((uint8_t)(p[j].b * 255.0f), 0u, 255u);
					dst[j * 4 + 3] = SPA_CLAMP((uint8_t)(p[j].a * 255.0f), 0u, 255u);
				}
				src += sstride;
				dst += dstride;
			}
		} else {
			for (i = 0; i < data->size.height; i++) {
				memcpy(dst, src, ostride);
				src += sstride;
				dst += dstride;
			}
		}
		SDL_UnlockTexture(data->texture);
	}

	SDL_RenderClear(data->renderer);
	/* now render the video */
	SDL_RenderCopy(data->renderer, data->texture, &data->rect, NULL);
	SDL_RenderPresent(data->renderer);

      done:
	pw_stream_queue_buffer(stream, b);

	if (stl != NULL && stl->release_point) {
		/* we promise to signal the release point */
		if (data->with_synctimeline_release)
			SPA_FLAG_CLEAR(stl->flags, SPA_META_SYNC_TIMELINE_UNSCHEDULED_RELEASE);
		cmd = 1;
		/* signal buffer release point */
		write(buf->datas[2].fd, &cmd, sizeof(cmd));
		pw_log_debug("release:%"PRIu64, stl->release_point);
	}
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
	case PW_STREAM_STATE_PAUSED:
		/* because we started inactive, activate ourselves now */
		pw_stream_set_active(data->stream, true);
		break;
	default:
		break;
	}
}

static void
on_stream_io_changed(void *_data, uint32_t id, void *area, uint32_t size)
{
	struct data *data = _data;

	switch (id) {
	case SPA_IO_Position:
		data->position = area;
		break;
	}
}

/* Be notified when the stream param changes. We're only looking at the
 * format changes.
 *
 * We are now supposed to call pw_stream_finish_format() with success or
 * failure, depending on if we can support the format. Because we gave
 * a list of supported formats, this should be ok.
 *
 * As part of pw_stream_finish_format() we can provide parameters that
 * will control the buffer memory allocation. This includes the metadata
 * that we would like on our buffer, the size, alignment, etc.
 */
static void
on_stream_param_changed(void *_data, uint32_t id, const struct spa_pod *param)
{
	struct data *data = _data;
	struct pw_stream *stream = data->stream;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	struct spa_pod_frame f;
	const struct spa_pod *params[5];
	uint32_t n_params = 0;
	Uint32 sdl_format;
	void *d;
	int32_t mult, size, blocks;

	if (param != NULL && id == SPA_PARAM_Tag) {
		spa_debug_pod(0, NULL, param);
		return;
	}
	if (param != NULL && id == SPA_PARAM_Latency) {
		struct spa_latency_info info;
		if (spa_latency_parse(param, &info) >= 0)
			fprintf(stderr, "got latency: %"PRIu64"\n", (info.min_ns + info.max_ns) / 2);
		return;
	}
	/* NULL means to clear the format */
	if (param == NULL || id != SPA_PARAM_Format)
		return;

	fprintf(stderr, "got format:\n");
	spa_debug_format(2, NULL, param);

	if (spa_format_parse(param, &data->format.media_type, &data->format.media_subtype) < 0)
		return;

	if (data->format.media_type != SPA_MEDIA_TYPE_video)
		return;

	switch (data->format.media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		/* call a helper function to parse the format for us. */
		spa_format_video_raw_parse(param, &data->format.info.raw);
		sdl_format = id_to_sdl_format(data->format.info.raw.format);
		data->size = SPA_RECTANGLE(data->format.info.raw.size.width,
				data->format.info.raw.size.height);
		mult = 1;
		break;
	case SPA_MEDIA_SUBTYPE_dsp:
		spa_format_video_dsp_parse(param, &data->format.info.dsp);
		if (data->format.info.dsp.format != SPA_VIDEO_FORMAT_DSP_F32)
			return;
		sdl_format = SDL_PIXELFORMAT_RGBA32;
		data->size = SPA_RECTANGLE(data->position->video.size.width,
				data->position->video.size.height);
		mult = 4;
		break;
	default:
		sdl_format = SDL_PIXELFORMAT_UNKNOWN;
		break;
	}

	if (sdl_format == SDL_PIXELFORMAT_UNKNOWN) {
		pw_stream_set_error(stream, -EINVAL, "unknown pixel format");
		return;
	}
	if (data->size.width == 0 || data->size.height == 0) {
		pw_stream_set_error(stream, -EINVAL, "invalid size");
		return;
	}

	data->texture = SDL_CreateTexture(data->renderer,
					  sdl_format,
					  SDL_TEXTUREACCESS_STREAMING,
					  data->size.width,
					  data->size.height);
	switch(sdl_format) {
	case SDL_PIXELFORMAT_YV12:
	case SDL_PIXELFORMAT_IYUV:
		data->stride = data->size.width;
		size = (data->stride * data->size.height) * 3 / 2;
		data->is_yuv = true;
		blocks = 3;
		break;
	case SDL_PIXELFORMAT_YUY2:
		data->is_yuv = true;
		data->stride = data->size.width * 2;
		size = (data->stride * data->size.height);
		blocks = 1;
		break;
	default:
		if (SDL_LockTexture(data->texture, NULL, &d, &data->stride) < 0) {
			fprintf(stderr, "Couldn't lock texture: %s\n", SDL_GetError());
			data->stride = data->size.width * 2;
		} else
			SDL_UnlockTexture(data->texture);
		size = data->stride * data->size.height;
		blocks = 1;
		break;
	}

	data->rect.x = 0;
	data->rect.y = 0;
	data->rect.w = data->size.width;
	data->rect.h = data->size.height;

	if (data->with_synctimeline) {
		/* first add Buffer with 3 blocks (1 data, 2 sync fds). */
		spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
		spa_pod_builder_add(&b,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(3),
			SPA_PARAM_BUFFERS_size,    SPA_POD_Int(size * mult),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(data->stride * mult),
			SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1<<SPA_DATA_MemFd)),
			0);
		/* this depends on the negotiation of the SyncTimeline metadata */
		spa_pod_builder_prop(&b, SPA_PARAM_BUFFERS_metaType, SPA_POD_PROP_FLAG_MANDATORY);
		spa_pod_builder_int(&b, 1<<SPA_META_SyncTimeline);
		params[n_params++] = spa_pod_builder_pop(&b, &f);

		/* explicit sync information */
		spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
		spa_pod_builder_add(&b,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_SyncTimeline),
			SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_sync_timeline)),
			0);
		if (data->with_synctimeline_release) {
			/* drop features flags if not provided by both sides */
			spa_pod_builder_prop(&b, SPA_PARAM_META_features, SPA_POD_PROP_FLAG_DROP);
			spa_pod_builder_int(&b, SPA_META_FEATURE_SYNC_TIMELINE_RELEASE);
		}
		params[n_params++] = spa_pod_builder_pop(&b, &f);
	}

	/* fallback for when the synctimeline is not negotiated */
	params[n_params++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, MAX_BUFFERS),
		SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(blocks),
		SPA_PARAM_BUFFERS_size,    SPA_POD_Int(size * mult),
		SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(data->stride * mult),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1<<SPA_DATA_MemFd)));

	/* a header metadata with timing information */
	params[n_params++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

	/* we are done */
	pw_stream_update_params(stream, params, n_params);
}

static void on_stream_add_buffer(void *_data, struct pw_buffer *buffer)
{
}

/* these are the stream events we listen for */
static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.io_changed = on_stream_io_changed,
	.param_changed = on_stream_param_changed,
	.process = on_process,
	.add_buffer = on_stream_add_buffer,
};

static int build_format(struct data *data, struct spa_pod_builder *b, const struct spa_pod **params)
{
	uint32_t n_params = 0;
	SDL_RendererInfo info;

	SDL_GetRendererInfo(data->renderer, &info);
	params[n_params++] = sdl_build_formats(&info, b);

	fprintf(stderr, "supported SDL formats:\n");
	spa_debug_format(2, NULL, params[0]);

	params[n_params++] = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,		SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,	SPA_POD_Id(SPA_MEDIA_SUBTYPE_dsp),
			SPA_FORMAT_VIDEO_format,	SPA_POD_Id(SPA_VIDEO_FORMAT_DSP_F32));

	fprintf(stderr, "supported DSP formats:\n");
	spa_debug_format(2, NULL, params[1]);

	return n_params;
}

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	pw_main_loop_quit(data->loop);
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	const struct spa_pod *params[3];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct pw_properties *props;
	int res, n_params;

	pw_init(&argc, &argv);

	data.with_synctimeline = true;
	data.with_synctimeline_release = true;

	/* create a main loop */
	data.loop = pw_main_loop_new(NULL);

	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);

	/* create a simple stream, the simple stream manages to core and remote
	 * objects for you if you don't need to deal with them
	 *
	 * If you plan to autoconnect your stream, you need to provide at least
	 * media, category and role properties
	 *
	 * Pass your events and a user_data pointer as the last arguments. This
	 * will inform you about the stream state. The most important event
	 * you need to listen to is the process event where you need to consume
	 * the data provided to you.
	 */
	props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Camera",
			NULL),
	data.path = argc > 1 ? argv[1] : NULL;
	if (data.path)
		pw_properties_set(props, PW_KEY_TARGET_OBJECT, data.path);

	data.stream = pw_stream_new_simple(
			pw_main_loop_get_loop(data.loop),
			"video-play",
			props,
			&stream_events,
			&data);

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		fprintf(stderr, "can't initialize SDL: %s\n", SDL_GetError());
		return -1;
	}

	if (SDL_CreateWindowAndRenderer
	    (WIDTH, HEIGHT, SDL_WINDOW_RESIZABLE, &data.window, &data.renderer)) {
		fprintf(stderr, "can't create window: %s\n", SDL_GetError());
		return -1;
	}

	/* build the extra parameters to connect with. To connect, we can provide
	 * a list of supported formats.  We use a builder that writes the param
	 * object to the stack. */
	n_params = build_format(&data, &b, params);

	/* now connect the stream, we need a direction (input/output),
	 * an optional target node to connect to, some flags and parameters
	 */
	if ((res = pw_stream_connect(data.stream,
			  PW_DIRECTION_INPUT,
			  PW_ID_ANY,
			  PW_STREAM_FLAG_AUTOCONNECT |	/* try to automatically connect this stream */
			  PW_STREAM_FLAG_INACTIVE |	/* we will activate ourselves */
			  PW_STREAM_FLAG_MAP_BUFFERS,	/* mmap the buffer data for us */
			  params, n_params))		/* extra parameters, see above */ < 0) {
		fprintf(stderr, "can't connect: %s\n", spa_strerror(res));
		return -1;
	}

	/* do things until we quit the mainloop */
	pw_main_loop_run(data.loop);

	pw_stream_destroy(data.stream);
	pw_main_loop_destroy(data.loop);

	SDL_DestroyTexture(data.texture);
	SDL_DestroyRenderer(data.renderer);
	SDL_DestroyWindow(data.window);
	pw_deinit();

	return 0;
}
