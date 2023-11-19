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
#include <stdlib.h>
#include <signal.h>
#include <limits.h>
#include <math.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/dll.h>
#include <spa/utils/ratelimit.h>
#include <spa/debug/types.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/raw.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>

#include <pulse/pulseaudio.h>
#include "module-protocol-pulse/defs.h"
#include "module-protocol-pulse/format.h"

/** \page page_module_pulse_tunnel Pulse Tunnel
 *
 * The pulse-tunnel module provides a source or sink that tunnels all audio to
 * a remote PulseAudio connection.
 *
 * It is usually used with the PulseAudio or module-protocol-pulse on the remote
 * end to accept the connection.
 *
 * This module is usually used together with module-zeroconf-discover that will
 * automatically load the tunnel with the right parameters based on zeroconf
 * information.
 *
 * ## Module Name
 *
 * `libpipewire-module-pulse-tunnel`
 *
 * ## Module Options
 *
 * - `tunnel.mode`: the desired tunnel to create, must be `source` or `sink`.
 *                  (Default `sink`)
 * - `pulse.server.address`: the address of the PulseAudio server to tunnel to.
 * - `pulse.latency`: the latency to end-to-end latency in milliseconds to
 *                    maintain (Default 200).
 * - `stream.props`: Extra properties for the local stream.
 *
 * ## General options
 *
 * Options with well-known behavior.
 *
 * - \ref PW_KEY_REMOTE_NAME
 * - \ref PW_KEY_AUDIO_FORMAT
 * - \ref PW_KEY_AUDIO_RATE
 * - \ref PW_KEY_AUDIO_CHANNELS
 * - \ref SPA_KEY_AUDIO_POSITION
 * - \ref PW_KEY_NODE_LATENCY
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_NODE_GROUP
 * - \ref PW_KEY_NODE_VIRTUAL
 * - \ref PW_KEY_MEDIA_CLASS
 * - \ref PW_KEY_TARGET_OBJECT to specify the remote node.name or serial.id to link to
 *
 * ## Example configuration of a virtual sink
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-pulse-tunnel
 *     args = {
 *         tunnel.mode = sink
 *         # Set the remote address to tunnel to
 *         pulse.server.address = "tcp:192.168.1.126"
 *         #pulse.latency = 200
 *         #audio.rate=<sample rate>
 *         #audio.channels=<number of channels>
 *         #audio.position=<channel map>
 *         #target.object=<remote target name>
 *         stream.props = {
 *             # extra sink properties
 *         }
 *     }
 * }
 * ]
 *\endcode
 */

#define NAME "pulse-tunnel"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define DEFAULT_FORMAT "S16"
#define DEFAULT_RATE 48000
#define DEFAULT_CHANNELS 2
#define DEFAULT_POSITION "[ FL FR ]"

#define MODULE_USAGE	"( remote.name=<remote> ] "				\
			"( node.latency=<latency as fraction> ] "		\
			"( node.name=<name of the nodes> ] "			\
			"( node.description=<description of the nodes> ] "	\
			"( node.target=<remote node target name or serial> ] "	\
			"( audio.format=<sample format> ] "			\
			"( audio.rate=<sample rate> ] "				\
			"( audio.channels=<number of channels> ] "		\
			"( audio.position=<channel map> ] "			\
			"pulse.server.address=<address> "			\
			"( pulse.latency=<latency in msec, default 200> ) "	\
			"( tunnel.mode=source|sink, default sink ) "				\
			"( stream.props=<properties> ) "


static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create a PulseAudio tunnel" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

#define RINGBUFFER_SIZE		(1u << 22)
#define RINGBUFFER_MASK		(RINGBUFFER_SIZE-1)

#define DEFAULT_LATENCY_MSEC	(200)

struct impl {
	struct pw_context *context;
	struct pw_loop *main_loop;

#define MODE_SINK	0
#define MODE_SOURCE	1
	uint32_t mode;
	struct pw_properties *props;

	struct pw_impl_module *module;

	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	uint32_t latency_msec;

	struct pw_properties *stream_props;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_audio_info_raw info;
	uint32_t frame_size;

	struct spa_ringbuffer ring;
	void *buffer;
	uint8_t empty[8192];

	bool mute;
	pa_cvolume volume;

	pa_threaded_mainloop *pa_mainloop;
	pa_context *pa_context;
	pa_stream *pa_stream;
	uint32_t pa_index;

