/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

/*
 [title]
 Video input stream using \ref pw_stream "pw_stream", with format fixation.
 [title]
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <libdrm/drm_fourcc.h>

#include <spa/utils/result.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/format.h>

#include <pipewire/pipewire.h>

#define WIDTH   640
#define HEIGHT  480

#define MAX_BUFFERS	64
#define MAX_MOD		8

#include "sdl.h"

struct pixel {
	float r, g, b, a;
};

struct pw_version {
  int major;
  int minor;
  int micro;
};

struct modifier_info {
        uint32_t spa_format;
        uint32_t n_modifiers;
        uint64_t modifiers[MAX_MOD];
};

struct data {
	const char *path;

	SDL_Renderer *renderer;
	SDL_Window *window;
	SDL_Texture *texture;
	SDL_Texture *cursor;

	struct pw_main_loop *loop;
	struct spa_source *reneg;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct spa_video_info format;
	int32_t stride;
	struct spa_rectangle size;

	uint32_t n_mod_info;
	struct modifier_info mod_info[2];

	int counter;
};

static struct pw_version parse_pw_version(const char* version) {
	struct pw_version pw_version;
	sscanf(version, "%d.%d.%d", &pw_version.major, &pw_version.minor,
		&pw_version.micro);
	return pw_version;
}

static bool has_pw_version(int major, int minor, int micro) {
	struct pw_version pw_version = parse_pw_version(pw_get_library_version());
	printf("PW Version: %d.%d.%d\n", pw_version.major, pw_version.minor,
		pw_version.micro);
	return major <= pw_version.major && minor <= pw_version.minor && micro <= pw_version.micro;
}

static void init_modifiers(struct data *data)
{
	data->n_mod_info = 1;
	data->mod_info[0].spa_format = SPA_VIDEO_FORMAT_RGB;
	data->mod_info[0].n_modifiers = 2;
	data->mod_info[0].modifiers[0] = DRM_FORMAT_MOD_LINEAR;
	data->mod_info[0].modifiers[1] = DRM_FORMAT_MOD_INVALID;
}

static void destroy_modifiers(struct data *data)
{
	data->mod_info[0].n_modifiers = 0;
}

static void strip_modifier(struct data *data, uint32_t spa_format, uint64_t modifier)
{
	if (data->mod_info[0].spa_format != spa_format)
		return;
	struct modifier_info *mod_info = &data->mod_info[0];
	uint32_t counter = 0;
	// Dropping of single modifiers is only supported on PipeWire 0.3.40 and newer.
	// On older PipeWire just dropping all modifiers might work on Versions newer then 0.3.33/35
	if (has_pw_version(0,3,40)) {
		printf("Dropping a single modifier\n");
		for (uint32_t i = 0; i < mod_info->n_modifiers; i++) {
			if (mod_info->modifiers[i] == modifier)
				continue;
			mod_info->modifiers[counter++] = mod_info->modifiers[i];
		}
	} else {
		printf("Dropping all modifiers\n");
		counter = 0;
	}
	mod_info->n_modifiers = counter;
}

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

static struct spa_pod *build_format(struct spa_pod_builder *b, SDL_RendererInfo *info, enum spa_video_format format,
        uint64_t *modifiers, int modifier_count)
{
	struct spa_pod_frame f[2];
	int i, c;

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
	spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
	/* format */
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
	/* modifiers */
	if (modifier_count == 1 && modifiers[0] == DRM_FORMAT_MOD_INVALID) {
		// we only support implicit modifiers, use shortpath to skip fixation phase
		spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
		spa_pod_builder_long(b, modifiers[0]);
	} else if (modifier_count > 0) {
		// build an enumeration of modifiers
		spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
		spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
		// modifiers from the array
		for (i = 0, c = 0; i < modifier_count; i++) {
			spa_pod_builder_long(b, modifiers[i]);
			if (c++ == 0)
				spa_pod_builder_long(b, modifiers[i]);
		}
		spa_pod_builder_pop(b, &f[1]);
	}
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
		SPA_POD_CHOICE_RANGE_Rectangle(
			&SPA_RECTANGLE(WIDTH, HEIGHT),
			&SPA_RECTANGLE(1,1),
			&SPA_RECTANGLE(info->max_texture_width,
				       info->max_texture_height)),
		0);
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate,
		SPA_POD_CHOICE_RANGE_Fraction(
			&SPA_FRACTION(25,1),
			&SPA_FRACTION(0,1),
			&SPA_FRACTION(30,1)),
		0);
	return spa_pod_builder_pop(b, &f[0]);
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
	uint32_t i;
	uint8_t *src, *dst;

	b = NULL;
	/* dequeue and queue old buffers, use the last available
	 * buffer */
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

	pw_log_info("new buffer %p", buf);

	handle_events(data);

	if (buf->datas[0].type == SPA_DATA_DmaBuf) {
		// Simulate a failed import of a DmaBuf
		// We should try another modifier
		printf("Failed to import dmabuf, stripping modifier %"PRIu64"\n", data->format.info.raw.modifier);
		strip_modifier(data, data->format.info.raw.format, data->format.info.raw.modifier);
		pw_loop_signal_event(pw_main_loop_get_loop(data->loop), data->reneg);
		goto done;
	}

	if ((sdata = buf->datas[0].data) == NULL)
		goto done;

	if (SDL_LockTexture(data->texture, NULL, &ddata, &dstride) < 0) {
		fprintf(stderr, "Couldn't lock texture: %s\n", SDL_GetError());
		goto done;
	}

	/* copy video image in texture */
	sstride = buf->datas[0].chunk->stride;
	if (sstride == 0)
		sstride = buf->datas[0].chunk->size / data->size.height;
	ostride = SPA_MIN(sstride, dstride);

	src = sdata;
	dst = ddata;

	for (i = 0; i < data->size.height; i++) {
		memcpy(dst, src, ostride);
		src += sstride;
		dst += dstride;
	}
	SDL_UnlockTexture(data->texture);

	SDL_RenderClear(data->renderer);
	/* now render the video */
	SDL_RenderCopy(data->renderer, data->texture, NULL, NULL);
	SDL_RenderPresent(data->renderer);

      done:
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
	case PW_STREAM_STATE_PAUSED:
		break;
	case PW_STREAM_STATE_STREAMING:
	default:
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
	const struct spa_pod *params[1];
	Uint32 sdl_format;
	void *d;

	/* NULL means to clear the format */
	if (param == NULL || id != SPA_PARAM_Format)
		return;

	fprintf(stderr, "got format:\n");
	spa_debug_format(2, NULL, param);

	if (spa_format_parse(param, &data->format.media_type, &data->format.media_subtype) < 0)
		return;

	if (data->format.media_type != SPA_MEDIA_TYPE_video ||
	    data->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;

	/* call a helper function to parse the format for us. */
	spa_format_video_raw_parse(param, &data->format.info.raw);
	sdl_format = id_to_sdl_format(data->format.info.raw.format);
	data->size = data->format.info.raw.size;

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
	SDL_LockTexture(data->texture, NULL, &d, &data->stride);
	SDL_UnlockTexture(data->texture);

	/* a SPA_TYPE_OBJECT_ParamBuffers object defines the acceptable size,
	 * number, stride etc of the buffers */
	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, MAX_BUFFERS),
		SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size,    SPA_POD_Int(data->stride * data->size.height),
		SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(data->stride),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1<<SPA_DATA_MemPtr) | (1<<SPA_DATA_DmaBuf)));

	/* we are done */
	pw_stream_update_params(stream, params, 1);
}

