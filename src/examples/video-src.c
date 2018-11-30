/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
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
#include <errno.h>
#include <time.h>
#include <math.h>

#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>

#include <pipewire/pipewire.h>

#define BPP	3
#define WIDTH	320
#define HEIGHT	200
#define CROP	8

#define M_PI_M2 ( M_PI + M_PI )

struct data {
	struct pw_main_loop *loop;
	struct spa_source *timer;

	struct pw_core *core;
	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct spa_video_info_raw format;
	int32_t stride;

	int counter;
	uint32_t seq;

	double crop;
	double accumulator;
};

static void on_timeout(void *userdata, uint64_t expirations)
{
	struct data *data = userdata;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	int i, j;
	uint8_t *p;
	struct spa_meta *m;
	struct spa_meta_header *h;
	struct spa_meta_region *mc;
	struct spa_meta_cursor *mcs;

	pw_log_trace("timeout");

	if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
		pw_log_warn("out of buffers");
		return;
	}

	buf = b->buffer;
	if ((p = buf->datas[0].data) == NULL)
		return;

	if ((h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h)))) {
#if 0
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		h->pts = SPA_TIMESPEC_TO_NSEC(&now);
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
		mc->region.size.width = WIDTH - data->crop*2;
		mc->region.size.height = HEIGHT - data->crop*2;
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

		mb = SPA_MEMBER(mcs, mcs->bitmap_offset, struct spa_meta_bitmap);
		mb->format = SPA_VIDEO_FORMAT_ARGB;
		mb->size.width = 64;
		mb->size.height = 64;
		mb->stride = 64 * 4;
		mb->offset = sizeof(struct spa_meta_bitmap);

		bitmap = SPA_MEMBER(mb, mb->offset, uint32_t);
		color = (cos(data->accumulator) + 1.0) * (1 << 23);
		color |= 0xff000000;

		for (i = 0; i < mb->size.height; i++) {
			for (j = 0; j < mb->size.width; j++) {
				int v = (i - 32) * (i - 32) + (j - 32) * (j - 32);
				bitmap[i*64+j] = (v <= 32*32) ? color : 0x00000000;
			}
		}
	}

	for (i = 0; i < data->format.size.height; i++) {
		for (j = 0; j < data->format.size.width * BPP; j++) {
			p[j] = data->counter + j * i;
		}
		p += buf->datas[0].chunk->stride;
		data->counter += 13;
	}

	data->accumulator += M_PI_M2 / 50.0;
	if (data->accumulator >= M_PI_M2)
		data->accumulator -= M_PI_M2;

	buf->datas[0].chunk->size = buf->datas[0].maxsize;

	pw_stream_queue_buffer(data->stream, b);
}