	struct spa_ratelimit rate_limit;

	uint32_t target_latency;
	uint32_t current_latency;
	uint32_t target_buffer;
	struct spa_io_rate_match *rate_match;
	struct spa_dll dll;
	float max_error;
	unsigned resync:1;

	unsigned int do_disconnect:1;
};

static void cork_stream(struct impl *impl, bool cork)
{
	pa_operation *operation;

	pa_threaded_mainloop_lock(impl->pa_mainloop);

	pw_log_debug("corking: %d", cork);
	if (cork && impl->mode == MODE_SINK) {
		/* When the sink becomes suspended (which is the only case where we
		 * cork the stream), we don't want to keep any old data around, because
		 * the old data is most likely unrelated to the audio that will be
		 * played at the time when the sink starts running again. */
		if ((operation = pa_stream_flush(impl->pa_stream, NULL, NULL)))
			pa_operation_unref(operation);

		spa_ringbuffer_init(&impl->ring);
		memset(impl->buffer, 0, RINGBUFFER_SIZE);
	}
	if (!cork)
		impl->resync = true;

	if ((operation = pa_stream_cork(impl->pa_stream, cork, NULL, NULL)))
		pa_operation_unref(operation);

	pa_threaded_mainloop_unlock(impl->pa_mainloop);
}

static void stream_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->stream_listener);
	impl->stream = NULL;
}

static void stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = d;
	switch (state) {
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
		if (impl->module)
			pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_STREAM_STATE_PAUSED:
		cork_stream(impl, true);
		break;
	case PW_STREAM_STATE_STREAMING:
		cork_stream(impl, false);
		break;
	default:
		break;
	}
}

static void stream_param_changed(void *d, uint32_t id, const struct spa_pod *param)
{
	struct impl *impl = d;
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[1];
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	struct spa_pod_prop *prop;

	if (param == NULL || id != SPA_PARAM_Props)
		return;

	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_mute:
		{
			bool mute;
			if (spa_pod_get_bool(&prop->value, &mute) == 0) {
				pa_threaded_mainloop_lock(impl->pa_mainloop);
				if (impl->mode == MODE_SOURCE) {
					pa_context_set_source_output_mute(impl->pa_context,
							impl->pa_index, mute,
							NULL, impl);
				} else {
					pa_context_set_sink_input_mute(impl->pa_context,
							impl->pa_index, mute,
							NULL, impl);
				}
				pa_threaded_mainloop_unlock(impl->pa_mainloop);
                        }
			break;
		}
		case SPA_PROP_channelVolumes:
		{
			struct pa_cvolume volume;
			uint32_t n;
			float vols[SPA_AUDIO_MAX_CHANNELS];

			if ((n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					vols, SPA_AUDIO_MAX_CHANNELS)) > 0) {
				volume.channels = n;
				for (n = 0; n < volume.channels; n++)
					volume.values[n] = pa_sw_volume_from_linear(vols[n]);

				pa_threaded_mainloop_lock(impl->pa_mainloop);
				if (impl->mode == MODE_SOURCE) {
					pa_context_set_source_output_volume(impl->pa_context,
							impl->pa_index, &volume,
							NULL, impl);
				} else {
					pa_context_set_sink_input_volume(impl->pa_context,
							impl->pa_index, &volume,
							NULL, impl);
				}
				pa_threaded_mainloop_unlock(impl->pa_mainloop);
			}
			break;
		}
		case SPA_PROP_softVolumes:
		case SPA_PROP_softMute:
			break;
		default:
			spa_pod_builder_raw_padded(&b, prop, SPA_POD_PROP_SIZE(prop));
			break;
		}
	}
	param = spa_pod_builder_pop(&b, &f[0]);

	pw_stream_set_param(impl->stream, id, param);
}

static void update_rate(struct impl *impl, uint32_t filled)
{
	float error, corr;
	uint32_t current_latency;

	if (impl->rate_match == NULL)
		return;

	current_latency = impl->current_latency + filled;
	error = (float)impl->target_latency - (float)(current_latency);
	error = SPA_CLAMP(error, -impl->max_error, impl->max_error);

	corr = spa_dll_update(&impl->dll, error);
	pw_log_debug("error:%f corr:%f current:%u target:%u",
			error, corr,
			current_latency, impl->target_latency);

	SPA_FLAG_SET(impl->rate_match->flags, SPA_IO_RATE_MATCH_FLAG_ACTIVE);
	impl->rate_match->rate = 1.0f / corr;
}

