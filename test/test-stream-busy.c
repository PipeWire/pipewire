/* PipeWire */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pwtest.h"

#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/atomic.h>

#include <pipewire/pipewire.h>

#define WIDTH		320
#define HEIGHT		240
#define N_BUFFERS	4
#define N_BUFFERS_MAX	16
#define PERIOD_MS	40
#define TIMEOUT_MS	5000
#define SETTLE_MS	100

enum mode {
	RETURN_THEN_QUEUE,
	RETURN_THEN_FLUSH,
	FLUSH,
};

struct port {
	uint32_t id;
	uint32_t node_id;
	bool output;
};

struct data {
	enum mode mode;

	struct pw_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct spa_hook registry_listener;
	struct port ports[16];
	uint32_t n_ports;

	struct pw_stream *producer;
	struct spa_hook producer_listener;
	enum pw_stream_state producer_state;

	struct pw_stream *consumer;
	struct spa_hook consumer_listener;
	enum pw_stream_state consumer_state;
	struct pw_buffer *buffers[N_BUFFERS_MAX];
	uint32_t n_buffers;

	struct pw_proxy *link;
	struct spa_hook link_listener;
	enum pw_link_state link_state;

	/* the busy count of the checked input buffer after each step */
	struct pw_buffer *checked;
	uint32_t busy[4];
	uint32_t n_busy;
	int res_return;
	int res_queue;
	int res_flush;
	bool same_buffer;
	bool done;
};

static struct spa_meta_busy *find_busy(struct pw_buffer *b)
{
	return spa_buffer_find_meta_data(b->buffer, SPA_META_Busy, sizeof(struct spa_meta_busy));
}

static void record(struct data *d, struct pw_buffer *b)
{
	struct spa_meta_busy *m = find_busy(b);

	if (d->n_busy < SPA_N_ELEMENTS(d->busy))
		d->busy[d->n_busy++] = m ? SPA_ATOMIC_LOAD(m->count) : UINT32_MAX;
}

static void producer_process(void *userdata)
{
	struct data *d = userdata;
	struct pw_buffer *b;
	struct spa_data *sd;

	if ((b = pw_stream_dequeue_buffer(d->producer)) == NULL)
		return;

	sd = &b->buffer->datas[0];
	if (sd->data != NULL) {
		memset(sd->data, 0, SPA_MIN(sd->maxsize, 64u));
		sd->chunk->offset = 0;
		sd->chunk->size = SPA_MIN(sd->maxsize, (uint32_t)(WIDTH * HEIGHT * 2));
		sd->chunk->stride = WIDTH * 2;
	}
	pw_stream_queue_buffer(d->producer, b);
}

static void producer_param_changed(void *userdata, uint32_t id, const struct spa_pod *param)
{
	struct data *d = userdata;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];

	if (param == NULL || id != SPA_PARAM_Format)
		return;

	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(N_BUFFERS),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(WIDTH * HEIGHT * 2),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(WIDTH * 2),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
			(1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)));
	pw_stream_update_params(d->producer, params, 1);
}

static void producer_state_changed(void *userdata, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct data *d = userdata;
	d->producer_state = state;
}

static const struct pw_stream_events producer_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = producer_state_changed,
	.param_changed = producer_param_changed,
	.process = producer_process,
};

/* the received buffer that waits in the stream is the only busy one */
static struct pw_buffer *pending_buffer(struct data *d)
{
	struct pw_buffer *pending = NULL;

	for (uint32_t i = 0; i < d->n_buffers; i++) {
		struct spa_meta_busy *m = find_busy(d->buffers[i]);
		if (m == NULL || SPA_ATOMIC_LOAD(m->count) == 0)
			continue;
		if (pending != NULL)
			return NULL;
		pending = d->buffers[i];
	}
	return pending;
}

static void consumer_process(void *userdata)
{
	struct data *d = userdata;
	struct pw_buffer *b;

	if (d->done)
		return;

	d->same_buffer = true;
	if (d->mode == FLUSH) {
		if ((b = pending_buffer(d)) == NULL)
			return;
		record(d, b);
		d->res_flush = pw_stream_flush(d->consumer, false);
		record(d, b);
	} else {
		if ((b = pw_stream_dequeue_buffer(d->consumer)) == NULL)
			return;
		record(d, b);
		d->res_return = pw_stream_return_buffer(d->consumer, b);
		record(d, b);
		if (d->res_return < 0) {
			/* nothing more to check, the result is rejected below */
		} else if (d->mode == RETURN_THEN_QUEUE) {
			d->same_buffer = pw_stream_dequeue_buffer(d->consumer) == b;
			if (d->same_buffer) {
				record(d, b);
				d->res_queue = pw_stream_queue_buffer(d->consumer, b);
				record(d, b);
			}
		} else {
			d->res_flush = pw_stream_flush(d->consumer, false);
			record(d, b);
		}
	}
	d->checked = b;
	d->done = true;
}

