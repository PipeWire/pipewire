/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

/*
 [title]
 Fixating negotiated modifiers.
 [title]
 */

#include "config.h"

#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <math.h>
#include <libdrm/drm_fourcc.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <spa/param/video/format-utils.h>
#include <spa/debug/format.h>

#include <pipewire/pipewire.h>

#define BPP		3
#define CURSOR_WIDTH	64
#define CURSOR_HEIGHT	64
#define CURSOR_BPP	4

#define MAX_BUFFERS	64

#define M_PI_M2 ( M_PI + M_PI )

uint64_t supported_modifiers[] = {DRM_FORMAT_MOD_INVALID, DRM_FORMAT_MOD_LINEAR};

struct data {
	struct pw_thread_loop *loop;
	struct spa_source *timer;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct spa_video_info_raw format;
	int32_t stride;

	int counter;
	uint32_t seq;

	double crop;
	double accumulator;
};

static void draw_elipse(uint32_t *dst, int width, int height, uint32_t color)
{
	int i, j, r1, r2, r12, r22, r122;

	r1 = width/2;
	r12 = r1 * r1;
	r2 = height/2;
	r22 = r2 * r2;
	r122 = r12 * r22;

	for (i = -r2; i < r2; i++) {
		for (j = -r1; j < r1; j++) {
			dst[(i + r2)*width+(j+r1)] =
				(i * i * r12 + j * j * r22 <= r122) ? color : 0x00000000;
		}
	}
}

static struct spa_pod *fixate_format(struct spa_pod_builder *b, enum spa_video_format format,
        uint64_t *modifier)
{
	struct spa_pod_frame f[1];

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
	spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
	/* format */
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
	/* modifiers */
	if (modifier) {
		// we only support implicit modifiers, use shortpath to skip fixation phase
		spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
		spa_pod_builder_long(b, *modifier);
	}
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
		SPA_POD_CHOICE_RANGE_Rectangle(
			&SPA_RECTANGLE(320, 240),
			&SPA_RECTANGLE(1,1),
			&SPA_RECTANGLE(4096,4096)),
		0);
	// variable framerate
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate,
		SPA_POD_Fraction(&SPA_FRACTION(25, 1)), 0);
	return spa_pod_builder_pop(b, &f[0]);
}

static struct spa_pod *build_format(struct spa_pod_builder *b, enum spa_video_format format,
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
			&SPA_RECTANGLE(320, 240),
			&SPA_RECTANGLE(1,1),
			&SPA_RECTANGLE(4096,4096)),
		0);
	// variable framerate
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate,
		SPA_POD_Fraction(&SPA_FRACTION(25, 1)), 0);
	return spa_pod_builder_pop(b, &f[0]);
}

/* called when we should push a new buffer in the queue */
static void on_process(void *userdata)
{
	struct data *data = userdata;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	uint32_t i, j;
	uint8_t *p;
	struct spa_meta *m;
	struct spa_meta_header *h;
	struct spa_meta_region *mc;
	struct spa_meta_cursor *mcs;

	if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
		pw_log_warn("out of buffers: %m");
		return;
	}

	buf = b->buffer;
	if ((p = buf->datas[0].data) == NULL) {
		printf("No data ptr\n");
		goto done;
	}

	if ((h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h)))) {
#if 0
		h->pts = pw_stream_get_nsec(data->stream);
#else
		h->pts = -1;
#endif
		h->flags = 0;
		h->seq = data->seq++;
		h->dts_offset = 0;
	}
	if ((m = spa_buffer_find_meta(buf, SPA_META_VideoDamage))) {
		struct spa_meta_region *r = spa_meta_first(m);

		if (spa_meta_check(r, m)) {
			r->region.position = SPA_POINT(0,0);
			r->region.size = data->format.size;
			r++;
		}
		if (spa_meta_check(r, m))
			r->region = SPA_REGION(0,0,0,0);
	}
	if ((mc = spa_buffer_find_meta_data(buf, SPA_META_VideoCrop, sizeof(*mc)))) {
		data->crop = (sin(data->accumulator) + 1.0) * 32.0;
		mc->region.position.x = data->crop;
		mc->region.position.y = data->crop;
		mc->region.size.width = data->format.size.width - data->crop*2;
		mc->region.size.height = data->format.size.height - data->crop*2;
	}
	if ((mcs = spa_buffer_find_meta_data(buf, SPA_META_Cursor, sizeof(*mcs)))) {
		struct spa_meta_bitmap *mb;
		uint32_t *bitmap, color;

		mcs->id = 1;
		mcs->position.x = (sin(data->accumulator) + 1.0) * 160.0 + 80;
		mcs->position.y = (cos(data->accumulator) + 1.0) * 100.0 + 50;
		mcs->hotspot.x = 0;
		mcs->hotspot.y = 0;
		mcs->bitmap_offset = sizeof(struct spa_meta_cursor);

		mb = SPA_PTROFF(mcs, mcs->bitmap_offset, struct spa_meta_bitmap);
		mb->format = SPA_VIDEO_FORMAT_ARGB;
		mb->size.width = CURSOR_WIDTH;
		mb->size.height = CURSOR_HEIGHT;
		mb->stride = CURSOR_WIDTH * CURSOR_BPP;
		mb->offset = sizeof(struct spa_meta_bitmap);

		bitmap = SPA_PTROFF(mb, mb->offset, uint32_t);
		color = (cos(data->accumulator) + 1.0) * (1 << 23);
		color |= 0xff000000;

		draw_elipse(bitmap, mb->size.width, mb->size.height, color);
	}

	for (i = 0; i < data->format.size.height; i++) {
		for (j = 0; j < data->format.size.width * BPP; j++) {
			p[j] = data->counter + j * i;
		}
		p += data->stride;
		data->counter += 13;
	}

	data->accumulator += M_PI_M2 / 50.0;
	if (data->accumulator >= M_PI_M2)
		data->accumulator -= M_PI_M2;

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->size = data->format.size.height * data->stride;
	buf->datas[0].chunk->stride = data->stride;