static void playback_stream_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *buf;
	struct spa_data *bd;
	int32_t filled;
	uint32_t write_index, offs, size;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	bd = &buf->buffer->datas[0];
	offs = SPA_MIN(bd->chunk->offset, bd->maxsize);
	size = SPA_MIN(bd->chunk->size, bd->maxsize - offs);
	size = SPA_MIN(size, RINGBUFFER_SIZE);

	filled = spa_ringbuffer_get_write_index(&impl->ring, &write_index);

	if (filled < 0) {
		pw_log_warn("%p: underrun write:%u filled:%d",
				impl, write_index, filled);
	} else if ((uint32_t)filled + size > RINGBUFFER_SIZE) {
		pw_log_warn("%p: overrun write:%u filled:%d + size:%u > max:%u",
                                        impl, write_index, filled,
                                        size, RINGBUFFER_SIZE);
		impl->resync = true;
	} else {
		update_rate(impl, filled / impl->frame_size);
	}
	spa_ringbuffer_write_data(&impl->ring,
				impl->buffer, RINGBUFFER_SIZE,
				write_index & RINGBUFFER_MASK,
				SPA_PTROFF(bd->data, offs, void),
				size);
	write_index += size;
	spa_ringbuffer_write_update(&impl->ring, write_index);

	pw_stream_queue_buffer(impl->stream, buf);
}

static void capture_stream_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *buf;
	struct spa_data *bd;
	int32_t avail;
	uint32_t size, req, index;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	bd = &buf->buffer->datas[0];

	if ((req = buf->requested * impl->frame_size) == 0)
		req = 4096 * impl->frame_size;

	size = SPA_MIN(bd->maxsize, req);
	size = SPA_ROUND_DOWN(size, impl->frame_size);

	avail = spa_ringbuffer_get_read_index(&impl->ring, &index);
	if (avail < (int32_t)size)
		memset(bd->data, 0, size);
	if (avail > (int32_t)RINGBUFFER_SIZE) {
		index += avail - impl->target_buffer;
		avail = impl->target_buffer;
	}
	if (avail > 0) {
		avail = SPA_ROUND_DOWN(avail, impl->frame_size);
		update_rate(impl, avail / impl->frame_size);

		avail = SPA_MIN(size, (uint32_t)avail);
		spa_ringbuffer_read_data(&impl->ring,
				impl->buffer, RINGBUFFER_SIZE,
				index & RINGBUFFER_MASK,
				bd->data, avail);

		index += avail;
		spa_ringbuffer_read_update(&impl->ring, index);
	}
	bd->chunk->offset = 0;
	bd->chunk->size = size;
	bd->chunk->stride = impl->frame_size;

	pw_stream_queue_buffer(impl->stream, buf);
}

static void stream_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
	struct impl *impl = data;
	switch (id) {
	case SPA_IO_RateMatch:
		impl->rate_match = area;
		break;
	}
}

static const struct pw_stream_events playback_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.io_changed = stream_io_changed,
	.param_changed = stream_param_changed,
	.process = playback_stream_process
};

static const struct pw_stream_events capture_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.io_changed = stream_io_changed,
	.param_changed = stream_param_changed,
	.process = capture_stream_process
};

