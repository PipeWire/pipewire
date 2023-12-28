/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
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

/** \page page_module_loopback Loopback
 *
 * The loopback module passes the output of a capture stream unmodified to a playback stream.
 * It can be used to construct a link between a source and sink but also to
 * create new virtual sinks or sources or to remap channel between streams.
 *
 * Because both ends of the loopback are built with streams, the session manager can
 * manage the configuration and connection with the sinks and sources.
 *
 * ## Module Name
 *
 * `libpipewire-module-loopback`
 *
 * ## Module Options
 *
 * - `node.description`: a human readable name for the loopback streams
 * - `target.delay.sec`: delay in seconds as float (Since 0.3.60)
 * - `capture.props = {}`: properties to be passed to the input stream
 * - `playback.props = {}`: properties to be passed to the output stream
 *
 * ## General options
 *
 * Options with well-known behavior. Most options can be added to the global
 * configuration or the individual streams:
 *
 * - \ref PW_KEY_REMOTE_NAME
 * - \ref PW_KEY_AUDIO_RATE
 * - \ref PW_KEY_AUDIO_CHANNELS
 * - \ref SPA_KEY_AUDIO_POSITION
 * - \ref PW_KEY_MEDIA_NAME
 * - \ref PW_KEY_NODE_LATENCY
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_NODE_GROUP
 * - \ref PW_KEY_NODE_LINK_GROUP
 * - \ref PW_KEY_NODE_VIRTUAL
 * - \ref PW_KEY_NODE_NAME: See notes below. If not specified, defaults to
 *   	'loopback-<pid>-<module-id>'.
 *
 * Stream only properties:
 *
 * - \ref PW_KEY_MEDIA_CLASS
 * - \ref PW_KEY_NODE_NAME:  if not given per stream, the global node.name will be
 *         prefixed with 'input.' and 'output.' to generate a capture and playback
 *         stream node.name respectively.
 *
 * ## Example configuration of a virtual sink
 *
 * This Virtual sink routes stereo input to the rear channels of a 7.1 sink.
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-loopback
 *     args = {
 *         node.description = "CM106 Stereo Pair 2"
 *         #target.delay.sec = 1.5
 *         capture.props = {
 *             node.name = "CM106_stereo_pair_2"
 *             media.class = "Audio/Sink"
 *             audio.position = [ FL FR ]
 *         }
 *         playback.props = {
 *             node.name = "playback.CM106_stereo_pair_2"
 *             audio.position = [ RL RR ]
 *             target.object = "alsa_output.usb-0d8c_USB_Sound_Device-00.analog-surround-71"
 *             node.dont-reconnect = true
 *             stream.dont-remix = true
 *             node.passive = true
 *         }
 *     }
 * }
 * ]
 *\endcode
 *
 * ## Example configuration of a virtual source
 *
 * This Virtual source routes the front-left channel of a multi-channel input to a mono channel.
 * This is useful for splitting up multi-channel inputs from USB audio interfaces that are not yet fully supported by alsa.
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-loopback
 *     args = {
 *       node.description = "Scarlett Focusrite Line 1"
 *       capture.props = {
 *           audio.position = [ FL ]
 *           stream.dont-remix = true
 *           node.target = "alsa_input.usb-Focusrite_Scarlett_Solo_USB_Y7ZD17C24495BC-00.analog-stereo"
 *           node.passive = true
 *       }
 *       playback.props = {
 *           node.name = "SF_mono_in_1"
 *           media.class = "Audio/Source"
 *           audio.position = [ MONO ]
 *       }
 *     }
 * }
 * ]
 *\endcode
 *
 * ## See also
 *
 * `pw-loopback` : a tool that loads the loopback module with given parameters.
 */

#define NAME "loopback"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create loopback streams" },
	{ PW_KEY_MODULE_USAGE, " ( remote.name=<remote> ) "
				"( node.latency=<latency as fraction> ) "
				"( node.description=<description of the nodes> ) "
				"( audio.rate=<sample rate> ) "
				"( audio.channels=<number of channels> ) "
				"( audio.position=<channel map> ) "
				"( target.delay.sec=<delay as seconds in float> ) "
				"( capture.props=<properties> ) "
				"( playback.props=<properties> ) " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

#define DEFAULT_RATE	48000

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

	struct pw_properties *capture_props;
	struct pw_stream *capture;
	struct spa_hook capture_listener;
	struct spa_audio_info_raw capture_info;

	struct pw_properties *playback_props;
	struct pw_stream *playback;
	struct spa_hook playback_listener;
	struct spa_audio_info_raw playback_info;

	unsigned int do_disconnect:1;
	unsigned int recalc_delay:1;

	struct spa_io_position *position;
	struct spa_audio_info_raw info;
	uint32_t rate;
	float target_delay;
	struct spa_ringbuffer buffer;
	uint8_t *buffer_data;
	uint32_t buffer_size;
};