done:
	pw_stream_queue_buffer(data->stream, b);
}

/* trigger the graph when we are a driver */
static void on_timeout(void *userdata, uint64_t expirations)
{
	struct data *data = userdata;
	pw_log_trace("timeout");
	pw_stream_trigger_process(data->stream);
}

/* when the stream is STREAMING, start the timer at 40ms intervals
 * to produce and push a frame. In other states we PAUSE the timer. */
static void on_stream_state_changed(void *_data, enum pw_stream_state old, enum pw_stream_state state,
				    const char *error)
{
	struct data *data = _data;

	printf("stream state: \"%s\"\n", pw_stream_state_as_string(state));

	switch (state) {
	case PW_STREAM_STATE_PAUSED:
		printf("node id: %d\n", pw_stream_get_node_id(data->stream));
		pw_loop_update_timer(pw_thread_loop_get_loop(data->loop),
				data->timer, NULL, NULL, false);
		break;
	case PW_STREAM_STATE_STREAMING:
	{
		struct timespec timeout, interval;

		timeout.tv_sec = 0;
		timeout.tv_nsec = 1;
		interval.tv_sec = 0;
		interval.tv_nsec = 40 * SPA_NSEC_PER_MSEC;

		if (pw_stream_is_driving(data->stream))
			pw_loop_update_timer(pw_thread_loop_get_loop(data->loop),
					data->timer, &timeout, &interval, false);
		break;
	}
	default:
		break;
	}
}

/* we set the PW_STREAM_FLAG_ALLOC_BUFFERS flag when connecting so we need
 * to provide buffer memory.  */