static int create_stream(struct impl *impl)
{
	int res;
	uint32_t n_params;
	const struct spa_pod *params[2];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	struct spa_latency_info latency;

	impl->stream = pw_stream_new(impl->core, "pulse", impl->stream_props);
	impl->stream_props = NULL;

	if (impl->stream == NULL)
		return -errno;

	if (impl->mode == MODE_SOURCE) {
		pw_stream_add_listener(impl->stream,
				&impl->stream_listener,
				&capture_stream_events, impl);
	} else {
		pw_stream_add_listener(impl->stream,
				&impl->stream_listener,
				&playback_stream_events, impl);
	}

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &impl->info);

	spa_zero(latency);
	latency.direction = impl->mode == MODE_SOURCE ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT;
	latency.min_ns = latency.max_ns = impl->latency_msec * SPA_NSEC_PER_MSEC;

	params[n_params++] = spa_latency_build(&b,
			SPA_PARAM_Latency, &latency);

	if ((res = pw_stream_connect(impl->stream,
			impl->mode == MODE_SOURCE ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	return 0;
}

static int
do_schedule_destroy(struct spa_loop *loop,
	bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	if (impl->module)
		pw_impl_module_schedule_destroy(impl->module);
	return 0;
}

static void module_schedule_destroy(struct impl *impl)
{
	pw_loop_invoke(impl->main_loop, do_schedule_destroy, 1, NULL, 0, false, impl);
}

static void context_state_cb(pa_context *c, void *userdata)
{
	struct impl *impl = userdata;
	bool do_destroy = false;
	switch (pa_context_get_state(c)) {
	case PA_CONTEXT_TERMINATED:
	case PA_CONTEXT_FAILED:
		do_destroy = true;
		SPA_FALLTHROUGH;
	case PA_CONTEXT_READY:
		pa_threaded_mainloop_signal(impl->pa_mainloop, 0);
		break;
	case PA_CONTEXT_UNCONNECTED:
		do_destroy = true;
		break;
	case PA_CONTEXT_CONNECTING:
	case PA_CONTEXT_AUTHORIZING:
	case PA_CONTEXT_SETTING_NAME:
		break;
	}
	if (do_destroy)
		module_schedule_destroy(impl);
}

static void stream_state_cb(pa_stream *s, void * userdata)
{
	struct impl *impl = userdata;
	bool do_destroy = false;
	switch (pa_stream_get_state(s)) {
	case PA_STREAM_FAILED:
	case PA_STREAM_TERMINATED:
		do_destroy = true;
		SPA_FALLTHROUGH;
	case PA_STREAM_READY:
		impl->pa_index = pa_stream_get_index(impl->pa_stream);
		pa_threaded_mainloop_signal(impl->pa_mainloop, 0);
		break;
	case PA_STREAM_UNCONNECTED:
		do_destroy = true;
		break;
	case PA_STREAM_CREATING:
		break;
	}
	if (do_destroy)
		module_schedule_destroy(impl);
}

static void stream_read_request_cb(pa_stream *s, size_t length, void *userdata)
{
	struct impl *impl = userdata;
	int32_t filled;
	uint32_t index;
	pa_usec_t latency;
	int negative;

	filled = spa_ringbuffer_get_write_index(&impl->ring, &index);

	if (filled < 0) {
		pw_log_warn("%p: underrun write:%u filled:%d",
				impl, index, filled);
	} else if (filled + length > RINGBUFFER_SIZE) {
		pw_log_warn("%p: overrun write:%u filled:%d",
				impl, index, filled);
	}
	while (length > 0) {
		const void *p;
		size_t nbytes = 0;

		if (SPA_UNLIKELY(pa_stream_peek(impl->pa_stream, &p, &nbytes) != 0)) {
			pw_log_error("pa_stream_peek() failed: %s",
					pa_strerror(pa_context_errno(impl->pa_context)));
			return;
		}
		pw_log_debug("read %zd nbytes:%zd", length, nbytes);

		if (length < nbytes)
			break;

		while (nbytes > 0) {
			uint32_t to_write = SPA_MIN(nbytes, sizeof(impl->empty));

			spa_ringbuffer_write_data(&impl->ring,
					impl->buffer, RINGBUFFER_SIZE,
					index & RINGBUFFER_MASK,
					p ? p : impl->empty, to_write);

			index += to_write;
			p = p ? SPA_PTROFF(p, to_write, void) : NULL;
			nbytes -= to_write;
			length -= to_write;
			filled += to_write;
		}
		pa_stream_drop(impl->pa_stream);
	}

	pa_stream_get_latency(impl->pa_stream, &latency, &negative);
	impl->current_latency = latency * impl->info.rate / SPA_USEC_PER_SEC;

	spa_ringbuffer_write_update(&impl->ring, index);
}

static void stream_write_request_cb(pa_stream *s, size_t length, void *userdata)
{
	struct impl *impl = userdata;
	int32_t avail;
	uint32_t index;
	size_t size;
	pa_usec_t latency;
	int negative, res;

	if (impl->resync) {
		impl->resync = false;
		avail = length + impl->target_buffer;
		spa_ringbuffer_get_write_index(&impl->ring, &index);
		index -= avail;
	} else {
		avail = spa_ringbuffer_get_read_index(&impl->ring, &index);
	}

	pa_stream_get_latency(impl->pa_stream, &latency, &negative);
	impl->current_latency = latency * impl->info.rate / SPA_USEC_PER_SEC;

	while (avail < (int32_t)length) {
		uint32_t maxsize = SPA_ROUND_DOWN(sizeof(impl->empty), impl->frame_size);
		/* send silence for the data we don't have */
		size = SPA_MIN(length - avail, maxsize);
		if ((res = pa_stream_write(impl->pa_stream,
				impl->empty, size,
				NULL, 0, PA_SEEK_RELATIVE)) != 0)
			pw_log_warn("error writing stream: %s", pa_strerror(res));
		length -= size;
	}
	while (length > 0 && avail >= (int32_t)length) {
		void *data;

		size = length;
		pa_stream_begin_write(impl->pa_stream, &data, &size);

		spa_ringbuffer_read_data(&impl->ring,
				impl->buffer, RINGBUFFER_SIZE,
				index & RINGBUFFER_MASK,
				data, size);

		if ((res = pa_stream_write(impl->pa_stream,
			data, size, NULL, 0, PA_SEEK_RELATIVE)) != 0)
			pw_log_warn("error writing stream: %zd %s", size,
					pa_strerror(res));

		index += size;
		length -= size;
		avail -= size;
		spa_ringbuffer_read_update(&impl->ring, index);
	}
}
static void stream_underflow_cb(pa_stream *s, void *userdata)
{
	struct impl *impl = userdata;
	struct timespec ts;
	int suppressed;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	if ((suppressed = spa_ratelimit_test(&impl->rate_limit, SPA_TIMESPEC_TO_NSEC(&ts))) >= 0)
		pw_log_warn("underflow (%d suppressed)", suppressed);
	impl->resync = true;
}
static void stream_overflow_cb(pa_stream *s, void *userdata)
{
	struct impl *impl = userdata;
	struct timespec ts;
	int suppressed;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	if ((suppressed = spa_ratelimit_test(&impl->rate_limit, SPA_TIMESPEC_TO_NSEC(&ts))) >= 0)
		pw_log_warn("overflow (%d suppressed)", suppressed);
	impl->resync = true;
}

static void stream_latency_update_cb(pa_stream *s, void *userdata)
{
	struct impl *impl = userdata;
	pa_usec_t usec;
	int negative;

	pa_stream_get_latency(s, &usec, &negative);

	pw_log_debug("latency %ld negative %d", usec, negative);
	pa_threaded_mainloop_signal(impl->pa_mainloop, 0);
}

static int
do_stream_sync_volumes(struct spa_loop *loop,
	bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[1];
	struct spa_pod *param;
	uint32_t i;
	float vols[SPA_AUDIO_MAX_CHANNELS];
	float soft_vols[SPA_AUDIO_MAX_CHANNELS];

	for (i = 0; i < impl->volume.channels; i++) {
		vols[i] = pa_sw_volume_to_linear(impl->volume.values[i]);
		soft_vols[i] = 1.0f;
	}

	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&b, SPA_PROP_softMute, 0);
	spa_pod_builder_bool(&b, impl->mute);
	spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
	spa_pod_builder_bool(&b, impl->mute);

	spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
	spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float,
			impl->volume.channels, vols);
	spa_pod_builder_prop(&b, SPA_PROP_softVolumes, 0);
	spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float,
			impl->volume.channels, soft_vols);
	param = spa_pod_builder_pop(&b, &f[0]);

	pw_stream_set_param(impl->stream, SPA_PARAM_Props, param);
	return 0;
}

