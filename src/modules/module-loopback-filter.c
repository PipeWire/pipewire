/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans, 2023 Asymptotic Inc. */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/utils/ringbuffer.h>
#include <spa/param/latency-utils.h>
#include <spa/debug/types.h>

#include <pipewire/impl.h>
#include <pipewire/extensions/profiler.h>

#define NAME "loopback-filter"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Arun Raghavan <arun@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create loopback streams" },
	{ PW_KEY_MODULE_USAGE, " [ remote.name=<remote> ] "
				"[ node.latency=<latency as fraction> ] "
				"[ node.description=<description of the nodes> ] "
				"[ audio.rate=<sample rate> ] "
				"[ audio.channels=<number of channels> ] "
				"[ audio.position=<channel map> ] "
				"[ capture1.props=<properties> ] "
				"[ capture2.props=<properties> ] "
				"[ playback1.props=<properties> ] "
				"[ playback2.props=<properties> ] " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>

#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

#include <pipewire/pipewire.h>

struct impl {
	struct pw_context *context;

	struct pw_impl_module *module;

	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct pw_properties *capture1_props;
	struct pw_stream *capture1;
	struct spa_hook capture1_listener;
	struct spa_audio_info_raw capture1_info;

	struct pw_properties *capture2_props;
	struct pw_stream *capture2;
	struct spa_hook capture2_listener;
	struct spa_audio_info_raw capture2_info;

	struct pw_properties *playback1_props;
	struct pw_stream *playback1;
	struct spa_hook playback1_listener;
	struct spa_audio_info_raw playback1_info;

	struct pw_properties *playback2_props;
	struct pw_stream *playback2;
	struct spa_hook playback2_listener;
	struct spa_audio_info_raw playback2_info;

	unsigned int do_disconnect:1;

	struct spa_ringbuffer buffer;
	uint8_t *buffer_data;
	uint32_t buffer_size;

	bool capture1_streaming;
	bool capture2_streaming;
	bool capture1_ready;
	bool capture2_ready;
};

static void trigger_playback(struct impl *impl)
{
	if ((impl->capture1_streaming && !impl->capture1_ready) || (impl->capture2_streaming && !impl->capture2_ready))
		return;

	if (impl->capture2_streaming) {
		// loopback1 and loopback2 are running, trigger the second which cascades to the first
		pw_stream_trigger_process(impl->playback2);
	} else {
		// Only loopback1 is running, so trigger just that
		pw_stream_trigger_process(impl->playback1);
	}

	impl->capture1_ready = false;
	impl->capture2_ready = false;
}

static void capture1_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->capture1_listener);
	impl->capture1 = NULL;
}

static void capture1_process(void *d)
{
	struct impl *impl = d;
	pw_log_trace("capture1 trigger");

	impl->capture1_ready = true;
	trigger_playback(impl);
}

static void capture2_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->capture2_listener);
	impl->capture2 = NULL;
}

static void capture2_process(void *d)
{
	struct impl *impl = d;
	pw_log_trace("capture2 trigger");

	impl->capture2_ready = true;
	trigger_playback(impl);
}