static void on_stream_state_changed(void *_data, enum pw_stream_state old, enum pw_stream_state state,
				    const char *error)
{
	struct data *data = _data;

	printf("stream state: \"%s\"\n", pw_stream_state_as_string(state));

	switch (state) {
	case PW_STREAM_STATE_CONFIGURE:
		printf("node id: %d\n", pw_stream_get_node_id(data->stream));
		break;
	case PW_STREAM_STATE_STREAMING:
	{
		struct timespec timeout, interval;

		timeout.tv_sec = 0;
		timeout.tv_nsec = 1;
		interval.tv_sec = 0;
		interval.tv_nsec = 40 * SPA_NSEC_PER_MSEC;

		pw_loop_update_timer(pw_main_loop_get_loop(data->loop),
				data->timer, &timeout, &interval, false);
		break;
	}
	default:
		pw_loop_update_timer(pw_main_loop_get_loop(data->loop),
				data->timer, NULL, NULL, false);
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
	const struct spa_pod *params[5];

	if (format == NULL) {
		pw_stream_finish_format(stream, 0, NULL, 0);
		return;
	}
	spa_format_video_raw_parse(format, &data->format);

	data->stride = SPA_ROUND_UP_N(data->format.size.width * BPP, 4);

	params[0] = spa_pod_builder_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, &SPA_POD_CHOICE_RANGE_Int(2, 1, 32),
		SPA_PARAM_BUFFERS_blocks,  &SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size,    &SPA_POD_Int(data->stride * data->format.size.height),
		SPA_PARAM_BUFFERS_stride,  &SPA_POD_Int(data->stride),
		SPA_PARAM_BUFFERS_align,   &SPA_POD_Int(16),
		0);

	params[1] = spa_pod_builder_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, &SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, &SPA_POD_Int(sizeof(struct spa_meta_header)),
		0);

	params[2] = spa_pod_builder_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, &SPA_POD_Id(SPA_META_VideoDamage),
		SPA_PARAM_META_size, &SPA_POD_CHOICE_RANGE_Int(
					sizeof(struct spa_meta_region) * 16,
					sizeof(struct spa_meta_region) * 1,
					sizeof(struct spa_meta_region) * 16),
		0);
	params[3] = spa_pod_builder_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, &SPA_POD_Id(SPA_META_VideoCrop),
		SPA_PARAM_META_size, &SPA_POD_Int(sizeof(struct spa_meta_region)),
		0);
	params[4] = spa_pod_builder_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, &SPA_POD_Id(SPA_META_Cursor),
		SPA_PARAM_META_size, &SPA_POD_Int(sizeof(struct spa_meta_cursor) +
					      sizeof(struct spa_meta_bitmap) +
					      64 * 64 * 4),
		0);

	pw_stream_finish_format(stream, 0, params, 5);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.format_changed = on_stream_format_changed,
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

		printf("remote state: \"%s\"\n",
		       pw_remote_state_as_string(state));

		data->stream = pw_stream_new(remote, "video-src",
			pw_properties_new(
				"media.class", "Video/Source",
				NULL));

#if 0
		params[0] = spa_pod_builder_object(&b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,       &SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,    &SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_VIDEO_format,    &SPA_POD_Id(SPA_VIDEO_FORMAT_RGB),
			SPA_FORMAT_VIDEO_size,      &SPA_POD_CHOICE_RANGE_Rectangle(
							SPA_RECTANGLE(320, 240),
							SPA_RECTANGLE(1, 1),
							SPA_RECTANGLE(4096, 4096)),
			SPA_FORMAT_VIDEO_framerate, &SPA_POD_Fraction(SPA_FRACTION(25, 1)),
			0);
#else
		params[0] = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			":", SPA_FORMAT_mediaType,     "I", SPA_MEDIA_TYPE_video,
			":", SPA_FORMAT_mediaSubtype,  "I", SPA_MEDIA_SUBTYPE_raw,
			":", SPA_FORMAT_VIDEO_format,  "I", SPA_VIDEO_FORMAT_RGB,
			":", SPA_FORMAT_VIDEO_size,    "?rR", 3,
						&SPA_RECTANGLE(320, 240),
						&SPA_RECTANGLE(1, 1),
						&SPA_RECTANGLE(4096, 4096),
			":", SPA_FORMAT_VIDEO_framerate, "F", &SPA_FRACTION(25, 1));
#endif

		pw_stream_add_listener(data->stream,
				       &data->stream_listener,
				       &stream_events,
				       data);

		pw_stream_connect(data->stream,
				  PW_DIRECTION_OUTPUT,
				  SPA_ID_INVALID,
				  PW_STREAM_FLAG_DRIVER |
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

int main(int argc, char *argv[])
{
	struct data data = { 0, };

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);
	data.core = pw_core_new(pw_main_loop_get_loop(data.loop), NULL);
	data.remote = pw_remote_new(data.core, NULL, 0);

	data.timer = pw_loop_add_timer(pw_main_loop_get_loop(data.loop), on_timeout, &data);

	pw_remote_add_listener(data.remote, &data.remote_listener, &remote_events, &data);

	pw_remote_connect(data.remote);

	pw_main_loop_run(data.loop);

	pw_core_destroy(data.core);
	pw_main_loop_destroy(data.loop);

	return 0;
}