static void stream_sync_volumes(struct impl *impl, const struct pa_cvolume *volume, bool mute)
{
	impl->mute = mute;
	impl->volume = *volume;
	pw_loop_invoke(impl->main_loop, do_stream_sync_volumes, 1, NULL, 0, false, impl);
}

static void source_output_info_cb(pa_context *c, const pa_source_output_info *i, int eol, void *userdata)
{
	struct impl *impl = userdata;
	if (i != NULL)
		stream_sync_volumes(impl, &i->volume, i->mute);
}

static void sink_input_info_cb(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata)
{
	struct impl *impl = userdata;
	if (i != NULL)
		stream_sync_volumes(impl, &i->volume, i->mute);
}

static void context_subscribe_cb(pa_context *c, pa_subscription_event_type_t t,
		uint32_t idx, void *userdata)
{
	struct impl *impl = userdata;
	if (idx != impl->pa_index)
		return;

	if (impl->mode == MODE_SOURCE)
		pa_context_get_source_output_info(impl->pa_context,
				idx, source_output_info_cb, impl);
	else
		pa_context_get_sink_input_info(impl->pa_context,
				idx, sink_input_info_cb, impl);
}

static pa_proplist* tunnel_new_proplist(struct impl *impl)
{
	pa_proplist *proplist = pa_proplist_new();
	pa_proplist_sets(proplist, PA_PROP_APPLICATION_NAME, "PipeWire");
	pa_proplist_sets(proplist, PA_PROP_APPLICATION_ID, "org.pipewire.PipeWire");
	pa_proplist_sets(proplist, PA_PROP_APPLICATION_VERSION, PACKAGE_VERSION);
	return proplist;
}