static void capture_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->capture_listener);
	impl->capture = NULL;
}

static void recalculate_delay(struct impl *impl)
{
	uint32_t target = impl->rate * impl->target_delay, cdelay, pdelay;
	uint32_t delay, w;
	struct pw_time pwt;

	pw_stream_get_time_n(impl->playback, &pwt, sizeof(pwt));
	pdelay = pwt.delay;
	pw_stream_get_time_n(impl->capture, &pwt, sizeof(pwt));
	cdelay = pwt.delay;

	delay = target - SPA_MIN(target, pdelay + cdelay);
	delay = SPA_MIN(delay, impl->buffer_size / 4);

	spa_ringbuffer_get_write_index(&impl->buffer, &w);
	spa_ringbuffer_read_update(&impl->buffer, w - (delay * 4));

	pw_log_info("target:%d c:%d + p:%d + delay:%d = (%d)",
			target, cdelay, pdelay, delay,
			cdelay + pdelay + delay);
}

static void capture_process(void *d)
{
	struct impl *impl = d;
	pw_stream_trigger_process(impl->playback);
}

static void playback_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *in, *out;
	uint32_t i;

	if (impl->recalc_delay) {
		recalculate_delay(impl);
		impl->recalc_delay = false;
	}

	in = NULL;
	while (true) {
		struct pw_buffer *t;
		if ((t = pw_stream_dequeue_buffer(impl->capture)) == NULL)
			break;
		if (in)
			pw_stream_queue_buffer(impl->capture, in);
		in = t;
	}
	if (in == NULL)
		pw_log_debug("%p: out of capture buffers: %m", impl);

	if ((out = pw_stream_dequeue_buffer(impl->playback)) == NULL)
		pw_log_debug("%p: out of playback buffers: %m", impl);

	if (in != NULL && out != NULL) {
		uint32_t outsize = UINT32_MAX;
		int32_t stride = 0;
		struct spa_data *d;
		const void *src[in->buffer->n_datas];
		uint32_t r, w, buffer_size;

		for (i = 0; i < in->buffer->n_datas; i++) {
			uint32_t offs, size;

			d = &in->buffer->datas[i];
			offs = SPA_MIN(d->chunk->offset, d->maxsize);
			size = SPA_MIN(d->chunk->size, d->maxsize - offs);

			src[i] = SPA_PTROFF(d->data, offs, void);
			outsize = SPA_MIN(outsize, size);
			stride = SPA_MAX(stride, d->chunk->stride);
		}
		if (impl->buffer_size > 0) {
			buffer_size = impl->buffer_size;
			spa_ringbuffer_get_write_index(&impl->buffer, &w);
			for (i = 0; i < in->buffer->n_datas; i++) {
				void *buffer_data = &impl->buffer_data[i * buffer_size];
				spa_ringbuffer_write_data(&impl->buffer,
						buffer_data, buffer_size,
						w % buffer_size, src[i], outsize);
				src[i] = buffer_data;
			}
			w += outsize;
			spa_ringbuffer_write_update(&impl->buffer, w);
			spa_ringbuffer_get_read_index(&impl->buffer, &r);
		} else {
			r = 0;
			buffer_size = outsize;
		}
		for (i = 0; i < out->buffer->n_datas; i++) {
			d = &out->buffer->datas[i];

			outsize = SPA_MIN(outsize, d->maxsize);

			if (i < in->buffer->n_datas)
				spa_ringbuffer_read_data(&impl->buffer,
						src[i], buffer_size,
						r % buffer_size,
						d->data, outsize);
			else
				memset(d->data, 0, outsize);

			d->chunk->offset = 0;
			d->chunk->size = outsize;
			d->chunk->stride = stride;
		}
		if (impl->buffer_size > 0) {
			r += outsize;
			spa_ringbuffer_read_update(&impl->buffer, r);
		}
	}

	if (in != NULL)
		pw_stream_queue_buffer(impl->capture, in);
	if (out != NULL)
		pw_stream_queue_buffer(impl->playback, out);
}

static void param_latency_changed(struct impl *impl, const struct spa_pod *param,
		struct pw_stream *other)
{
	struct spa_latency_info latency;
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	const struct spa_pod *params[1];

	if (param == NULL || spa_latency_parse(param, &latency) < 0)
		return;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[0] = spa_latency_build(&b, SPA_PARAM_Latency, &latency);
	pw_stream_update_params(other, params, 1);