/* these are the stream events we listen for */
static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.param_changed = on_stream_param_changed,
	.process = on_process,
};

static int build_formats(struct data *data, struct spa_pod_builder *b, const struct spa_pod **params)
{
	SDL_RendererInfo info;
	int n_params = 0;

	SDL_GetRendererInfo(data->renderer, &info);

	if (data->mod_info[0].n_modifiers > 0) {
		params[n_params++] = build_format(b, &info, SPA_VIDEO_FORMAT_RGB, data->mod_info[0].modifiers, data->mod_info[0].n_modifiers);
	}
	params[n_params++] = build_format(b, &info, SPA_VIDEO_FORMAT_RGB, NULL, 0);

	for (int i=0; i < n_params; i++) {
		spa_debug_format(2, NULL, params[i]);
	}

	return n_params;
}

static void reneg_format(void *_data, uint64_t expiration)
{
	struct data *data = (struct data*) _data;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[2];
	uint32_t n_params;

	if (data->format.info.raw.format == 0)
		return;

	fprintf(stderr, "renegotiate formats:\n");
	n_params = build_formats(data, &b, params);

	pw_stream_update_params(data->stream, params, n_params);
}

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	pw_main_loop_quit(data->loop);
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	const struct spa_pod *params[2];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct pw_properties *props;
	int res, n_params;

	pw_init(&argc, &argv);

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
		/* Set stream target if given on command line */
		pw_properties_set(props, PW_KEY_TARGET_OBJECT, data.path);

	data.stream = pw_stream_new_simple(
			pw_main_loop_get_loop(data.loop),
			"video-play-fixate",
			props,
			&stream_events,
			&data);

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		fprintf(stderr, "can't initialize SDL: %s\n", SDL_GetError());
		return -1;
	}

	init_modifiers(&data);

	if (SDL_CreateWindowAndRenderer
	    (WIDTH, HEIGHT, SDL_WINDOW_RESIZABLE, &data.window, &data.renderer)) {
		fprintf(stderr, "can't create window: %s\n", SDL_GetError());
		return -1;
	}

	/* build the extra parameters to connect with. To connect, we can provide
	 * a list of supported formats.  We use a builder that writes the param
	 * object to the stack. */
	printf("supported formats:\n");
	n_params = build_formats(&data, &b, params);

	/* now connect the stream, we need a direction (input/output),
	 * an optional target node to connect to, some flags and parameters
	 */
	if ((res = pw_stream_connect(data.stream,
			  PW_DIRECTION_INPUT,
			  PW_ID_ANY,
			  PW_STREAM_FLAG_AUTOCONNECT |	/* try to automatically connect this stream */
			  PW_STREAM_FLAG_MAP_BUFFERS,	/* mmap the buffer data for us */
			  params, n_params))		/* extra parameters, see above */ < 0) {
		fprintf(stderr, "can't connect: %s\n", spa_strerror(res));
		return -1;
	}

	data.reneg = pw_loop_add_event(pw_main_loop_get_loop(data.loop), reneg_format, &data);

	/* do things until we quit the mainloop */
	pw_main_loop_run(data.loop);

	pw_stream_destroy(data.stream);
	pw_main_loop_destroy(data.loop);

	destroy_modifiers(&data);

	SDL_DestroyTexture(data.texture);
	if (data.cursor)
		SDL_DestroyTexture(data.cursor);
	SDL_DestroyRenderer(data.renderer);
	SDL_DestroyWindow(data.window);
	pw_deinit();

	return 0;
}