static int create_pulse_stream(struct impl *impl)
{
	pa_sample_spec ss;
	pa_channel_map map;
	const char *server_address, *remote_node_target;
	pa_proplist *props = NULL;
	pa_mainloop_api *api;
	char stream_name[1024];
	pa_buffer_attr bufferattr;
	int res = -EIO;
	uint32_t latency_bytes, i, aux = 0;

	if ((impl->pa_mainloop = pa_threaded_mainloop_new()) == NULL)
		goto error;

	api = pa_threaded_mainloop_get_api(impl->pa_mainloop);

	props = tunnel_new_proplist(impl);
	impl->pa_context = pa_context_new_with_proplist(api, "PipeWire", props);
	pa_proplist_free(props);

	if (impl->pa_context == NULL)
		goto error;

	pa_context_set_state_callback(impl->pa_context, context_state_cb, impl);

	server_address = pw_properties_get(impl->props, "pulse.server.address");

	if (pa_context_connect(impl->pa_context, server_address, 0, NULL) < 0) {
		res = pa_context_errno(impl->pa_context);
		goto error;
	}

	pa_threaded_mainloop_lock(impl->pa_mainloop);

	pa_context_set_subscribe_callback(impl->pa_context, context_subscribe_cb, impl);

	if (pa_threaded_mainloop_start(impl->pa_mainloop) < 0)
		goto error_unlock;

	for (;;) {
		pa_context_state_t state;

		state = pa_context_get_state(impl->pa_context);
		if (state == PA_CONTEXT_READY)
			break;

		if (!PA_CONTEXT_IS_GOOD(state)) {
			res = pa_context_errno(impl->pa_context);
			goto error_unlock;
		}
		/* Wait until the context is ready */
		pa_threaded_mainloop_wait(impl->pa_mainloop);
	}

	ss.format = (pa_sample_format_t) format_id2pa(impl->info.format);
	ss.channels = impl->info.channels;
	ss.rate = impl->info.rate;

	map.channels = impl->info.channels;
	for (i = 0; i < map.channels; i++)
		map.map[i] = (pa_channel_position_t)channel_id2pa(impl->info.position[i], &aux);

	snprintf(stream_name, sizeof(stream_name), _("Tunnel for %s@%s"),
			pw_get_user_name(), pw_get_host_name());

	if (!(impl->pa_stream = pa_stream_new(impl->pa_context, stream_name, &ss, &map))) {
		res = pa_context_errno(impl->pa_context);
		goto error_unlock;
	}

	pa_stream_set_state_callback(impl->pa_stream, stream_state_cb, impl);
	pa_stream_set_read_callback(impl->pa_stream, stream_read_request_cb, impl);
	pa_stream_set_write_callback(impl->pa_stream, stream_write_request_cb, impl);
	pa_stream_set_underflow_callback(impl->pa_stream, stream_underflow_cb, impl);
	pa_stream_set_overflow_callback(impl->pa_stream, stream_overflow_cb, impl);
	pa_stream_set_latency_update_callback(impl->pa_stream, stream_latency_update_cb, impl);

	remote_node_target = pw_properties_get(impl->props, PW_KEY_TARGET_OBJECT);

	bufferattr.fragsize = (uint32_t) -1;
	bufferattr.minreq = (uint32_t) -1;
	bufferattr.maxlength = (uint32_t) -1;
	bufferattr.prebuf = (uint32_t) -1;

	latency_bytes = pa_usec_to_bytes(impl->latency_msec * SPA_USEC_PER_MSEC, &ss);

	impl->target_latency = latency_bytes / impl->frame_size;

	/* half in our buffer, half in the network + remote */
	impl->target_buffer = latency_bytes / 2;

	if (impl->mode == MODE_SOURCE) {
		bufferattr.fragsize = latency_bytes / 2;

		pa_context_subscribe(impl->pa_context,
				PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT, NULL, impl);

		res = pa_stream_connect_record(impl->pa_stream,
				remote_node_target, &bufferattr,
				PA_STREAM_DONT_MOVE |
				PA_STREAM_INTERPOLATE_TIMING |
				PA_STREAM_ADJUST_LATENCY |
				PA_STREAM_AUTO_TIMING_UPDATE);
	} else {
		bufferattr.tlength = latency_bytes / 2;
		bufferattr.minreq = bufferattr.tlength / 4;
		bufferattr.prebuf = bufferattr.tlength;

		pa_context_subscribe(impl->pa_context,
				PA_SUBSCRIPTION_MASK_SINK_INPUT, NULL, impl);

		res = pa_stream_connect_playback(impl->pa_stream,
				remote_node_target, &bufferattr,
				PA_STREAM_DONT_MOVE |
				PA_STREAM_INTERPOLATE_TIMING |
				PA_STREAM_ADJUST_LATENCY |
				PA_STREAM_AUTO_TIMING_UPDATE,
				NULL, NULL);
	}

	if (res < 0) {
		res = pa_context_errno(impl->pa_context);
		goto error_unlock;
	}

	for (;;) {
		pa_stream_state_t state;

		state = pa_stream_get_state(impl->pa_stream);
		if (state == PA_STREAM_READY)
			break;

		if (!PA_STREAM_IS_GOOD(state)) {
			res = pa_context_errno(impl->pa_context);
			goto error_unlock;
		}

		/* Wait until the stream is ready */
		pa_threaded_mainloop_wait(impl->pa_mainloop);
	}

	pa_threaded_mainloop_unlock(impl->pa_mainloop);

	return 0;

error_unlock:
	pa_threaded_mainloop_unlock(impl->pa_mainloop);
error:
	pw_log_error("failed to connect: %s", pa_strerror(res));
	return err_to_res(res);
}


