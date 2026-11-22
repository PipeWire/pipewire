/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Viorel Cernateanu */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pwtest.h"

#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include <pipewire/pipewire.h>

#define WIDTH		320
#define HEIGHT		240
#define N_BUFFERS	8
#define PERIOD_MS	40
#define PROGRESS_TIMEOUT_MS	5000
#define STALL_TIMEOUT_MS	2000
#define MAX_SEQ		1024
#define N_BUFFERS_MAX	64

SPA_STATIC_ASSERT(PROGRESS_TIMEOUT_MS / PERIOD_MS < MAX_SEQ);

struct data;

struct consumer {
	struct data *data;
	struct pw_stream *stream;
	struct spa_hook listener;
	enum pw_stream_state state;
	uint32_t new_frames;
	uint32_t repeats;
	uint32_t invalid;
	uint8_t seen[MAX_SEQ];
};

struct port {
	uint32_t id;
	uint32_t node_id;
	bool output;
};

struct data {
	struct pw_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct pw_stream *producer;
	struct spa_hook producer_listener;
	enum pw_stream_state producer_state;
	uint32_t n_buffers;
	uint32_t uses[N_BUFFERS_MAX];
	uint32_t seq;
	uint32_t produced;
	uint32_t reused;
	uint8_t in_reused[MAX_SEQ];	/* published in a reused buffer after the initial pool */

	struct consumer consumers[2];

	struct port ports[16];
	uint32_t n_ports;

	struct pw_proxy *out_port;
	struct spa_hook out_port_listener;
	char output_mode[32];

	struct pw_proxy *links[2];
	struct spa_hook link_listeners[2];
	enum pw_link_state link_states[2];
};

static void producer_process(void *userdata)
{
	struct data *d = userdata;
	struct pw_buffer *b;
	struct spa_data *sd;

	uint32_t index;
	bool reuse;

	if ((b = pw_stream_dequeue_buffer(d->producer)) == NULL)
		return;

	index = (uint32_t)(uintptr_t)b->user_data;
	sd = &b->buffer->datas[0];
	if (index >= N_BUFFERS_MAX || sd->data == NULL || sd->maxsize < sizeof(d->seq)) {
		pw_stream_queue_buffer(d->producer, b);
		return;
	}
	reuse = d->uses[index] > 0;

	memcpy(sd->data, &d->seq, sizeof(d->seq));
	sd->chunk->offset = 0;
	sd->chunk->size = SPA_MIN(sd->maxsize, (uint32_t)(WIDTH * HEIGHT * 2));
	sd->chunk->stride = WIDTH * 2;

	/* a frame only counts once it is queued */
	if (pw_stream_queue_buffer(d->producer, b) < 0)
		return;

	d->uses[index]++;
	if (reuse) {
		d->reused++;
		if (d->seq >= d->n_buffers && d->seq < MAX_SEQ)
			d->in_reused[d->seq] = 1;
	}
	d->seq++;
	d->produced++;
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

static void producer_add_buffer(void *userdata, struct pw_buffer *buffer)
{
	struct data *d = userdata;
	buffer->user_data = (void *)(uintptr_t)d->n_buffers++;
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
	.add_buffer = producer_add_buffer,
	.process = producer_process,
};

static void consumer_process(void *userdata)
{
	struct consumer *c = userdata;
	struct pw_buffer *b;

	while ((b = pw_stream_dequeue_buffer(c->stream)) != NULL) {
		struct spa_data *sd = &b->buffer->datas[0];
		uint32_t seq;

		if (sd->data != NULL && sd->chunk->size >= sizeof(seq)) {
			memcpy(&seq, SPA_PTROFF(sd->data, sd->chunk->offset, void), sizeof(seq));
			/* only a sequence number that the producer has written counts */
			if (seq >= c->data->seq || seq >= MAX_SEQ)
				c->invalid++;
			else if (c->seen[seq]++)
				c->repeats++;
			else
				c->new_frames++;
		}
		pw_stream_queue_buffer(c->stream, b);
	}
}

static void consumer_state_changed(void *userdata, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct consumer *c = userdata;
	c->state = state;
}

static const struct pw_stream_events consumer_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = consumer_state_changed,
	.process = consumer_process,
};

