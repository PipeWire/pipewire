/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

/*
 [title]
 Video source using \ref pw_stream.
 [title]
 */

#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <math.h>

#include <spa/param/video/format-utils.h>
#include <spa/param/tag-utils.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>

#include <pipewire/pipewire.h>

#define BPP		4
#define CURSOR_WIDTH	64
#define CURSOR_HEIGHT	64
#define CURSOR_BPP	4

#define MAX_BUFFERS	64

#define M_PI_M2 ( M_PI + M_PI )

struct data {
	struct pw_main_loop *loop;
	struct spa_source *timer;

	struct pw_context *context;
	struct pw_core *core;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct spa_video_info_raw format;
	int32_t stride;

	int counter;
	uint32_t seq;

	double crop;
	double accumulator;
	int res;
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
	if ((p = buf->datas[0].data) == NULL)
		return;

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
		mc->region.position.x = (int32_t)data->crop;
		mc->region.position.y = (int32_t)data->crop;
		mc->region.size.width = data->format.size.width - (int32_t)(data->crop*2);
		mc->region.size.height = data->format.size.height - (int32_t)(data->crop*2);
	}
	if ((mcs = spa_buffer_find_meta_data(buf, SPA_META_Cursor, sizeof(*mcs)))) {
		struct spa_meta_bitmap *mb;
		uint32_t *bitmap, color;

		mcs->id = 1;
		mcs->position.x = (int32_t)((sin(data->accumulator) + 1.0) * 160.0 + 80);
		mcs->position.y = (int32_t)((cos(data->accumulator) + 1.0) * 100.0 + 50);
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
		color = (uint32_t)((cos(data->accumulator) + 1.0) * (1 << 23));
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

	pw_stream_queue_buffer(data->stream, b);
}

static void on_timeout(void *userdata, uint64_t expirations)
{
	struct data *data = userdata;
	pw_log_trace("timeout");
	pw_stream_trigger_process(data->stream);
}

static void on_stream_state_changed(void *_data, enum pw_stream_state old, enum pw_stream_state state,
				    const char *error)
{
	struct data *data = _data;

	printf("stream state: \"%s\" %s\n", pw_stream_state_as_string(state), error ? error : "");

	switch (state) {
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
		pw_main_loop_quit(data->loop);
		break;

	case PW_STREAM_STATE_PAUSED:
		printf("node id: %d\n", pw_stream_get_node_id(data->stream));
		pw_loop_update_timer(pw_main_loop_get_loop(data->loop),
				data->timer, NULL, NULL, false);
		break;
	case PW_STREAM_STATE_STREAMING:
	{
		struct timespec timeout, interval;

		timeout.tv_sec = 0;
		timeout.tv_nsec = 1;
		interval.tv_sec = 0;
		interval.tv_nsec = 40 * SPA_NSEC_PER_MSEC;

		printf("driving:%d lazy:%d\n",
				pw_stream_is_driving(data->stream),
				pw_stream_is_lazy(data->stream));

		if (pw_stream_is_driving(data->stream) != pw_stream_is_lazy(data->stream))
			pw_loop_update_timer(pw_main_loop_get_loop(data->loop),
					data->timer, &timeout, &interval, false);
		break;
	}
	default:
		break;
	}
}

static void
on_stream_param_changed(void *_data, uint32_t id, const struct spa_pod *param)
{
	struct data *data = _data;
	struct pw_stream *stream = data->stream;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[5];
	uint32_t n_params = 0;

	if (param != NULL && id == SPA_PARAM_Tag) {
		spa_debug_pod(0, NULL, param);
		return;
	}
	if (param == NULL || id != SPA_PARAM_Format)
		return;

	fprintf(stderr, "got format:\n");
	spa_debug_format(2, NULL, param);

	spa_format_video_raw_parse(param, &data->format);

	data->stride = SPA_ROUND_UP_N(data->format.size.width * BPP, 4);

	params[n_params++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, MAX_BUFFERS),
		SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size,    SPA_POD_Int(data->stride * data->format.size.height),
		SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(data->stride));

	params[n_params++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

	params[n_params++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
		SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(
					sizeof(struct spa_meta_region) * 16,
					sizeof(struct spa_meta_region) * 1,
					sizeof(struct spa_meta_region) * 16));
	params[n_params++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region)));
#define CURSOR_META_SIZE(w,h)	(sizeof(struct spa_meta_cursor) + \
				 sizeof(struct spa_meta_bitmap) + w * h * CURSOR_BPP)
	params[n_params++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
		SPA_PARAM_META_size, SPA_POD_Int(
			CURSOR_META_SIZE(CURSOR_WIDTH,CURSOR_HEIGHT)));

	pw_stream_update_params(stream, params, n_params);
}

static void
on_trigger_done(void *_data)
{
	pw_log_trace("trigger done");
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
	.state_changed = on_stream_state_changed,
	.param_changed = on_stream_param_changed,
	.trigger_done = on_trigger_done,
};

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

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);

	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);

	data.context = pw_context_new(pw_main_loop_get_loop(data.loop), NULL, 0);

	data.timer = pw_loop_add_timer(pw_main_loop_get_loop(data.loop), on_timeout, &data);

	data.core = pw_context_connect(data.context, NULL, 0);
	if (data.core == NULL) {
		fprintf(stderr, "can't connect: %m\n");
		data.res = -errno;
		goto cleanup;
	}

	data.stream = pw_stream_new(data.core, "video-src",
		pw_properties_new(
			PW_KEY_MEDIA_CLASS, "Video/Source",
			PW_KEY_NODE_SUPPORTS_REQUEST, "1",
			NULL));

	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType,       SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype,    SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format,    SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA),
		SPA_FORMAT_VIDEO_size,      SPA_POD_CHOICE_RANGE_Rectangle(
						&SPA_RECTANGLE(320, 240),
						&SPA_RECTANGLE(1, 1),
						&SPA_RECTANGLE(4096, 4096)),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(25, 1)));

	{
		struct spa_pod_frame f;
		/* send a tag, output tags travel downstream */
		spa_tag_build_start(&b, &f, SPA_PARAM_Tag, SPA_DIRECTION_OUTPUT);
		spa_tag_build_add_dict(&b,
				&SPA_DICT_ITEMS(
					SPA_DICT_ITEM("my-tag-key", "my-special-tag-value")));
		params[1] = spa_tag_build_end(&b, &f);
	}

	pw_stream_add_listener(data.stream,
			       &data.stream_listener,
			       &stream_events,
			       &data);

	pw_stream_connect(data.stream,
			  PW_DIRECTION_OUTPUT,
			  PW_ID_ANY,
			  PW_STREAM_FLAG_DRIVER |
			  PW_STREAM_FLAG_MAP_BUFFERS,
			  params, 2);

	pw_main_loop_run(data.loop);

cleanup:
	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);
	pw_deinit();

	return data.res;
}
