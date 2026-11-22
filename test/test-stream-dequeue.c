/* PipeWire */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>
#include <time.h>

#include "pwtest.h"

#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include <pipewire/pipewire.h>

#define WIDTH		320
#define HEIGHT		240
#define TIMEOUT_MS	5000
#define SETTLE_MS	100

struct data {
	struct pw_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_stream *stream;
	struct spa_hook listener;
	enum pw_stream_state state;
	void *buffers_io;
};

static void state_changed(void *userdata, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct data *d = userdata;
	d->state = state;
}

static void io_changed(void *userdata, uint32_t id, void *area, uint32_t size)
{
	struct data *d = userdata;
	if (id == SPA_IO_Buffers)
		d->buffers_io = area;
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = state_changed,
	.io_changed = io_changed,
};

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* An output stream that is connected but not linked has no SPA_IO_Buffers
 * area. Dequeueing a buffer from it must report that none is available. */
static void check_dequeue_without_io(const char *mode)
{
	struct data d = { 0 };
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];
	struct pw_buffer *buf;
	uint64_t end;
	int err;

	pw_init(0, NULL);
	d.loop = pw_loop_new(NULL);
	pwtest_ptr_notnull(d.loop);
	d.context = pw_context_new(d.loop, NULL, 0);
	pwtest_ptr_notnull(d.context);
	d.core = pw_context_connect(d.context, NULL, 0);
	pwtest_ptr_notnull(d.core);

	pw_loop_enter(d.loop);

	d.stream = pw_stream_new(d.core, "dequeue-without-io",
			pw_properties_new(
				PW_KEY_MEDIA_TYPE, "Video",
				PW_KEY_MEDIA_CATEGORY, "Capture",
				PW_KEY_MEDIA_CLASS, "Video/Source",
				PW_KEY_NODE_OUTPUT_MODE, mode,
				PW_KEY_NODE_AUTOCONNECT, "false",
				NULL));
	pwtest_ptr_notnull(d.stream);
	pw_stream_add_listener(d.stream, &d.listener, &stream_events, &d);
	params[0] = spa_format_video_raw_build(&b, SPA_PARAM_EnumFormat,
			&SPA_VIDEO_INFO_RAW_INIT(
				.format = SPA_VIDEO_FORMAT_YUY2,
				.size = SPA_RECTANGLE(WIDTH, HEIGHT),
				.framerate = SPA_FRACTION(25, 1)));
	pwtest_neg_errno_ok(pw_stream_connect(d.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS, params, 1));

	end = now_ms() + TIMEOUT_MS;
	while (d.state != PW_STREAM_STATE_PAUSED && now_ms() < end)
		pw_loop_iterate(d.loop, 10);
	/* keep going a little so that a late io_changed would be seen */
	end = now_ms() + SETTLE_MS;
	while (now_ms() < end)
		pw_loop_iterate(d.loop, 10);

	pwtest_int_eq(d.state, PW_STREAM_STATE_PAUSED);
	pwtest_ptr_null(d.buffers_io);
	fprintf(stderr, "mode:%s state:%s buffers_io:%p\n", mode,
			pw_stream_state_as_string(d.state), d.buffers_io);

	errno = 0;
	buf = pw_stream_dequeue_buffer(d.stream);
	err = errno;
	pwtest_ptr_null(buf);
	pwtest_int_eq(err, EPIPE);

	pw_stream_destroy(d.stream);
	pw_loop_leave(d.loop);
	pw_core_disconnect(d.core);
	pw_context_destroy(d.context);
	pw_loop_destroy(d.loop);
	pw_deinit();
}

PWTEST(stream_dequeue_without_io_fast)
{
	check_dequeue_without_io("fast");
	return PWTEST_PASS;
}

PWTEST(stream_dequeue_without_io_reliable)
{
	check_dequeue_without_io("reliable");
	return PWTEST_PASS;
}

PWTEST_SUITE(stream_dequeue)
{
	pwtest_add(stream_dequeue_without_io_fast, PWTEST_ARG_DAEMON);
	pwtest_add(stream_dequeue_without_io_reliable, PWTEST_ARG_DAEMON);

	return PWTEST_PASS;
}