static void port_info(void *userdata, const struct pw_port_info *info)
{
	struct data *d = userdata;
	const char *str;

	if (info->props && (str = spa_dict_lookup(info->props, PW_KEY_PORT_OUTPUT_MODE)) != NULL)
		snprintf(d->output_mode, sizeof(d->output_mode), "%s", str);
}

static const struct pw_port_events port_events = {
	PW_VERSION_PORT_EVENTS,
	.info = port_info,
};

static void link_info_0(void *userdata, const struct pw_link_info *info)
{
	struct data *d = userdata;
	d->link_states[0] = info->state;
}

static void link_info_1(void *userdata, const struct pw_link_info *info)
{
	struct data *d = userdata;
	d->link_states[1] = info->state;
}

static const struct pw_link_events link_events[2] = {
	{ PW_VERSION_LINK_EVENTS, .info = link_info_0, },
	{ PW_VERSION_LINK_EVENTS, .info = link_info_1, },
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
		d->consumers[0].state == PW_STREAM_STATE_PAUSED &&
		d->consumers[1].state == PW_STREAM_STATE_PAUSED &&
		find_output_port(d, pw_stream_get_node_id(d->producer)) != NULL;
}

static bool streaming(struct data *d)
{
	return d->producer_state == PW_STREAM_STATE_STREAMING &&
		d->consumers[0].state == PW_STREAM_STATE_STREAMING &&
		d->consumers[1].state == PW_STREAM_STATE_STREAMING &&
		d->link_states[0] == PW_LINK_STATE_ACTIVE &&
		d->link_states[1] == PW_LINK_STATE_ACTIVE &&
		d->output_mode[0] != '\0';
}

static bool iterate_until(struct data *d, bool (*cond)(struct data *d), uint64_t timeout_ms)
{
	uint64_t end = now_ms() + timeout_ms;

	while (!cond(d) && now_ms() < end)
		pw_loop_iterate(d->loop, 10);
	return cond(d);
}

/* any frame published in a reused buffer, once the initial pool is used up,
 * that both consumers received */
static bool reuse_seen(struct data *d)
{
	for (uint32_t seq = d->n_buffers; seq < d->seq && seq < MAX_SEQ; seq++)
		if (d->in_reused[seq] && d->consumers[0].seen[seq] && d->consumers[1].seen[seq])
			return true;
	return false;
}

static const struct spa_pod *build_format(struct spa_pod_builder *b)
{
	return spa_format_video_raw_build(b, SPA_PARAM_EnumFormat,
			&SPA_VIDEO_INFO_RAW_INIT(
				.format = SPA_VIDEO_FORMAT_YUY2,
				.size = SPA_RECTANGLE(WIDTH, HEIGHT),
				.framerate = SPA_FRACTION(25, 1)));
}

/* A refcounted output port hands buffers back to the producer with
 * port_reuse_buffer. These must become available to the producer again. */