static void consumer_add_buffer(void *userdata, struct pw_buffer *buffer)
{
	struct data *d = userdata;
	if (d->n_buffers < N_BUFFERS_MAX)
		d->buffers[d->n_buffers++] = buffer;
}

static void consumer_state_changed(void *userdata, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct data *d = userdata;
	d->consumer_state = state;
}

static const struct pw_stream_events consumer_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = consumer_state_changed,
	.add_buffer = consumer_add_buffer,
	.process = consumer_process,
};

static void link_info(void *userdata, const struct pw_link_info *info)
{
	struct data *d = userdata;
	d->link_state = info->state;
}

static const struct pw_link_events link_events = {
	PW_VERSION_LINK_EVENTS,
	.info = link_info,
};

static void registry_global(void *userdata, uint32_t id, uint32_t permissions,
		const char *type, uint32_t version, const struct spa_dict *props)
{
	struct data *d = userdata;
	const char *node_id, *direction;

	if (!spa_streq(type, PW_TYPE_INTERFACE_Port) || props == NULL ||
	    d->n_ports == SPA_N_ELEMENTS(d->ports))
		return;
	if ((node_id = spa_dict_lookup(props, PW_KEY_NODE_ID)) == NULL ||
	    (direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION)) == NULL)
		return;

	d->ports[d->n_ports++] = (struct port) {
		.id = id,
		.node_id = atoi(node_id),
		.output = spa_streq(direction, "out"),
	};
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_global,
};

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const struct port *find_output_port(struct data *d, uint32_t node_id)
{
	for (uint32_t i = 0; i < d->n_ports; i++)
		if (d->ports[i].node_id == node_id && d->ports[i].output)
			return &d->ports[i];
	return NULL;
}

static bool streams_ready(struct data *d)
{
	return d->producer_state == PW_STREAM_STATE_PAUSED &&
		d->consumer_state == PW_STREAM_STATE_PAUSED &&
		find_output_port(d, pw_stream_get_node_id(d->producer)) != NULL;
}

static bool streaming(struct data *d)
{
	return d->producer_state == PW_STREAM_STATE_STREAMING &&
		d->consumer_state == PW_STREAM_STATE_STREAMING &&
		d->link_state == PW_LINK_STATE_ACTIVE;
}

static bool iterate_until(struct data *d, bool (*cond)(struct data *d), uint64_t timeout_ms)
{
	uint64_t end = now_ms() + timeout_ms;

	while (!cond(d) && now_ms() < end)
		pw_loop_iterate(d->loop, 10);
	return cond(d);
}

static const struct spa_pod *build_format(struct spa_pod_builder *b)
{
	return spa_format_video_raw_build(b, SPA_PARAM_EnumFormat,
			&SPA_VIDEO_INFO_RAW_INIT(
				.format = SPA_VIDEO_FORMAT_YUY2,
				.size = SPA_RECTANGLE(WIDTH, HEIGHT),
				.framerate = SPA_FRACTION(25, 1)));
}

static struct pw_properties *video_props(void)
{
	return pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Video",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_NODE_AUTOCONNECT, "false",
			NULL);
}

/* A received input buffer is busy until the stream gives it back to the
 * peer. Check the busy count of one buffer after each step. */