static void playback1_process(void *d) {
	struct impl *impl = d;
	struct pw_buffer *in1 = NULL, *out1 = NULL;

	pw_log_trace("playback1 trigger");

	in1 = NULL;
	while (true) {
		struct pw_buffer *t;
		if ((t = pw_stream_dequeue_buffer(impl->capture1)) == NULL)
			break;
		if (in1) {
			pw_stream_queue_buffer(impl->capture1, in1);
			pw_log_warn("dropping capture1 buffers: %m");
		}
		in1 = t;
	}
	if (in1 == NULL)
		pw_log_debug("out of capture1 buffers: %m");

	if ((out1 = pw_stream_dequeue_buffer(impl->playback1)) == NULL)
		pw_log_warn("out of playback1 buffers: %m");

	if (in1 != NULL && out1 != NULL) {
		uint32_t outsize;
		int32_t stride;
		struct spa_data *d;
		const int32_t *src;
		int32_t *dst;
		uint32_t offs, size;

		d = &in1->buffer->datas[0];
		offs = SPA_MIN(d->chunk->offset, d->maxsize);
		size = SPA_MIN(d->chunk->size, d->maxsize - offs);

		src = SPA_PTROFF(d->data, offs, void);
		outsize = size;
		stride = d->chunk->stride;

		d = &out1->buffer->datas[0];
		outsize = SPA_MIN(outsize, d->maxsize);
		dst = d->data;

		for (uint32_t i = 0; i < outsize / sizeof(uint32_t); i++) {
			if (i % 24 == 1)
				dst[i] = src[i] / 2;
			else
				dst[i] = src[i];
		}

		d->chunk->offset = 0;
		d->chunk->size = outsize;
		d->chunk->stride = stride;
	}

	if (in1 != NULL)
		pw_stream_queue_buffer(impl->capture1, in1);
	if (out1 != NULL)
		pw_stream_queue_buffer(impl->playback1, out1);
}

static void playback2_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *in2 = NULL, *out2 = NULL;

	pw_log_trace("playback2 process");

	in2 = NULL;
	while (true) {
		struct pw_buffer *t;
		if ((t = pw_stream_dequeue_buffer(impl->capture2)) == NULL)
			break;
		if (in2) {
			pw_stream_queue_buffer(impl->capture2, in2);
			pw_log_warn("dropping capture2 buffers: %m");
		}
		in2 = t;
	}

	// Can be NULL if USB isn't connected
	out2 = pw_stream_dequeue_buffer(impl->playback2);

	if (in2 != NULL && out2 != NULL) {
		uint32_t outsize;
		int32_t stride;
		struct spa_data *d;
		const int32_t *src;
		int32_t *dst;
		uint32_t offs, size;

		d = &in2->buffer->datas[0];
		offs = SPA_MIN(d->chunk->offset, d->maxsize);
		size = SPA_MIN(d->chunk->size, d->maxsize - offs);

		src = SPA_PTROFF(d->data, offs, void);
		outsize = size;
		stride = d->chunk->stride;

		d = &out2->buffer->datas[0];
		outsize = SPA_MIN(outsize, d->maxsize);
		dst = d->data;

		for (uint32_t i = 0; i < outsize / sizeof(uint32_t); i++) {
			if (i % 8 == 0)
				dst[i] = src[i] / 2;
			else
				dst[i] = src[i];
		}

		d->chunk->offset = 0;
		d->chunk->size = outsize;
		d->chunk->stride = stride;
	}

	if (in2 != NULL)
		pw_stream_queue_buffer(impl->capture2, in2);
	if (out2 != NULL)
		pw_stream_queue_buffer(impl->playback2, out2);

	// We've pushed data on both streams, trigger the other stream to
	// complete the graph processing iteration
	pw_stream_trigger_process(impl->playback1);
}

static void stream_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = data;

	// FIXME: We have to check stream states here, because the callback is
	// used across all streams and we don't know which one this was called
	// on.
	if (pw_stream_get_state(impl->capture1, NULL) == PW_STREAM_STATE_STREAMING)
		impl->capture1_streaming = true;
	else
		impl->capture1_streaming = false;
	if (pw_stream_get_state(impl->capture2, NULL) == PW_STREAM_STATE_STREAMING)
		impl->capture2_streaming = true;
	else
		impl->capture2_streaming = false;
	pw_log_debug("Stream states: capture1 %u, capture2 %u", impl->capture1_streaming, impl->capture2_streaming);

	switch (state) {
	case PW_STREAM_STATE_UNCONNECTED:
		pw_log_info("module %p: unconnected", impl);
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_STREAM_STATE_ERROR:
		pw_log_info("module %p: error: %s", impl, error);
		break;
	default:
		break;
	}
}

static const struct pw_stream_events in1_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = capture1_destroy,
	.process = capture1_process,
	.state_changed = stream_state_changed,
};

