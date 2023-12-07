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
				"[ loopback.count=<number of loopbacks> ] "
				"[ node.latency=<latency as fraction> ] "
				"[ node.description=<description of the nodes> ] "
				"[ audio.rate=<sample rate> ] "
				"[ audio.channels=<number of channels> ] "
				"[ audio.position=<channel map> ] "
				"[ capture1.props=<properties> ] "
				"[ playback1.props=<properties> ] "
				"[ capture2.props=<properties> ] "
				"[ playback2.props=<properties> ] " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

#include <stdlib.h>
#include <getopt.h>

#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

#include <pipewire/pipewire.h>

#define MAX_LOOBACKS 5

#define IMPL_FOREACH_LOOPBACK(impl, l, idx) \
	for (idx = 0, l = &impl->loopbacks[0]; idx < impl->n_loopbacks; l = &impl->loopbacks[++idx])

struct loopback;

typedef void process_fn(struct loopback*, const int32_t*, int32_t*, uint32_t*);
typedef void skip_fn(struct loopback*);

static process_fn noop_process;
static process_fn attenuate_process;

struct device_fns {
	const char *name;
	process_fn *process;
	skip_fn *skip;
};

static struct device_fns fns[] = {
	{
		.name = "fpga",
		.process = noop_process,
		.skip = NULL,
	},
	{
		.name = "usb",
		.process = attenuate_process,
		.skip = NULL,
	},
};

static struct device_fns* lookup_device_fns(const char *name) {
	for (unsigned int i = 0; i < SPA_N_ELEMENTS(fns); i++)
		if (spa_streq(name, fns[i].name))
			return &fns[i];

	return NULL;
}

struct loopback {
	struct impl *impl;

	struct pw_stream *playback;
	struct pw_properties *playback_props;
	struct spa_hook playback_listener;
	struct spa_audio_info_raw playback_info;

	struct pw_stream *capture;
	struct pw_properties *capture_props;
	struct spa_hook capture_listener;
	struct spa_audio_info_raw capture_info;

	bool needs_capture;
	bool capture_ready;
	bool capture_streaming;

	process_fn *process;
	skip_fn *skip;
};

struct impl {
	struct pw_context *context;

	struct pw_impl_module *module;

	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	unsigned int n_loopbacks;
	struct loopback loopbacks[MAX_LOOBACKS];

	unsigned int do_disconnect:1;
};

static void trigger_playback(struct impl *impl)
{
	struct loopback *l;
	unsigned long i;

	// Make sure all streaming capture streams are ready
	IMPL_FOREACH_LOOPBACK(impl, l, i) {
		if (l->needs_capture && l->capture_streaming && !l->capture_ready)
			return;
	}

	IMPL_FOREACH_LOOPBACK(impl, l, i) {
		if (!l->needs_capture || l->capture_streaming)
			pw_stream_trigger_process(l->playback);
		else if (l->skip)
			l->skip(l);
		l->capture_ready = false;
	}
}

static void capture_destroy(void *d)
{
	struct loopback *l = d;
	spa_hook_remove(&l->capture_listener);
	l->capture = NULL;
}

static void capture_process(void *d)
{
	struct loopback *l = d;
	pw_log_trace("capture trigger");

	l->capture_ready = true;
	trigger_playback(l->impl);
}

static void noop_process(struct loopback *l, const int32_t *src, int32_t *dst, uint32_t *size)
{
	// We have access to `impl` inside `l`, so we could look up any of the
	// other streams and forward data there if needed
	for (uint32_t i = 0; i < *size / sizeof(uint32_t); i++) {
		dst[i] = src[i];
	}
}

static void attenuate_process(struct loopback *l, const int32_t *src, int32_t *dst, uint32_t *size)
{
	for (uint32_t i = 0; i < *size / sizeof(uint32_t); i++) {
		dst[i] = src[i] / 2;
	}
}

