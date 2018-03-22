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

#define BPP    3

struct data {
	struct type type;

	struct pw_main_loop *loop;
	struct spa_source *timer;

	struct pw_core *core;
	struct pw_type *t;
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

	pw_log_trace("timeout");

	b = pw_stream_dequeue_buffer(data->stream);
	if (b == NULL) {
		pw_log_warn("out of buffers");
		return;
	}
	buf = b->buffer;

	if (buf->datas[0].type == data->t->data.MemFd ||
	    buf->datas[0].type == data->t->data.DmaBuf) {
		map =
		    mmap(NULL, buf->datas[0].maxsize + buf->datas[0].mapoffset,
			 PROT_READ | PROT_WRITE, MAP_SHARED, buf->datas[0].fd, 0);
		if (map == MAP_FAILED) {
			printf("failed to mmap: %s\n", strerror(errno));
			return;
		}
		p = SPA_MEMBER(map, buf->datas[0].mapoffset, uint8_t);
	} else if (buf->datas[0].type == data->t->data.MemPtr) {
		map = NULL;
		p = buf->datas[0].data;
	} else
		return;

	if ((h = spa_buffer_find_meta(buf, data->t->meta.Header))) {
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
on_stream_format_changed(void *_data, struct spa_pod *format)
{
	struct data *data = _data;
	struct pw_stream *stream = data->stream;
	struct pw_type *t = data->t;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	struct spa_pod *params[2];

	if (format == NULL) {
		pw_stream_finish_format(stream, 0, NULL, 0);
		return;
	}
	spa_format_video_raw_parse(format, &data->format, &data->type.format_video);

	data->stride = SPA_ROUND_UP_N(data->format.size.width * BPP, 4);

	params[0] = spa_pod_builder_object(&b,
		t->param.idBuffers, t->param_buffers.Buffers,
		":", t->param_buffers.size,    "i", data->stride * data->format.size.height,
		":", t->param_buffers.stride,  "i", data->stride,
		":", t->param_buffers.buffers, "iru", 2,
			SPA_POD_PROP_MIN_MAX(1, 32),
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
				pw_properties_new("node.driver", "true", NULL));

		params[0] = spa_pod_builder_object(&b,
			data->t->param.idEnumFormat, data->t->spa_format,
			"I", data->type.media_type.video,
			"I", data->type.media_subtype.raw,
			":", data->type.format_video.format,    "I", data->type.video_format.RGB,
			":", data->type.format_video.size,      "Rru", &SPA_RECTANGLE(320, 240),
				SPA_POD_PROP_MIN_MAX(&SPA_RECTANGLE(1, 1),
						     &SPA_RECTANGLE(4096, 4096)),
			":", data->type.format_video.framerate, "F", &SPA_FRACTION(25, 1));

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
	data.t = pw_core_get_type(data.core);
	data.remote = pw_remote_new(data.core, NULL, 0);

	init_type(&data.type, data.t->map);

	data.timer = pw_loop_add_timer(pw_main_loop_get_loop(data.loop), on_timeout, &data);

	pw_remote_add_listener(data.remote, &data.remote_listener, &remote_events, &data);

	pw_remote_connect(data.remote);

	pw_main_loop_run(data.loop);

	pw_core_destroy(data.core);
	pw_main_loop_destroy(data.loop);

	return 0;
}