static const struct pw_stream_events in2_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = capture2_destroy,
	.process = capture2_process,
	.state_changed = stream_state_changed,
};

static void playback1_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->playback1_listener);
	impl->playback1 = NULL;
}

static const struct pw_stream_events out1_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = playback1_destroy,
	.process = playback1_process,
	.state_changed = stream_state_changed,
};

static void playback2_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->playback2_listener);
	impl->playback2 = NULL;
}

static const struct pw_stream_events out2_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = playback2_destroy,
	.process = playback2_process,
	.state_changed = stream_state_changed,
};

static int setup_streams(struct impl *impl)
{
	int res;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b;

	impl->capture1 = pw_stream_new(impl->core,
			"loopback capture1", impl->capture1_props);
	impl->capture1_props = NULL;
	if (impl->capture1 == NULL)
		return -errno;

	pw_stream_add_listener(impl->capture1,
			&impl->capture1_listener,
			&in1_stream_events, impl);

	impl->capture2 = pw_stream_new(impl->core,
			"loopback capture2", impl->capture2_props);
	impl->capture2_props = NULL;
	if (impl->capture2 == NULL)
		return -errno;

	pw_stream_add_listener(impl->capture2,
			&impl->capture2_listener,
			&in2_stream_events, impl);

	impl->playback1 = pw_stream_new(impl->core,
			"loopback playback1", impl->playback1_props);
	impl->playback1_props = NULL;
	if (impl->playback1 == NULL)
		return -errno;

	pw_stream_add_listener(impl->playback1,
			&impl->playback1_listener,
			&out1_stream_events, impl);

	impl->playback2 = pw_stream_new(impl->core,
			"loopback playback2", impl->playback2_props);
	impl->playback2_props = NULL;
	if (impl->playback2 == NULL)
		return -errno;

	pw_stream_add_listener(impl->playback2,
			&impl->playback2_listener,
			&out2_stream_events, impl);

	/* connect playback first to activate it before capture triggers it */
	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&impl->playback1_info);
	if ((res = pw_stream_connect(impl->playback1,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS |
			PW_STREAM_FLAG_TRIGGER,
			params, n_params)) < 0)
		return res;

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&impl->playback2_info);
	if ((res = pw_stream_connect(impl->playback2,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS |
			PW_STREAM_FLAG_TRIGGER,
			params, n_params)) < 0)
		return res;

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&impl->capture1_info);
	if ((res = pw_stream_connect(impl->capture1,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_ASYNC |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&impl->capture2_info);
	if ((res = pw_stream_connect(impl->capture2,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_ASYNC |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	return 0;
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = data;

	if (res == -ENOENT) {
		pw_log_info("message id:%u seq:%d res:%d (%s): %s",
				id, seq, res, spa_strerror(res), message);
	} else {
		pw_log_warn("error id:%u seq:%d res:%d (%s): %s",
				id, seq, res, spa_strerror(res), message);
	}

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static void core_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->core_listener);
	impl->core = NULL;
	pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void impl_destroy(struct impl *impl)
{
	/* deactivate both streams before destroying any of them */
	if (impl->capture1)
		pw_stream_set_active(impl->capture1, false);
	if (impl->capture2)
		pw_stream_set_active(impl->capture2, false);
	if (impl->playback1)
		pw_stream_set_active(impl->playback1, false);
	if (impl->playback2)
		pw_stream_set_active(impl->playback1, false);

	if (impl->capture1)
		pw_stream_destroy(impl->capture1);
	if (impl->capture2)
		pw_stream_destroy(impl->capture2);
	if (impl->playback1)
		pw_stream_destroy(impl->playback1);
	if (impl->playback2)
		pw_stream_destroy(impl->playback2);

	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	pw_properties_free(impl->capture1_props);
	pw_properties_free(impl->capture2_props);
	pw_properties_free(impl->playback1_props);
	pw_properties_free(impl->playback2_props);
	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static uint32_t channel_from_name(const char *name)
{
	int i;
	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (spa_streq(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)))
			return spa_type_audio_channel[i].type;
	}
	return SPA_AUDIO_CHANNEL_UNKNOWN;
}

static void parse_position(struct spa_audio_info_raw *info, const char *val, size_t len)
{
	struct spa_json it[2];
	char v[256];

	spa_json_init(&it[0], val, len);
        if (spa_json_enter_array(&it[0], &it[1]) <= 0)
                spa_json_init(&it[1], val, len);

	info->channels = 0;
	while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
	    info->channels < SPA_AUDIO_MAX_CHANNELS) {
		info->position[info->channels++] = channel_from_name(v);
	}
}

static void parse_audio_info(struct pw_properties *props, struct spa_audio_info_raw *info)
{
	const char *str;

	*info = SPA_AUDIO_INFO_RAW_INIT(
			.format = SPA_AUDIO_FORMAT_S32_LE);
	info->rate = pw_properties_get_int32(props, PW_KEY_AUDIO_RATE, 0);
	info->channels = pw_properties_get_uint32(props, PW_KEY_AUDIO_CHANNELS, 0);
	info->channels = SPA_MIN(info->channels, SPA_AUDIO_MAX_CHANNELS);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(info, str, strlen(str));
}

static void copy_props(struct impl *impl, struct pw_properties *props, const char *key)
{
	const char *str;
	if ((str = pw_properties_get(props, key)) != NULL) {
		if (pw_properties_get(impl->capture1_props, key) == NULL)
			pw_properties_set(impl->capture1_props, key, str);
		if (pw_properties_get(impl->capture2_props, key) == NULL)
			pw_properties_set(impl->capture2_props, key, str);
		if (pw_properties_get(impl->playback1_props, key) == NULL)
			pw_properties_set(impl->playback1_props, key, str);
		if (pw_properties_get(impl->playback2_props, key) == NULL)
			pw_properties_set(impl->playback2_props, key, str);
	}
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props;
	struct impl *impl;
	uint32_t id = pw_global_get_id(pw_impl_module_get_global(module));
	uint32_t pid = getpid();
	const char *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args)
		props = pw_properties_new_string(args);
	else
		props = pw_properties_new(NULL, NULL);
	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->capture1_props = pw_properties_new(NULL, NULL);
	impl->capture2_props = pw_properties_new(NULL, NULL);
	impl->playback1_props = pw_properties_new(NULL, NULL);
	impl->playback2_props = pw_properties_new(NULL, NULL);
	if (impl->capture1_props == NULL
			|| impl->capture2_props == NULL
			|| impl->playback1_props == NULL
			|| impl->playback2_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->module = module;
	impl->context = context;

	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_GROUP, "loopback-%u-%u", pid, id);
	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");

	if ((str = pw_properties_get(props, "capture1.props")) != NULL)
		pw_properties_update_string(impl->capture1_props, str, strlen(str));
	if ((str = pw_properties_get(props, "capture2.props")) != NULL)
		pw_properties_update_string(impl->capture2_props, str, strlen(str));
	if ((str = pw_properties_get(props, "playback1.props")) != NULL)
		pw_properties_update_string(impl->playback1_props, str, strlen(str));
	if ((str = pw_properties_get(props, "playback2.props")) != NULL)
		pw_properties_update_string(impl->playback2_props, str, strlen(str));


	copy_props(impl, props, PW_KEY_AUDIO_RATE);
	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_DESCRIPTION);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LATENCY);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, PW_KEY_MEDIA_NAME);

	if ((str = pw_properties_get(props, PW_KEY_NODE_NAME)) == NULL) {
		pw_properties_setf(props, PW_KEY_NODE_NAME,
				"loopback-%u-%u", pid, id);
		str = pw_properties_get(props, PW_KEY_NODE_NAME);
	}
	if (pw_properties_get(impl->capture1_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(impl->capture1_props, PW_KEY_NODE_NAME,
				"input1.%s", str);
	if (pw_properties_get(impl->capture2_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(impl->capture2_props, PW_KEY_NODE_NAME,
				"input2.%s", str);
	if (pw_properties_get(impl->playback1_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(impl->playback1_props, PW_KEY_NODE_NAME,
				"output1.%s", str);
	if (pw_properties_get(impl->playback2_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(impl->playback2_props, PW_KEY_NODE_NAME,
				"output2.%s", str);

	if (pw_properties_get(impl->capture1_props, PW_KEY_NODE_LINK_GROUP) == NULL)
		pw_properties_setf(impl->capture1_props, PW_KEY_NODE_LINK_GROUP, "loopback-%u-%u-1", pid, id);
	if (pw_properties_get(impl->capture2_props, PW_KEY_NODE_LINK_GROUP) == NULL)
		pw_properties_setf(impl->capture2_props, PW_KEY_NODE_LINK_GROUP, "loopback-%u-%u-2", pid, id);
	if (pw_properties_get(impl->playback1_props, PW_KEY_NODE_LINK_GROUP) == NULL)
		pw_properties_setf(impl->playback1_props, PW_KEY_NODE_LINK_GROUP, "loopback-%u-%u-1", pid, id);
	if (pw_properties_get(impl->playback2_props, PW_KEY_NODE_LINK_GROUP) == NULL)
		pw_properties_setf(impl->playback2_props, PW_KEY_NODE_LINK_GROUP, "loopback-%u-%u-2", pid, id);

	if (pw_properties_get(impl->capture1_props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(impl->capture1_props, PW_KEY_NODE_DESCRIPTION, str);
	if (pw_properties_get(impl->capture2_props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(impl->capture2_props, PW_KEY_NODE_DESCRIPTION, str);
	if (pw_properties_get(impl->playback1_props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(impl->playback1_props, PW_KEY_NODE_DESCRIPTION, str);
	if (pw_properties_get(impl->playback2_props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(impl->playback2_props, PW_KEY_NODE_DESCRIPTION, str);

	parse_audio_info(impl->capture1_props, &impl->capture1_info);
	parse_audio_info(impl->capture2_props, &impl->capture2_info);
	parse_audio_info(impl->playback1_props, &impl->playback1_info);
	parse_audio_info(impl->playback2_props, &impl->playback2_info);

	if (pw_properties_get(impl->capture1_props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(impl->capture1_props, PW_KEY_MEDIA_NAME, "%s input 1",
				pw_properties_get(impl->capture1_props, PW_KEY_NODE_DESCRIPTION));
	if (pw_properties_get(impl->capture2_props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(impl->capture2_props, PW_KEY_MEDIA_NAME, "%s input 2",
				pw_properties_get(impl->capture2_props, PW_KEY_NODE_DESCRIPTION));
	if (pw_properties_get(impl->playback1_props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(impl->playback1_props, PW_KEY_MEDIA_NAME, "%s output 1",
				pw_properties_get(impl->playback1_props, PW_KEY_NODE_DESCRIPTION));
	if (pw_properties_get(impl->playback2_props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(impl->playback2_props, PW_KEY_MEDIA_NAME, "%s output 2",
				pw_properties_get(impl->playback2_props, PW_KEY_NODE_DESCRIPTION));

	impl->core = pw_context_get_object(impl->context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		impl->core = pw_context_connect(impl->context,
				pw_properties_new(
					PW_KEY_REMOTE_NAME, str,
					NULL),
				0);
		impl->do_disconnect = true;
	}
	if (impl->core == NULL) {
		res = -errno;
		pw_log_error("can't connect: %m");
		goto error;
	}

	pw_properties_free(props);

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener,
			&core_proxy_events, impl);
	pw_core_add_listener(impl->core,
			&impl->core_listener,
			&core_events, impl);

	setup_streams(impl);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	pw_properties_free(props);
	impl_destroy(impl);
	return res;
}