static void check_busy(enum mode mode, const char *name, const uint32_t *expected,
		uint32_t n_expected)
{
	struct data d = { .mode = mode };
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	const struct spa_pod *params[1];
	const struct port *out;
	struct pw_properties *props;
	uint64_t start, next;

	pw_init(0, NULL);
	d.loop = pw_loop_new(NULL);
	pwtest_ptr_notnull(d.loop);
	d.context = pw_context_new(d.loop, NULL, 0);
	pwtest_ptr_notnull(d.context);
	d.core = pw_context_connect(d.context, NULL, 0);
	pwtest_ptr_notnull(d.core);

	pw_loop_enter(d.loop);

	d.registry = pw_core_get_registry(d.core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(d.registry, &d.registry_listener, &registry_events, &d);

	props = video_props();
	pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Video/Source");
	d.producer = pw_stream_new(d.core, "busy-src", props);
	pwtest_ptr_notnull(d.producer);
	pw_stream_add_listener(d.producer, &d.producer_listener, &producer_events, &d);
	b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	params[0] = build_format(&b);
	pwtest_neg_errno_ok(pw_stream_connect(d.producer, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS, params, 1));

	d.consumer = pw_stream_new(d.core, "busy-sink", video_props());
	pwtest_ptr_notnull(d.consumer);
	pw_stream_add_listener(d.consumer, &d.consumer_listener, &consumer_events, &d);
	b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	params[0] = build_format(&b);
	pwtest_neg_errno_ok(pw_stream_connect(d.consumer, PW_DIRECTION_INPUT, PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS, params, 1));

	pwtest_bool_true(iterate_until(&d, streams_ready, TIMEOUT_MS));

	out = find_output_port(&d, pw_stream_get_node_id(d.producer));
	props = pw_properties_new(NULL, NULL);
	pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", pw_stream_get_node_id(d.producer));
	pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", out->id);
	pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", pw_stream_get_node_id(d.consumer));
	d.link = pw_core_create_object(d.core, "link-factory",
			PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0);
	pwtest_ptr_notnull(d.link);
	pw_proxy_add_object_listener(d.link, &d.link_listener, &link_events, &d);
	pw_properties_free(props);

	pwtest_bool_true(iterate_until(&d, streaming, TIMEOUT_MS));

	/* drive the graph until the consumer has done its steps, then keep
	 * going a little so that a late release would be seen */
	start = next = now_ms();
	while (!d.done && now_ms() < start + TIMEOUT_MS) {
		if (now_ms() >= next) {
			pw_stream_trigger_process(d.producer);
			next += PERIOD_MS;
		}
		pw_loop_iterate(d.loop, 5);
	}
	pwtest_bool_true(d.done);
	start = now_ms();
	while (now_ms() < start + SETTLE_MS)
		pw_loop_iterate(d.loop, 5);
	pwtest_ptr_notnull(find_busy(d.checked));
	if (mode != RETURN_THEN_QUEUE)
		record(&d, d.checked);

	fprintf(stderr, "case:%s steps:%u busy:", name, d.n_busy);
	for (uint32_t i = 0; i < d.n_busy; i++)
		fprintf(stderr, "%s%u", i ? "," : "", d.busy[i]);
	fprintf(stderr, " return:%d queue:%d flush:%d same:%d\n",
			d.res_return, d.res_queue, d.res_flush, d.same_buffer);

	/* return_buffer() returns the fill level of the queue on success */
	pwtest_int_ge(d.res_return, 0);
	pwtest_bool_true(d.same_buffer);
	pwtest_int_ge(d.res_queue, 0);
	pwtest_int_eq(d.res_flush, 0);
	pwtest_int_eq(d.n_busy, n_expected);
	for (uint32_t i = 0; i < n_expected; i++)
		pwtest_int_eq(d.busy[i], expected[i]);

	pw_proxy_destroy(d.link);
	pw_stream_destroy(d.consumer);
	pw_stream_destroy(d.producer);
	pw_proxy_destroy((struct pw_proxy *)d.registry);
	pw_loop_leave(d.loop);
	pw_core_disconnect(d.core);
	pw_context_destroy(d.context);
	pw_loop_destroy(d.loop);
	pw_deinit();
}

/* dequeue, return, dequeue again, queue */
PWTEST(stream_busy_input_return_then_queue)
{
	static const uint32_t expected[] = { 1, 1, 1, 0 };
	check_busy(RETURN_THEN_QUEUE, "return-then-queue", expected, SPA_N_ELEMENTS(expected));
	return PWTEST_PASS;
}

/* dequeue, return, flush, then a little later */
PWTEST(stream_busy_input_return_then_flush)
{
	static const uint32_t expected[] = { 1, 1, 0, 0 };
	check_busy(RETURN_THEN_FLUSH, "return-then-flush", expected, SPA_N_ELEMENTS(expected));
	return PWTEST_PASS;
}

/* flush a received buffer that was not dequeued, then a little later */
PWTEST(stream_busy_input_flush)
{
	static const uint32_t expected[] = { 1, 0, 0 };
	check_busy(FLUSH, "flush", expected, SPA_N_ELEMENTS(expected));
	return PWTEST_PASS;
}

PWTEST_SUITE(stream_busy)
{
	pwtest_add(stream_busy_input_return_then_queue, PWTEST_ARG_DAEMON);
	pwtest_add(stream_busy_input_return_then_flush, PWTEST_ARG_DAEMON);
	pwtest_add(stream_busy_input_flush, PWTEST_ARG_DAEMON);

	return PWTEST_PASS;
}
