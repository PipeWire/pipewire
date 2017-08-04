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

#include <spa/type-map.h>
#include <spa/format-utils.h>
#include <spa/video/format-utils.h>
#include <spa/format-builder.h>
#include <spa/props.h>
#include <spa/lib/debug.h>

#include <pipewire/pipewire.h>
#include <pipewire/sig.h>

struct type {
	uint32_t format;
	uint32_t props;
	struct spa_type_meta meta;
	struct spa_type_data data;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_video format_video;
	struct spa_type_video_format video_format;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
	type->props = spa_type_map_get_id(map, SPA_TYPE__Props);
	spa_type_meta_map(map, &type->meta);
	spa_type_data_map(map, &type->data);
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_video_map(map, &type->format_video);
	spa_type_video_format_map(map, &type->video_format);
}

#define BPP    3

struct data {
	struct type type;

	bool running;
	struct pw_loop *loop;
	struct spa_source *timer;

	struct pw_core *core;
	struct pw_type *t;
	struct pw_remote *remote;
	struct pw_callback_info remote_callbacks;

	struct pw_stream *stream;
	struct pw_callback_info stream_callbacks;

	struct spa_video_info_raw format;
	int32_t stride;

	uint8_t params_buffer[1024];
	int counter;
	uint32_t seq;
};

static void on_timeout(struct spa_loop_utils *utils, struct spa_source *source, void *userdata)
{
	struct data *data = userdata;
	uint32_t id;
	struct spa_buffer *buf;
	int i, j;
	uint8_t *p, *map;
	struct spa_meta_header *h;

	id = pw_stream_get_empty_buffer(data->stream);
	if (id == SPA_ID_INVALID)
		return;

	buf = pw_stream_peek_buffer(data->stream, id);

	if (buf->datas[0].type == data->type.data.MemFd) {
		map =
		    mmap(NULL, buf->datas[0].maxsize + buf->datas[0].mapoffset,
			 PROT_READ | PROT_WRITE, MAP_SHARED, buf->datas[0].fd, 0);
		if (map == MAP_FAILED) {
			printf("failed to mmap: %s\n", strerror(errno));
			return;
		}
		p = SPA_MEMBER(map, buf->datas[0].mapoffset, uint8_t);
	} else if (buf->datas[0].type == data->type.data.MemPtr) {
		map = NULL;
		p = buf->datas[0].data;
	} else
		return;

	if ((h = spa_buffer_find_meta(buf, data->type.meta.Header))) {
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

	pw_stream_send_buffer(data->stream, id);
}

static void on_stream_state_changed(void *_data, enum pw_stream_state old, enum pw_stream_state state,
				    const char *error)
{
	struct data *data = _data;

	printf("stream state: \"%s\"\n", pw_stream_state_as_string(state));

	switch (state) {
	case PW_STREAM_STATE_PAUSED:
		pw_loop_update_timer(data->loop, data->timer, NULL, NULL, false);
		break;

	case PW_STREAM_STATE_STREAMING:
	{
		struct timespec timeout, interval;

		timeout.tv_sec = 0;
		timeout.tv_nsec = 1;
		interval.tv_sec = 0;
		interval.tv_nsec = 40 * SPA_NSEC_PER_MSEC;

		pw_loop_update_timer(data->loop, data->timer, &timeout, &interval, false);
		break;
	}
	default:
		break;
	}
}

#define PROP(f,key,type,...)							\
	SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
#define PROP_U_MM(f,key,type,...)						\
	SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |				\
			SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)