static void playback_process(void *d) {
	struct loopback *l = d;
	struct pw_buffer *in = NULL, *out = NULL;

	pw_log_trace("playback trigger");

	in = NULL;
	while (true) {
		struct pw_buffer *t;
		if ((t = pw_stream_dequeue_buffer(l->capture)) == NULL)
			break;
		if (in) {
			pw_stream_queue_buffer(l->capture, in);
			pw_log_warn("dropping capture buffers: %m");
		}
		in = t;
	}
	if (in == NULL)
		pw_log_debug("out of capture buffers: %m");

	if ((out = pw_stream_dequeue_buffer(l->playback)) == NULL)
		pw_log_warn("out of playback buffers: %m");

	if (in != NULL && out != NULL) {
		uint32_t outsize;
		int32_t stride;
		struct spa_data *d;
		const int32_t *src;
		int32_t *dst;
		uint32_t offs, size;

		d = &in->buffer->datas[0];
		offs = SPA_MIN(d->chunk->offset, d->maxsize);
		size = SPA_MIN(d->chunk->size, d->maxsize - offs);

		src = SPA_PTROFF(d->data, offs, void);
		outsize = size;
		stride = d->chunk->stride;

		d = &out->buffer->datas[0];
		outsize = SPA_MIN(outsize, d->maxsize);
		dst = d->data;

		// Do the actual processing
		l->process(l, src, dst, &size);

		d->chunk->offset = 0;
		d->chunk->size = outsize;
		d->chunk->stride = stride;
	} else if (!l->needs_capture && out != NULL) {
		struct spa_data *d;
		int32_t *dst;
		uint32_t size;

		d = &out->buffer->datas[0];
		size = d->maxsize;
		dst = d->data;

		// Do the actual processing
		l->process(l, NULL, dst, &size);

		d->chunk->offset = 0;
		d->chunk->size = size;
	}

	if (in != NULL)
		pw_stream_queue_buffer(l->capture, in);
	if (out != NULL)
		pw_stream_queue_buffer(l->playback, out);
}

static void stream_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct loopback *l = data;

	// FIXME: We have to check stream states here, because the callback is
	// used across all streams and we don't know which one this was called
	// on.
	if (pw_stream_get_state(l->capture, NULL) == PW_STREAM_STATE_STREAMING)
		l->capture_streaming = true;
	else
		l->capture_streaming = false;
	pw_log_debug("stream state [%p]: %u", l, l->capture_streaming);

	switch (state) {
	case PW_STREAM_STATE_ERROR:
		pw_log_info("module %p: error: %s", l->impl, error);
		break;
	default:
		break;
	}
}

static const struct pw_stream_events capture_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = capture_destroy,
	.process = capture_process,
	.state_changed = stream_state_changed,
};

static void playback_destroy(void *d)
{
	struct loopback *l = d;
	spa_hook_remove(&l->playback_listener);
	l->playback = NULL;
}

static const struct pw_stream_events playback_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = playback_destroy,
	.process = playback_process,
	.state_changed = stream_state_changed,
};