PWTEST(stream_output_reuse_refcounted)
{
	static struct data d;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];
	const struct port *out;
	uint32_t src_id, last_produced;
	uint64_t start, next, last_progress;

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

	d.producer = pw_stream_new(d.core, "reuse-src",
			pw_properties_new(
				PW_KEY_MEDIA_TYPE, "Video",
				PW_KEY_MEDIA_CATEGORY, "Capture",
				PW_KEY_MEDIA_CLASS, "Video/Source",
				PW_KEY_NODE_OUTPUT_MODE, "refcounted",
				PW_KEY_NODE_AUTOCONNECT, "false",
				NULL));
	pwtest_ptr_notnull(d.producer);
	pw_stream_add_listener(d.producer, &d.producer_listener, &producer_events, &d);
	params[0] = build_format(&b);
	pwtest_neg_errno_ok(pw_stream_connect(d.producer, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS, params, 1));

	for (int i = 0; i < 2; i++) {
		struct consumer *c = &d.consumers[i];

		c->data = &d;
		c->stream = pw_stream_new(d.core, "reuse-sink",
				pw_properties_new(
					PW_KEY_MEDIA_TYPE, "Video",
					PW_KEY_MEDIA_CATEGORY, "Capture",
					PW_KEY_NODE_AUTOCONNECT, "false",
					NULL));
		pwtest_ptr_notnull(c->stream);
		pw_stream_add_listener(c->stream, &c->listener, &consumer_events, c);
		b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		params[0] = build_format(&b);
		pwtest_neg_errno_ok(pw_stream_connect(c->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
				PW_STREAM_FLAG_MAP_BUFFERS, params, 1));
	}

	pwtest_bool_true(iterate_until(&d, streams_ready, 5000));

	src_id = pw_stream_get_node_id(d.producer);
	out = find_output_port(&d, src_id);
	d.out_port = pw_registry_bind(d.registry, out->id, PW_TYPE_INTERFACE_Port,
			PW_VERSION_PORT, 0);
	pw_proxy_add_object_listener(d.out_port, &d.out_port_listener, &port_events, &d);

	for (int i = 0; i < 2; i++) {
		struct pw_properties *props = pw_properties_new(NULL, NULL);

		pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", src_id);
		pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", out->id);
		pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u",
				pw_stream_get_node_id(d.consumers[i].stream));
		d.links[i] = pw_core_create_object(d.core, "link-factory",
				PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0);
		pwtest_ptr_notnull(d.links[i]);
		pw_proxy_add_object_listener(d.links[i], &d.link_listeners[i], &link_events[i], &d);
		pw_properties_free(props);
	}

	pwtest_bool_true(iterate_until(&d, streaming, 5000));
	pwtest_str_eq(d.output_mode, "refcounted");
	pwtest_int_gt(d.n_buffers, 0u);

	/* drive the graph from the producer, one cycle every PERIOD_MS, until
	 * both consumers got a frame that was published in a reused buffer */
	start = last_progress = next = now_ms();
	last_produced = d.produced;
	while (!reuse_seen(&d)) {
		uint64_t t = now_ms();

		if (t >= start + PROGRESS_TIMEOUT_MS || t >= last_progress + STALL_TIMEOUT_MS)
			break;
		if (t >= next) {
			pw_stream_trigger_process(d.producer);
			next += PERIOD_MS;
		}
		pw_loop_iterate(d.loop, 5);
		if (d.produced != last_produced) {
			last_produced = d.produced;
			last_progress = now_ms();
		}
	}

	fprintf(stderr, "buffers:%u produced:%u reused:%u consumer0 new:%u repeats:%u invalid:%u consumer1 new:%u repeats:%u invalid:%u\n",
			d.n_buffers, d.produced, d.reused,
			d.consumers[0].new_frames, d.consumers[0].repeats, d.consumers[0].invalid,
			d.consumers[1].new_frames, d.consumers[1].repeats, d.consumers[1].invalid);

	/* the central check: a buffer the consumers returned was handed back to
	 * the producer, filled again and delivered to both consumers */
	if (!reuse_seen(&d))
		pwtest_fail_with_msg("no frame in a reused buffer reached both consumers: "
				"%u frames published with %u buffers, %u in reused buffers",
				d.produced, d.n_buffers, d.reused);
	for (int i = 0; i < 2; i++) {
		pwtest_int_eq(d.consumers[i].repeats, 0u);
		pwtest_int_eq(d.consumers[i].invalid, 0u);
	}

	for (int i = 0; i < 2; i++) {
		pw_proxy_destroy(d.links[i]);
		pw_stream_destroy(d.consumers[i].stream);
	}
	pw_proxy_destroy(d.out_port);
	pw_stream_destroy(d.producer);
	pw_proxy_destroy((struct pw_proxy *)d.registry);
	pw_loop_leave(d.loop);
	pw_core_disconnect(d.core);
	pw_context_destroy(d.context);
	pw_loop_destroy(d.loop);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST_SUITE(stream_reuse)
{
	pwtest_add(stream_output_reuse_refcounted, PWTEST_ARG_DAEMON);

	return PWTEST_PASS;
}