static void on_stream_add_buffer(void *_data, struct pw_buffer *buffer)
{
	printf("add_buffer\n");
	struct data *data = _data;
	struct spa_buffer *buf = buffer->buffer;
	struct spa_data *d;
#ifdef HAVE_MEMFD_CREATE
	unsigned int seals;
#endif

	pw_log_info("add buffer %p", buffer);
	d = buf->datas;

	if ((d[0].type & (1<<SPA_DATA_DmaBuf)) > 0) {
		printf("pretend to support dmabufs while setting the fd to -1\n");
		d[0].type = SPA_DATA_DmaBuf;
		d[0].fd = -1;
		d[0].data = NULL;
		return;
	}

	if ((d[0].type & (1<<SPA_DATA_MemFd)) == 0) {
		pw_log_error("unsupported data type %08x", d[0].type);
		return;
	}

	printf("use memfd\n");
	/* create the memfd on the buffer, set the type and flags */
	d[0].type = SPA_DATA_MemFd;
	d[0].flags = SPA_DATA_FLAG_READWRITE;
#ifdef HAVE_MEMFD_CREATE
	d[0].fd = memfd_create("video-src-fixate-memfd", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#else
	d[0].fd = -1;
#endif
	if (d[0].fd == -1) {
		pw_log_error("can't create memfd: %m");
		return;
	}
	d[0].mapoffset = 0;
	d[0].maxsize = data->stride * data->format.size.height;

	/* truncate to the right size before we set seals */
	if (ftruncate(d[0].fd, d[0].maxsize) < 0) {
		pw_log_error("can't truncate to %d: %m", d[0].maxsize);
		return;
	}
#ifdef HAVE_MEMFD_CREATE
	/* not enforced yet but server might require SEAL_SHRINK later */
	seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
	if (fcntl(d[0].fd, F_ADD_SEALS, seals) == -1) {
		pw_log_warn("Failed to add seals: %m");
	}
#endif

	/* now mmap so we can write to it in the process function above */
	d[0].data = mmap(NULL, d[0].maxsize, PROT_READ|PROT_WRITE,
			MAP_SHARED, d[0].fd, d[0].mapoffset);
	if (d[0].data == MAP_FAILED) {
		pw_log_error("can't mmap memory: %m");
		return;
	}
}

/* close the memfd we set on the buffers here */
static void on_stream_remove_buffer(void *_data, struct pw_buffer *buffer)
{
	printf("remove_buffer\n");
	struct spa_buffer *buf = buffer->buffer;
	struct spa_data *d;

	d = buf->datas;
	pw_log_info("remove buffer %p", buffer);
	if ((d[0].type & (1<<SPA_DATA_DmaBuf)) == 0)
		return;

	munmap(d[0].data, d[0].maxsize);
	close(d[0].fd);
}

/* Be notified when the stream param changes. We're only looking at the
 * format param.
 *
 * We are now supposed to call pw_stream_update_params() with success or
 * failure, depending on if we can support the format. Because we gave
 * a list of supported formats, this should be ok.
 *
 * As part of pw_stream_update_params() we can provide parameters that
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
	const struct spa_pod *params[5];
	int blocks, size, stride, buffertypes;

	if (param == NULL || id != SPA_PARAM_Format)
		return;

	printf("param changed: \n");
	spa_debug_format(4, NULL, param);

	spa_format_video_raw_parse(param, &data->format);

	data->stride = SPA_ROUND_UP_N(data->format.size.width * BPP, 4);

	const struct spa_pod_prop *prop_modifier;
	// check if client supports modifier
	if ((prop_modifier = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier)) == NULL) {
		blocks = 1;
		size = data->stride * data->format.size.height;
		stride = data->stride;
		buffertypes = (1<<SPA_DATA_MemFd);
	} else {
		// check if the modifier is fixated
		if ((prop_modifier->flags & SPA_POD_PROP_FLAG_DONT_FIXATE) > 0) {
			const struct spa_pod *pod_modifier = &prop_modifier->value;
			printf("fixating format\n");

			uint32_t n_modifiers = SPA_POD_CHOICE_N_VALUES(pod_modifier);
			uint64_t *modifiers = SPA_POD_CHOICE_VALUES(pod_modifier);
			uint64_t modifier;
			// shortcut for the old gbm allocator path
			if (n_modifiers == 1 && modifiers[0] == DRM_FORMAT_MOD_INVALID) {
				modifier = modifiers[0];
			} else {
				// Use the allocator to find the best modifier from the list
				modifier = modifiers[rand()%n_modifiers];
			}

			params[0] = fixate_format(&b, SPA_VIDEO_FORMAT_RGB, &modifier);

			params[1] = build_format(&b, SPA_VIDEO_FORMAT_RGB,
					supported_modifiers, sizeof(supported_modifiers)/sizeof(supported_modifiers[0]));
			params[2] = build_format(&b, SPA_VIDEO_FORMAT_RGB,
					NULL, 0);

			printf("announcing fixated EnumFormats\n");
			for (unsigned int i=0; i < 3; i++) {
			    spa_debug_format(4, NULL, params[i]);
			}

			pw_stream_update_params(stream, params, 3);
			return;
		}
		printf("no fixation required\n");
		blocks = 1;
		size = data->stride * data->format.size.height;
		stride = data->stride;
		buffertypes = (1<<SPA_DATA_DmaBuf);
	}

	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers,  SPA_POD_CHOICE_RANGE_Int(8, 2, MAX_BUFFERS),
		SPA_PARAM_BUFFERS_blocks,   SPA_POD_Int(blocks),
		SPA_PARAM_BUFFERS_size,     SPA_POD_Int(size),
		SPA_PARAM_BUFFERS_stride,   SPA_POD_Int(stride),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(buffertypes));

	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

	params[2] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
		SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(
					sizeof(struct spa_meta_region) * 16,
					sizeof(struct spa_meta_region) * 1,
					sizeof(struct spa_meta_region) * 16));
	params[3] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region)));
#define CURSOR_META_SIZE(w,h)	(sizeof(struct spa_meta_cursor) + \
				 sizeof(struct spa_meta_bitmap) + w * h * CURSOR_BPP)
	params[4] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
		SPA_PARAM_META_size, SPA_POD_Int(
			CURSOR_META_SIZE(CURSOR_WIDTH,CURSOR_HEIGHT)));

	pw_stream_update_params(stream, params, 5);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
	.state_changed = on_stream_state_changed,
	.param_changed = on_stream_param_changed,
	.add_buffer = on_stream_add_buffer,
	.remove_buffer = on_stream_remove_buffer,
};

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	pw_thread_loop_signal(data->loop, false);
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	const struct spa_pod *params[2];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	srand(32);

	pw_init(&argc, &argv);

	/* create a thread loop and start it */
	data.loop = pw_thread_loop_new("video-src-fixate", NULL);

	/* take the lock around all PipeWire functions. In callbacks, the lock
	 * is already taken for you but it's ok to lock again because the lock is
	 * recursive */
	pw_thread_loop_lock(data.loop);

	/* install some handlers to exit nicely */
	pw_loop_add_signal(pw_thread_loop_get_loop(data.loop), SIGINT, do_quit, &data);
	pw_loop_add_signal(pw_thread_loop_get_loop(data.loop), SIGTERM, do_quit, &data);

	/* start after the signal handlers are set */
	pw_thread_loop_start(data.loop);

	/* create a simple stream, the simple stream manages the core
	 * object for you if you don't want to deal with them.
	 *
	 * We're making a new video provider. We need to set the media-class
	 * property.
	 *
	 * Pass your events and a user_data pointer as the last arguments. This
	 * will inform you about the stream state. The most important event
	 * you need to listen to is the process event where you need to provide
	 * the data.
	 */
	data.stream = pw_stream_new_simple(
			pw_thread_loop_get_loop(data.loop),
			"video-src-fixate",
			pw_properties_new(
				PW_KEY_MEDIA_CLASS, "Video/Source",
				NULL),
			&stream_events,
			&data);

	/* make a timer to schedule our frames */
	data.timer = pw_loop_add_timer(pw_thread_loop_get_loop(data.loop), on_timeout, &data);

	/* build the extra parameter for the connection. Here we make an
	 * EnumFormat parameter which lists the possible formats we can provide.
	 * The server will select a format that matches and informs us about this
	 * in the stream param_changed event.
	 */
	params[0] = build_format(&b, SPA_VIDEO_FORMAT_RGB,
			supported_modifiers, sizeof(supported_modifiers)/sizeof(supported_modifiers[0]));
	params[1] = build_format(&b, SPA_VIDEO_FORMAT_RGB, NULL, 0);

	printf("announcing starting EnumFormats\n");
	for (unsigned int i=0; i < 2; i++) {
	    spa_debug_format(4, NULL, params[i]);
	}

	/* now connect the stream, we need a direction (input/output),
	 * an optional target node to connect to, some flags and parameters.
	 *
	 * Here we pass PW_STREAM_FLAG_ALLOC_BUFFERS. We should in the
	 * add_buffer callback configure the buffer memory. This should be
	 * fd backed memory (memfd, dma-buf, ...) that can be shared with
	 * the server.  */
	pw_stream_connect(data.stream,
			  PW_DIRECTION_OUTPUT,
			  PW_ID_ANY,
			  PW_STREAM_FLAG_DRIVER |
			  PW_STREAM_FLAG_ALLOC_BUFFERS,
			  params, 2);

	/* unlock, run the loop and wait, this will trigger the callbacks */
	pw_thread_loop_wait(data.loop);

	/* unlock before stop */
	pw_thread_loop_unlock(data.loop);
	pw_thread_loop_stop(data.loop);

	pw_stream_destroy(data.stream);

	/* destroy after dependent objects are destroyed */
	pw_thread_loop_destroy(data.loop);
	pw_deinit();

	return 0;
}