static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE) {
		if (impl->module)
			pw_impl_module_schedule_destroy(impl->module);
	}
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
	if (impl->module)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void impl_destroy(struct impl *impl)
{

	if (impl->pa_mainloop)
		pa_threaded_mainloop_stop(impl->pa_mainloop);
	if (impl->pa_stream)
		pa_stream_unref(impl->pa_stream);
	if (impl->pa_context) {
		pa_context_disconnect(impl->pa_context);
		pa_context_unref(impl->pa_context);
	}
	if (impl->pa_mainloop)
		pa_threaded_mainloop_free(impl->pa_mainloop);

	if (impl->stream)
		pw_stream_destroy(impl->stream);
	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	pw_loop_invoke(impl->main_loop, NULL, 0, NULL, 0, false, impl);

	pw_properties_free(impl->stream_props);
	pw_properties_free(impl->props);

	free(impl->buffer);
	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl->module = NULL;
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

static inline uint32_t format_from_name(const char *name, size_t len)
{
	int i;
	for (i = 0; spa_type_audio_format[i].name; i++) {
		if (strncmp(name, spa_debug_type_short_name(spa_type_audio_format[i].name), len) == 0)
			return spa_type_audio_format[i].type;
	}
	return SPA_AUDIO_FORMAT_UNKNOWN;
}

static void parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
	const char *str;

	spa_zero(*info);
	if ((str = pw_properties_get(props, PW_KEY_AUDIO_FORMAT)) == NULL)
		str = DEFAULT_FORMAT;
	info->format = format_from_name(str, strlen(str));

	info->rate = pw_properties_get_uint32(props, PW_KEY_AUDIO_RATE, info->rate);
	if (info->rate == 0)
		info->rate = DEFAULT_RATE;

	info->channels = pw_properties_get_uint32(props, PW_KEY_AUDIO_CHANNELS, info->channels);
	info->channels = SPA_MIN(info->channels, SPA_AUDIO_MAX_CHANNELS);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(info, str, strlen(str));
	if (info->channels == 0)
		parse_position(info, DEFAULT_POSITION, strlen(DEFAULT_POSITION));
}

