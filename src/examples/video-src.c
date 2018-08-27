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
#include <errno.h>
#include <time.h>
#include <sys/mman.h>

#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>

#include <pipewire/pipewire.h>

#define BPP    3

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
};

static void on_timeout(void *userdata, uint64_t expirations)
{
	struct data *data = userdata;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	int i, j;
	uint8_t *p, *map;
	struct spa_meta_header *h;
	struct spa_meta *m;

	pw_log_trace("timeout");

	b = pw_stream_dequeue_buffer(data->stream);
	if (b == NULL) {
		pw_log_warn("out of buffers");
		return;
	}
	buf = b->buffer;

	if (buf->datas[0].type == SPA_DATA_MemFd ||
	    buf->datas[0].type == SPA_DATA_DmaBuf) {
		map =
		    mmap(NULL, buf->datas[0].maxsize + buf->datas[0].mapoffset,
			 PROT_READ | PROT_WRITE, MAP_SHARED, buf->datas[0].fd, 0);
		if (map == MAP_FAILED) {
			printf("failed to mmap: %s\n", strerror(errno));
			return;
		}
		p = SPA_MEMBER(map, buf->datas[0].mapoffset, uint8_t);
	} else if (buf->datas[0].type == SPA_DATA_MemPtr) {
		map = NULL;
		p = buf->datas[0].data;
	} else
		return;

	if ((h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h)))) {
#if 0
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		h->pts = SPA_TIMESPEC_TO_TIME(&now);
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

	for (i = 0; i < data->format.size.height; i++) {
		for (j = 0; j < data->format.size.width * BPP; j++) {
			p[j] = data->counter + j * i;
		}
		p += buf->datas[0].chunk->stride;
		data->counter += 13;
	}

	if (map)
		munmap(map, buf->datas[0].maxsize + buf->datas[0].mapoffset);

	buf->datas[0].chunk->size = buf->datas[0].maxsize;

	pw_stream_queue_buffer(data->stream, b);
}

static void on_stream_state_changed(void *_data, enum pw_stream_state old, enum pw_stream_state state,
				    const char *error)
{
	struct data *data = _data;

	printf("stream state: \"%s\"\n", pw_stream_state_as_string(state));

	switch (state) {
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
	const struct spa_pod *params[3];

	if (format == NULL) {
		pw_stream_finish_format(stream, 0, NULL, 0);
		return;
	}
	spa_format_video_raw_parse(format, &data->format);

	data->stride = SPA_ROUND_UP_N(data->format.size.width * BPP, 4);

	params[0] = spa_pod_builder_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		":", SPA_PARAM_BUFFERS_buffers, "iru", 2,
			SPA_POD_PROP_MIN_MAX(1, 32),
		":", SPA_PARAM_BUFFERS_blocks,  "i", 1,
		":", SPA_PARAM_BUFFERS_size,    "i", data->stride * data->format.size.height,
		":", SPA_PARAM_BUFFERS_stride,  "i", data->stride,
		":", SPA_PARAM_BUFFERS_align,   "i", 16);

	params[1] = spa_pod_builder_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		":", SPA_PARAM_META_type, "I", SPA_META_Header,
		":", SPA_PARAM_META_size, "i", sizeof(struct spa_meta_header));

	params[2] = spa_pod_builder_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		":", SPA_PARAM_META_type, "I", SPA_META_VideoDamage,
		":", SPA_PARAM_META_size, "iru", sizeof(struct spa_meta_region) * 16,
			SPA_POD_PROP_MIN_MAX(sizeof(struct spa_meta_region) * 1,
					     sizeof(struct spa_meta_region) * 16));

	pw_stream_finish_format(stream, 0, params, 3);
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

		params[0] = spa_pod_builder_object(&b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			"I", SPA_MEDIA_TYPE_video,
			"I", SPA_MEDIA_SUBTYPE_raw,
			":", SPA_FORMAT_VIDEO_format,    "I", SPA_VIDEO_FORMAT_RGB,
			":", SPA_FORMAT_VIDEO_size,      "Rru", &SPA_RECTANGLE(320, 240),
				SPA_POD_PROP_MIN_MAX(&SPA_RECTANGLE(1, 1),
						     &SPA_RECTANGLE(4096, 4096)),
			":", SPA_FORMAT_VIDEO_framerate, "F", &SPA_FRACTION(25, 1));

		pw_stream_add_listener(data->stream,
				       &data->stream_listener,
				       &stream_events,
				       data);

		pw_stream_connect(data->stream,
				  PW_DIRECTION_OUTPUT,
				  NULL, PW_STREAM_FLAG_DRIVER,
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
