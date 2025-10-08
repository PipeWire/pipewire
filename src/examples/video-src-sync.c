/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2025 Wim Taymans */
/* SPDX-License-Identifier: MIT */

/*
 [title]
 Video source using \ref pw_stream and sync_timeline.
 [title]
 */

#include "config.h"

#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>

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

	int res;

	bool with_synctimeline;
	bool with_synctimeline_release;
};

static void on_process(void *userdata)
{
	struct data *data = userdata;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	uint32_t i, j;
	uint8_t *p;
	struct spa_meta_header *h;
	struct spa_meta_sync_timeline *stl;
	uint64_t cmd;

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
	if ((stl = spa_buffer_find_meta_data(buf, SPA_META_SyncTimeline, sizeof(*stl))) &&
	    stl->release_point) {
		if (!SPA_FLAG_IS_SET(stl->flags, SPA_META_SYNC_TIMELINE_UNSCHEDULED_RELEASE)) {
			/* The other end promised to schedule the release point, wait before we
			 * can use the buffer */
			if (read(buf->datas[2].fd, &cmd, sizeof(cmd)) < 0)
				pw_log_warn("release_point wait error %m");
			pw_log_debug("release_point:%"PRIu64, stl->release_point);
		} else if (spa_buffer_has_meta_features(buf, SPA_META_SyncTimeline,
					SPA_META_FEATURE_SYNC_TIMELINE_RELEASE)) {
			/* this happens when the other end did not get the buffer or
			 * will not trigger the release point, There is no point waiting,
			 * we can use the buffer right away */
			pw_log_warn("release_point not scheduled:%"PRIu64, stl->release_point);
		} else {
			/* The other end does not support the RELEASE flag, we don't
			 * know if the buffer was used or not or if the release point will
			 * ever be scheduled, we must assume we can reuse the buffer */
			pw_log_debug("assume buffer was released:%"PRIu64, stl->release_point);
		}
	}

	for (i = 0; i < data->format.size.height; i++) {
		for (j = 0; j < data->format.size.width * BPP; j++)
			p[j] = data->counter + j * i;
		p += data->stride;
		data->counter += 13;
	}

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->size = data->format.size.height * data->stride;
	buf->datas[0].chunk->stride = data->stride;

	if (stl) {
		/* set the UNSCHEDULED_RELEASE flag, the consumer will clear this if
		 * it promises to signal the release point */
		SPA_FLAG_SET(stl->flags, SPA_META_SYNC_TIMELINE_UNSCHEDULED_RELEASE);
		cmd = 1;
		stl->acquire_point = data->seq;
		stl->release_point = data->seq;
		/* write the acquire point */
		write(buf->datas[1].fd, &cmd, sizeof(cmd));
	}
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

		printf("driving:%d\n", pw_stream_is_driving(data->stream));

		if (pw_stream_is_driving(data->stream))
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
	struct spa_pod_frame f;

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

	/* first add Buffer with 3 blocks (1 data, 2 sync fds). */
	if (data->with_synctimeline) {
		spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
		spa_pod_builder_add(&b,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(3),
			SPA_PARAM_BUFFERS_size,    SPA_POD_Int(data->stride * data->format.size.height),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(data->stride),
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
		SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size,    SPA_POD_Int(data->stride * data->format.size.height),
		SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(data->stride),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1<<SPA_DATA_MemFd)));

	params[n_params++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

	pw_stream_update_params(stream, params, n_params);
}

/* we set the PW_STREAM_FLAG_ALLOC_BUFFERS flag when connecting so we need
 * to provide buffer memory.  */
static void on_stream_add_buffer(void *_data, struct pw_buffer *buffer)
{
	struct data *data = _data;
	struct spa_buffer *buf = buffer->buffer;
	struct spa_data *d;
#ifdef HAVE_MEMFD_CREATE
	unsigned int seals;
#endif
	struct spa_meta_sync_timeline *s;

	d = buf->datas;

	pw_log_debug("add buffer %p", buffer);
	if ((d[0].type & (1<<SPA_DATA_MemFd)) == 0) {
		pw_log_error("unsupported data type %08x", d[0].type);
		return;
	}

	/* create the memfd on the buffer, set the type and flags */
	d[0].type = SPA_DATA_MemFd;
	d[0].flags = SPA_DATA_FLAG_READWRITE | SPA_DATA_FLAG_MAPPABLE;
 #ifdef HAVE_MEMFD_CREATE
	d[0].fd = memfd_create("video-src-memfd", MFD_CLOEXEC | MFD_ALLOW_SEALING);
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

	if ((s = spa_buffer_find_meta_data(buf, SPA_META_SyncTimeline, sizeof(*s))) && buf->n_datas >= 3) {
		pw_log_debug("got sync timeline");
		/* acquire fd (just an example, not really syncobj here) */
		d[1].type = SPA_DATA_SyncObj;
		d[1].flags = SPA_DATA_FLAG_READWRITE;
		d[1].fd = eventfd(0, EFD_CLOEXEC);
		d[1].mapoffset = 0;
		d[1].maxsize = 0;
		if (d[1].fd == -1) {
			pw_log_error("can't create acquire fd: %m");
			return;
		}
		/* release fd (just an example, not really syncobj here) */
		d[2].type = SPA_DATA_SyncObj;
		d[2].flags = SPA_DATA_FLAG_READWRITE;
		d[2].fd = eventfd(0, EFD_CLOEXEC);
		d[2].mapoffset = 0;
		d[2].maxsize = 0;
		if (d[2].fd == -1) {
			pw_log_error("can't create release fd: %m");
			return;
		}
	}
	if (spa_buffer_has_meta_features(buf, SPA_META_SyncTimeline,
				SPA_META_FEATURE_SYNC_TIMELINE_RELEASE)) {
		pw_log_debug("got sync timeline release");
	}
}

/* close the memfd we set on the buffers here */
static void on_stream_remove_buffer(void *_data, struct pw_buffer *buffer)
{
	struct spa_buffer *buf = buffer->buffer;
	struct spa_data *d;

	d = buf->datas;
	pw_log_debug("remove buffer %p", buffer);

	munmap(d[0].data, d[0].maxsize);
	close(d[0].fd);
	if (buf->n_datas >= 3) {
		close(d[1].fd);
		close(d[2].fd);
	}
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
	pw_main_loop_quit(data->loop);
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	const struct spa_pod *params[2];
	uint32_t n_params = 0;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);

	data.with_synctimeline = true;
	data.with_synctimeline_release = true;

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

	data.stream = pw_stream_new(data.core, "video-src-sync",
		pw_properties_new(
			PW_KEY_MEDIA_CLASS, "Video/Source",
			NULL));

	params[n_params++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType,       SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype,    SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format,    SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA),
		SPA_FORMAT_VIDEO_size,      SPA_POD_CHOICE_RANGE_Rectangle(
						&SPA_RECTANGLE(320, 240),
						&SPA_RECTANGLE(1, 1),
						&SPA_RECTANGLE(4096, 4096)),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(25, 1)));

	pw_stream_add_listener(data.stream,
			       &data.stream_listener,
			       &stream_events,
			       &data);

	pw_stream_connect(data.stream,
			  PW_DIRECTION_OUTPUT,
			  PW_ID_ANY,
			  PW_STREAM_FLAG_DRIVER |
			  PW_STREAM_FLAG_ALLOC_BUFFERS |
			  PW_STREAM_FLAG_MAP_BUFFERS,
			  params, n_params);

	pw_main_loop_run(data.loop);

cleanup:
	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.loop);
	pw_deinit();

	return data.res;
}