static int calc_frame_size(struct spa_audio_info_raw *info)
{
	int res = info->channels;
	switch (info->format) {
	case SPA_AUDIO_FORMAT_U8:
	case SPA_AUDIO_FORMAT_S8:
	case SPA_AUDIO_FORMAT_ALAW:
	case SPA_AUDIO_FORMAT_ULAW:
		return res;
	case SPA_AUDIO_FORMAT_S16:
	case SPA_AUDIO_FORMAT_S16_OE:
	case SPA_AUDIO_FORMAT_U16:
		return res * 2;
	case SPA_AUDIO_FORMAT_S24:
	case SPA_AUDIO_FORMAT_S24_OE:
	case SPA_AUDIO_FORMAT_U24:
		return res * 3;
	case SPA_AUDIO_FORMAT_S24_32:
	case SPA_AUDIO_FORMAT_S24_32_OE:
	case SPA_AUDIO_FORMAT_S32:
	case SPA_AUDIO_FORMAT_S32_OE:
	case SPA_AUDIO_FORMAT_U32:
	case SPA_AUDIO_FORMAT_U32_OE:
	case SPA_AUDIO_FORMAT_F32:
	case SPA_AUDIO_FORMAT_F32_OE:
		return res * 4;
	case SPA_AUDIO_FORMAT_F64:
	case SPA_AUDIO_FORMAT_F64_OE:
		return res * 8;
	default:
		return 0;
	}
}

static void copy_props(struct impl *impl, struct pw_properties *props, const char *key)
{
	const char *str;
	if ((str = pw_properties_get(props, key)) != NULL) {
		if (pw_properties_get(impl->stream_props, key) == NULL)
			pw_properties_set(impl->stream_props, key, str);
	}
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props = NULL;
	struct impl *impl;
	const char *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}
	impl->props = props;

	impl->stream_props = pw_properties_new(NULL, NULL);
	if (impl->stream_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->module = module;
	impl->context = context;
	impl->main_loop = pw_context_get_main_loop(context);

	spa_ringbuffer_init(&impl->ring);
	impl->buffer = calloc(1, RINGBUFFER_SIZE);
	spa_dll_init(&impl->dll);
	impl->rate_limit.interval = 2 * SPA_NSEC_PER_SEC;
	impl->rate_limit.burst = 1;

	if ((str = pw_properties_get(props, "tunnel.mode")) != NULL) {
		if (spa_streq(str, "source")) {
			impl->mode = MODE_SOURCE;
		} else if (spa_streq(str, "sink")) {
			impl->mode = MODE_SINK;
		} else {
			pw_log_error("invalid tunnel.mode '%s'", str);
			res = -EINVAL;
			goto error;
		}
	}

	impl->latency_msec = pw_properties_get_uint32(props, "pulse.latency", DEFAULT_LATENCY_MSEC);


	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, PW_KEY_NODE_NETWORK) == NULL)
		pw_properties_set(props, PW_KEY_NODE_NETWORK, "true");

	if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_CLASS,
				impl->mode == MODE_SINK ?
					"Audio/Sink" : "Audio/Source");

	if ((str = pw_properties_get(props, "stream.props")) != NULL)
		pw_properties_update_string(impl->stream_props, str, strlen(str));

	copy_props(impl, props, PW_KEY_AUDIO_FORMAT);
	copy_props(impl, props, PW_KEY_AUDIO_RATE);
	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_NAME);
	copy_props(impl, props, PW_KEY_NODE_DESCRIPTION);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LATENCY);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, PW_KEY_NODE_NETWORK);
	copy_props(impl, props, PW_KEY_MEDIA_CLASS);

	parse_audio_info(impl->stream_props, &impl->info);

	impl->frame_size = calc_frame_size(&impl->info);
	if (impl->frame_size == 0) {
		pw_log_error("unsupported audio format:%d channels:%d",
				impl->info.format, impl->info.channels);
		res = -EINVAL;
		goto error;
	}
	spa_dll_set_bw(&impl->dll, SPA_DLL_BW_MIN, 128, impl->info.rate);
	impl->max_error = 256.0f;

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

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener,
			&core_proxy_events, impl);
	pw_core_add_listener(impl->core,
			&impl->core_listener,
			&core_events, impl);

	if ((res = create_pulse_stream(impl)) < 0)
		goto error;

	if ((res = create_stream(impl)) < 0)
		goto error;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	impl_destroy(impl);
	return res;
}