static int setup_streams(struct impl *impl)
{
	int res;
	struct loopback *l;
	unsigned long idx;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b;

	IMPL_FOREACH_LOOPBACK(impl, l, idx) {
		struct device_fns *fns;
		const char *str;

		str = pw_properties_get(l->playback_props, "loopback.device");
		fns = lookup_device_fns(str);

		if (!fns) {
			pw_log_error("Could not look up functions for device %s", str);
			return -EINVAL;
		}

		l->impl = impl;
		l->process = fns->process;
		l->skip = fns->skip;

		l->capture = pw_stream_new(impl->core,
				"loopback capture", l->capture_props);
		l->capture_props = NULL;
		if (l->capture == NULL)
			return -errno;

		pw_stream_add_listener(l->capture, &l->capture_listener,
				&capture_stream_events, l);

		l->playback = pw_stream_new(impl->core,
				"loopback playback", l->playback_props);
		l->playback_props = NULL;
		if (l->playback == NULL)
			return -errno;

		pw_stream_add_listener(l->playback, &l->playback_listener,
				&playback_stream_events, l);

		/* connect playback first to activate it before capture triggers it */
		n_params = 0;
		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
				&l->playback_info);
		if ((res = pw_stream_connect(l->playback,
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
				&l->capture_info);
		if ((res = pw_stream_connect(l->capture,
						PW_DIRECTION_INPUT,
						PW_ID_ANY,
						PW_STREAM_FLAG_AUTOCONNECT |
						PW_STREAM_FLAG_MAP_BUFFERS |
						PW_STREAM_FLAG_ASYNC |
						PW_STREAM_FLAG_RT_PROCESS,
						params, n_params)) < 0)
			return res;
	}

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
	struct loopback *l;
	unsigned long i;

	/* deactivate streams before destroying any of them */
	IMPL_FOREACH_LOOPBACK(impl, l, i) {
		if (l->capture)
			pw_stream_set_active(l->capture, false);
		if (l->playback)
			pw_stream_set_active(l->playback, false);
	}

	IMPL_FOREACH_LOOPBACK(impl, l, i) {
		if (l->capture)
			pw_stream_destroy(l->capture);
		if (l->playback)
			pw_stream_destroy(l->playback);

		pw_properties_free(l->capture_props);
		pw_properties_free(l->playback_props);
	}

	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

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
	struct loopback *l;
	unsigned long i;
	const char *str;

	if ((str = pw_properties_get(props, key)) != NULL) {
		IMPL_FOREACH_LOOPBACK(impl, l, i) {
			if (pw_properties_get(l->capture_props, key) == NULL)
				pw_properties_set(l->capture_props, key, str);
			if (pw_properties_get(l->playback_props, key) == NULL)
				pw_properties_set(l->playback_props, key, str);
		}
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
	char tmp_str[256];
	struct loopback *l;
	unsigned long i;
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

	// Defaulting to 2 to allow older configs to work as-is
	impl->n_loopbacks = pw_properties_get_uint32(props, "loopback.count", 2);

	IMPL_FOREACH_LOOPBACK(impl, l, i) {
		l->capture_props = pw_properties_new(NULL, NULL);
		l->playback_props = pw_properties_new(NULL, NULL);
		if (l->capture_props == NULL || l->playback_props == NULL) {
			res = -errno;
			pw_log_error( "can't create properties: %m");
			goto error;
		}
	}

	impl->module = module;
	impl->context = context;

	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_GROUP, "loopback-%u-%u", pid, id);
	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");

	IMPL_FOREACH_LOOPBACK(impl, l, i) {
		snprintf(tmp_str, sizeof(tmp_str), "capture%lu.props", i + 1);
		if ((str = pw_properties_get(props, tmp_str)) != NULL)
			pw_properties_update_string(l->capture_props, str, strlen(str));
		snprintf(tmp_str, sizeof(tmp_str), "playback%lu.props", i + 1);
		if ((str = pw_properties_get(props, tmp_str)) != NULL)
			pw_properties_update_string(l->playback_props, str, strlen(str));
	}

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

	IMPL_FOREACH_LOOPBACK(impl, l, i) {
		if (pw_properties_get(l->capture_props, PW_KEY_NODE_NAME) == NULL)
			pw_properties_setf(l->capture_props, PW_KEY_NODE_NAME,
					"input%lu.%s", i + 1, str);
		if (pw_properties_get(l->playback_props, PW_KEY_NODE_NAME) == NULL)
			pw_properties_setf(l->playback_props, PW_KEY_NODE_NAME,
					"output%lu.%s", i + 1, str);
	}

	IMPL_FOREACH_LOOPBACK(impl, l, i) {
		l->needs_capture = pw_properties_get_bool(l->playback_props, "loopback.needs-capture", true);
		if (l->needs_capture) {
			if (pw_properties_get(l->capture_props, PW_KEY_NODE_LINK_GROUP) == NULL)
				pw_properties_setf(l->capture_props, PW_KEY_NODE_LINK_GROUP, "loopback-%u-%u-%lu", pid, id, i + 1);
			if (pw_properties_get(l->playback_props, PW_KEY_NODE_LINK_GROUP) == NULL)
				pw_properties_setf(l->playback_props, PW_KEY_NODE_LINK_GROUP, "loopback-%u-%u-%lu", pid, id, i + 1);
		}

		if (pw_properties_get(l->capture_props, PW_KEY_NODE_DESCRIPTION) == NULL)
			pw_properties_set(l->capture_props, PW_KEY_NODE_DESCRIPTION, str);
		if (pw_properties_get(l->playback_props, PW_KEY_NODE_DESCRIPTION) == NULL)
			pw_properties_set(l->playback_props, PW_KEY_NODE_DESCRIPTION, str);

		if (pw_properties_get(l->capture_props, PW_KEY_MEDIA_NAME) == NULL)
			pw_properties_setf(l->capture_props, PW_KEY_MEDIA_NAME, "%s input %lu",
					pw_properties_get(l->capture_props, PW_KEY_NODE_DESCRIPTION), i + 1);
		if (pw_properties_get(l->playback_props, PW_KEY_MEDIA_NAME) == NULL)
			pw_properties_setf(l->playback_props, PW_KEY_MEDIA_NAME, "%s output %lu",
					pw_properties_get(l->playback_props, PW_KEY_NODE_DESCRIPTION), i + 1);

		parse_audio_info(l->capture_props, &l->capture_info);
		parse_audio_info(l->playback_props, &l->playback_info);
	}

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