	impl->recalc_delay = true;
}

static void param_tag_changed(struct impl *impl, const struct spa_pod *param,
		struct pw_stream *other)
{
	const struct spa_pod *params[1] = { param };
	if (param == NULL)
		return;
	pw_stream_update_params(other, params, 1);
}

static void recalculate_buffer(struct impl *impl)
{
	if (impl->target_delay > 0.0f) {
		uint32_t delay = impl->rate * impl->target_delay;
		void *data;

		impl->buffer_size = (delay + (1u<<15)) * 4;
		data = realloc(impl->buffer_data, impl->buffer_size * impl->info.channels);
		if (data == NULL) {
			pw_log_warn("can't allocate delay buffer, delay disabled: %m");
			impl->buffer_size = 0;
			free(impl->buffer_data);
		}
		impl->buffer_data = data;
		spa_ringbuffer_init(&impl->buffer);
	} else {
		impl->buffer_size = 0;
		free(impl->buffer_data);
		impl->buffer_data = NULL;
	}
	pw_log_info("configured delay:%f buffer:%d", impl->target_delay, impl->buffer_size);
	impl->recalc_delay = true;
}

static void stream_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = data;
	switch (state) {
	case PW_STREAM_STATE_PAUSED:
		pw_stream_flush(impl->playback, false);
		pw_stream_flush(impl->capture, false);
		impl->recalc_delay = true;
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		pw_log_info("module %p: unconnected", impl);
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_STREAM_STATE_ERROR:
		pw_log_info("module %p: error: %s", impl, error);
		break;
	case PW_STREAM_STATE_STREAMING:
	{
		uint32_t target = impl->info.rate;
		if (target == 0)
			target = impl->position ?
				impl->position->clock.target_rate.denom : DEFAULT_RATE;
		if (impl->rate != target) {
			impl->rate = target;
			recalculate_buffer(impl);
		}
		break;
	}
	default:
		break;
	}
}

static void capture_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct impl *impl = data;

	switch (id) {
	case SPA_PARAM_Format:
	{
		struct spa_audio_info_raw info;
		spa_zero(info);
		if (param != NULL) {
			if (spa_format_audio_raw_parse(param, &info) < 0 ||
			    info.channels == 0 ||
			    info.channels > SPA_AUDIO_MAX_CHANNELS)
				return;
		}
		impl->rate = 0;
		impl->info = info;
		break;
	}
	case SPA_PARAM_Latency:
		param_latency_changed(impl, param, impl->playback);
		break;
	case SPA_PARAM_Tag:
		param_tag_changed(impl, param, impl->playback);
		break;
	}
}

static void io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
	struct impl *impl = data;
	switch (id) {
	case SPA_IO_Position:
		impl->position = area;
		break;
	default:
		break;
	}
}

static const struct pw_stream_events in_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = capture_destroy,
	.process = capture_process,
	.state_changed = stream_state_changed,
	.param_changed = capture_param_changed,
	.io_changed = io_changed,
};

static void playback_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->playback_listener);
	impl->playback = NULL;
}

static void playback_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct impl *impl = data;

	switch (id) {
	case SPA_PARAM_Latency:
		param_latency_changed(impl, param, impl->capture);
		break;
	case SPA_PARAM_Tag:
		param_tag_changed(impl, param, impl->capture);
		break;
	}
}
static const struct pw_stream_events out_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = playback_destroy,
	.process = playback_process,
	.state_changed = stream_state_changed,
	.param_changed = playback_param_changed,
	.io_changed = io_changed,
};