static void
on_stream_format_changed(void *_data, struct spa_format *format)
{
	struct data *data = _data;
	struct pw_stream *stream = data->stream;
	struct pw_type *t = data->t;
	struct spa_pod_builder b = { NULL };
	struct spa_pod_frame f[2];
	struct spa_param *params[2];

	if (format == NULL) {
		pw_stream_finish_format(stream, SPA_RESULT_OK, NULL, 0);
		return;
	}
	spa_format_video_raw_parse(format, &data->format, &data->type.format_video);

	data->stride = SPA_ROUND_UP_N(data->format.size.width * BPP, 4);

	spa_pod_builder_init(&b, data->params_buffer, sizeof(data->params_buffer));
	spa_pod_builder_object(&b, &f[0], 0, t->param_alloc_buffers.Buffers,
		PROP(&f[1], t->param_alloc_buffers.size, SPA_POD_TYPE_INT,
			data->stride * data->format.size.height),
		PROP(&f[1], t->param_alloc_buffers.stride, SPA_POD_TYPE_INT,
			data->stride),
		PROP_U_MM(&f[1], t->param_alloc_buffers.buffers, SPA_POD_TYPE_INT,
			32,
			2, 32),
		PROP(&f[1], t->param_alloc_buffers.align, SPA_POD_TYPE_INT,
			16));
	params[0] = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_param);

	spa_pod_builder_object(&b, &f[0], 0, t->param_alloc_meta_enable.MetaEnable,
		PROP(&f[1], t->param_alloc_meta_enable.type, SPA_POD_TYPE_ID,
			t->meta.Header),
		PROP(&f[1], t->param_alloc_meta_enable.size, SPA_POD_TYPE_INT,
			sizeof(struct spa_meta_header)));
	params[1] = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_param);

	pw_stream_finish_format(stream, SPA_RESULT_OK, params, 2);
}

static const struct pw_stream_callbacks stream_callbacks = {
	PW_VERSION_STREAM_CALLBACKS,
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
		data->running = false;
		break;

	case PW_REMOTE_STATE_CONNECTED:
	{
		const struct spa_format *formats[1];
		uint8_t buffer[1024];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		struct spa_pod_frame f[2];

		printf("remote state: \"%s\"\n",
		       pw_remote_state_as_string(state));

		data->stream = pw_stream_new(remote, "video-src", NULL);

		spa_pod_builder_format(&b, &f[0], data->type.format,
			data->type.media_type.video,
			data->type.media_subtype.raw,
			PROP(&f[1], data->type.format_video.format, SPA_POD_TYPE_ID,
				data->type.video_format.RGB),
			PROP_U_MM(&f[1], data->type.format_video.size, SPA_POD_TYPE_RECTANGLE,
				320, 240,
				1, 1, 4096, 4096),
			PROP(&f[1], data->type.format_video.framerate, SPA_POD_TYPE_FRACTION,
				25, 1));
		formats[0] = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_format);

		pw_stream_add_callbacks(data->stream,
					&data->stream_callbacks,
					&stream_callbacks,
					data);

		pw_stream_connect(data->stream,
				  PW_DIRECTION_OUTPUT,
				  PW_STREAM_MODE_BUFFER,
				  NULL, PW_STREAM_FLAG_NONE, 1, formats);
		break;
	}
	default:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));
		break;
	}
}

static const struct pw_remote_callbacks remote_callbacks = {
	PW_VERSION_REMOTE_CALLBACKS,
	.state_changed = on_state_changed,
};

int main(int argc, char *argv[])
{
	struct data data = { 0, };

	pw_init(&argc, &argv);

	data.loop = pw_loop_new();
	data.running = true;
	data.core = pw_core_new(data.loop, NULL);
	data.t = pw_core_get_type(data.core);
	data.remote = pw_remote_new(data.core, NULL);

	init_type(&data.type, data.t->map);

	data.timer = pw_loop_add_timer(data.loop, on_timeout, &data);

	pw_remote_add_callbacks(data.remote, &data.remote_callbacks, &remote_callbacks, &data);

	pw_remote_connect(data.remote);

	pw_loop_enter(data.loop);
	while (data.running) {
		pw_loop_iterate(data.loop, -1);
	}
	pw_loop_leave(data.loop);

	pw_remote_destroy(data.remote);
	pw_loop_destroy(data.loop);

	return 0;
}