static int setup_streams(struct impl *impl)
{
	int res;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b;

	impl->capture = pw_stream_new(impl->core,
			"loopback capture", impl->capture_props);
	impl->capture_props = NULL;
	if (impl->capture == NULL)
		return -errno;

	pw_stream_add_listener(impl->capture,
			&impl->capture_listener,
			&in_stream_events, impl);

	impl->playback = pw_stream_new(impl->core,
			"loopback playback", impl->playback_props);
	impl->playback_props = NULL;
	if (impl->playback == NULL)
		return -errno;

	pw_stream_add_listener(impl->playback,
			&impl->playback_listener,
			&out_stream_events, impl);

	/* connect playback first to activate it before capture triggers it */
	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&impl->playback_info);
	if ((res = pw_stream_connect(impl->playback,
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
			&impl->capture_info);
	if ((res = pw_stream_connect(impl->capture,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS |
			PW_STREAM_FLAG_ASYNC,
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
	if (impl->capture)
		pw_stream_set_active(impl->capture, false);
	if (impl->playback)
		pw_stream_set_active(impl->playback, false);

	if (impl->capture)
		pw_stream_destroy(impl->capture);
	if (impl->playback)
		pw_stream_destroy(impl->playback);

	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	pw_properties_free(impl->capture_props);
	pw_properties_free(impl->playback_props);
	free(impl->buffer_data);
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
			.format = SPA_AUDIO_FORMAT_F32P);
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
		if (pw_properties_get(impl->capture_props, key) == NULL)
			pw_properties_set(impl->capture_props, key, str);
		if (pw_properties_get(impl->playback_props, key) == NULL)
			pw_properties_set(impl->playback_props, key, str);
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

	impl->capture_props = pw_properties_new(NULL, NULL);
	impl->playback_props = pw_properties_new(NULL, NULL);
	if (impl->capture_props == NULL || impl->playback_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->module = module;
	impl->context = context;

	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_GROUP, "loopback-%u-%u", pid, id);
	if (pw_properties_get(props, PW_KEY_NODE_LINK_GROUP) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_LINK_GROUP, "loopback-%u-%u", pid, id);
	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, "resample.prefill") == NULL)
		pw_properties_set(props, "resample.prefill", "true");

	if ((str = pw_properties_get(props, "capture.props")) != NULL)
		pw_properties_update_string(impl->capture_props, str, strlen(str));
	if ((str = pw_properties_get(props, "playback.props")) != NULL)
		pw_properties_update_string(impl->playback_props, str, strlen(str));

	if ((str = pw_properties_get(props, "target.delay.sec")) != NULL)
		spa_atof(str, &impl->target_delay);
	if (impl->target_delay > 0.0f &&
	    pw_properties_get(props, PW_KEY_NODE_LATENCY) == NULL)
		/* a source and sink (USB) usually have a 1.5 quantum delay, so we use
		 * a 2 times smaller quantum to compensate */
		pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u",
				(unsigned)(impl->target_delay * 48000 / 3), 48000);

	copy_props(impl, props, PW_KEY_AUDIO_RATE);
	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_DESCRIPTION);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LINK_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LATENCY);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, PW_KEY_MEDIA_NAME);
	copy_props(impl, props, "resample.prefill");

	if ((str = pw_properties_get(props, PW_KEY_NODE_NAME)) == NULL) {
		pw_properties_setf(props, PW_KEY_NODE_NAME,
				"loopback-%u-%u", pid, id);
		str = pw_properties_get(props, PW_KEY_NODE_NAME);
	}
	if (pw_properties_get(impl->capture_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(impl->capture_props, PW_KEY_NODE_NAME,
				"input.%s", str);
	if (pw_properties_get(impl->playback_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(impl->playback_props, PW_KEY_NODE_NAME,
				"output.%s", str);
	if (pw_properties_get(impl->capture_props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(impl->capture_props, PW_KEY_NODE_DESCRIPTION, str);
	if (pw_properties_get(impl->playback_props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_set(impl->playback_props, PW_KEY_NODE_DESCRIPTION, str);

	parse_audio_info(impl->capture_props, &impl->capture_info);
	parse_audio_info(impl->playback_props, &impl->playback_info);

	if (!impl->capture_info.rate && !impl->playback_info.rate) {
		if (pw_properties_get(impl->playback_props, "resample.disable") == NULL)
			pw_properties_set(impl->playback_props, "resample.disable", "true");
		if (pw_properties_get(impl->capture_props, "resample.disable") == NULL)
			pw_properties_set(impl->capture_props, "resample.disable", "true");
	} else if (impl->capture_info.rate && !impl->playback_info.rate)
		impl->playback_info.rate = impl->capture_info.rate;
	else if (impl->playback_info.rate && !impl->capture_info.rate)
		impl->capture_info.rate = !impl->playback_info.rate;
	else if (impl->capture_info.rate != impl->playback_info.rate) {
		pw_log_warn("Both capture and playback rate are set, but"
			" they are different. Using the highest of two. This behaviour"
			" is deprecated, please use equal rates in the module config");
		impl->playback_info.rate = impl->capture_info.rate =
			SPA_MAX(impl->playback_info.rate, impl->capture_info.rate);
	}

	if (pw_properties_get(impl->capture_props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(impl->capture_props, PW_KEY_MEDIA_NAME, "%s input",
				pw_properties_get(impl->capture_props, PW_KEY_NODE_DESCRIPTION));
	if (pw_properties_get(impl->playback_props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(impl->playback_props, PW_KEY_MEDIA_NAME, "%s output",
				pw_properties_get(impl->playback_props, PW_KEY_NODE_DESCRIPTION));

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
