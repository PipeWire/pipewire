/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

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
#include <spa/debug/pod.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/raw.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>
#include <pipewire/private.h>

#include <pulse/pulseaudio.h>
#include "module-protocol-pulse/format.h"

/** \page page_module_pulse_tunnel PipeWire Module: Pulse Tunnel
 */

#define NAME "pulse-tunnel"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE	"[ remote.name=<remote> ] "				\
			"[ node.latency=<latency as fraction> ] "		\
			"[ node.name=<name of the nodes> ] "			\
			"[ node.description=<description of the nodes> ] "	\
			"[ node.target=<remote node target name> ] "		\
			"[ audio.rate=<sample rate> ] "				\
			"[ audio.channels=<number of channels> ] "		\
			"[ audio.position=<channel map> ] "			\
			"pulse.server.address=<address> "			\
			"pulse.latency=<latency in msec> "			\
			"[ tunnel.mode=capture|playback "			\
			"[ stream.props=<properties> ] "


static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create a PulseAudio tunnel" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

#define RINGBUFFER_SIZE		(1u << 22)
#define RINGBUFFER_MASK		(RINGBUFFER_SIZE-1)

#define DEFAULT_LATENCY_MSEC	(100)

struct impl {
	struct pw_context *context;

#define MODE_PLAYBACK	0
#define MODE_CAPTURE	1
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
	struct spa_io_rate_match *rate_match;
	struct spa_audio_info_raw info;
	uint32_t frame_size;

	struct spa_ringbuffer ring;
	void *buffer;
	uint8_t empty[8192];

	pa_threaded_mainloop *pa_mainloop;
	pa_context *pa_context;
	pa_stream *pa_stream;

	unsigned int do_disconnect:1;
};

static void cork_stream(struct impl *impl, bool cork)
{
	pa_operation *operation;

	pa_threaded_mainloop_lock(impl->pa_mainloop);

	pw_log_info("corking: %d", cork);
	if (cork && impl->mode == MODE_PLAYBACK) {
		/* When the sink becomes suspended (which is the only case where we
		 * cork the stream), we don't want to keep any old data around, because
		 * the old data is most likely unrelated to the audio that will be
		 * played at the time when the sink starts running again. */
		if ((operation = pa_stream_flush(impl->pa_stream, NULL, NULL)))
			pa_operation_unref(operation);
		spa_ringbuffer_init(&impl->ring);
	}
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

static void playback_stream_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *buf;
	struct spa_data *bd;
	int32_t filled;
	uint32_t write_index, size;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	bd = &buf->buffer->datas[0];
	size = SPA_MIN(bd->chunk->size, RINGBUFFER_SIZE);

	filled = spa_ringbuffer_get_write_index(&impl->ring, &write_index);
	if (filled < 0) {
		pw_log_warn("%p: underrun write:%u filled:%d",
				impl, write_index, filled);
	} else if ((uint32_t)filled + size > RINGBUFFER_SIZE) {
		pw_log_debug("%p: overrun write:%u filled:%d size:%u max:%u",
                                        impl, write_index, filled,
                                        size, RINGBUFFER_SIZE);
	}
	spa_ringbuffer_write_data(&impl->ring,
				impl->buffer, RINGBUFFER_SIZE,
                                write_index & RINGBUFFER_MASK,
                                SPA_PTROFF(bd->data, bd->chunk->offset, void),
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
	uint32_t size, req, read_index;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	bd = &buf->buffer->datas[0];

	if (impl->rate_match)
		req = impl->rate_match->size * impl->frame_size;
	else
		req = 4096;

	avail = spa_ringbuffer_get_read_index(&impl->ring, &read_index);
	if (avail <= 0) {
		size = SPA_MIN(bd->maxsize, req);
		memset(bd->data, 0, size);
	} else {
		size = SPA_MIN(bd->maxsize, (uint32_t)avail);
		size = SPA_MIN(size, req);

		spa_ringbuffer_read_data(&impl->ring,
				impl->buffer, RINGBUFFER_SIZE,
				read_index & RINGBUFFER_MASK,
				bd->data, size);

		read_index += size;
		spa_ringbuffer_read_update(&impl->ring, read_index);
	}
	bd->chunk->offset = 0;
	bd->chunk->size = size;

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
	.process = playback_stream_process
};

static const struct pw_stream_events capture_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.io_changed = stream_io_changed,
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

	if (impl->mode == MODE_CAPTURE) {
		pw_stream_add_listener(impl->stream,
				&impl->stream_listener,
				&capture_stream_events, impl);
	} else {
		pw_stream_add_listener(impl->stream,
				&impl->stream_listener,
				&playback_stream_events, impl);
	}

	impl->frame_size = 2 * 2;

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &impl->info);

	spa_zero(latency);
	latency.direction = impl->mode == MODE_CAPTURE ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT;
	latency.min_ns = latency.max_ns = impl->latency_msec * SPA_NSEC_PER_MSEC;

	params[n_params++] = spa_latency_build(&b,
			SPA_PARAM_Latency, &latency);

	if ((res = pw_stream_connect(impl->stream,
			impl->mode == MODE_CAPTURE ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	return 0;
}

static void context_state_cb(pa_context *c, void *userdata)
{
	struct impl *impl = userdata;
	switch (pa_context_get_state(c)) {
	case PA_CONTEXT_READY:
	case PA_CONTEXT_TERMINATED:
	case PA_CONTEXT_FAILED:
		pa_threaded_mainloop_signal(impl->pa_mainloop, 0);
		break;
	case PA_CONTEXT_UNCONNECTED:
	case PA_CONTEXT_CONNECTING:
	case PA_CONTEXT_AUTHORIZING:
	case PA_CONTEXT_SETTING_NAME:
		break;
	}
}

static void stream_state_cb(pa_stream *s, void * userdata)
{
	struct impl *impl = userdata;
	switch (pa_stream_get_state(s)) {
	case PA_STREAM_READY:
	case PA_STREAM_FAILED:
	case PA_STREAM_TERMINATED:
		pa_threaded_mainloop_signal(impl->pa_mainloop, 0);
		break;
	case PA_STREAM_UNCONNECTED:
	case PA_STREAM_CREATING:
		break;
	}
}

static void stream_read_request_cb(pa_stream *s, size_t length, void *userdata)
{
	struct impl *impl = userdata;
	int32_t filled;
	uint32_t write_index;

	filled = spa_ringbuffer_get_write_index(&impl->ring, &write_index);

	if (filled < 0) {
		pw_log_warn("%p: underrun write:%u filled:%d",
				impl, write_index, filled);
	} else if (filled + length > RINGBUFFER_SIZE) {
		pw_log_warn("%p: overrun write:%u filled:%d",
				impl, write_index, filled);
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
					write_index & RINGBUFFER_MASK,
					p ? p : impl->empty, to_write);

			write_index += to_write;
			p = p ? SPA_PTROFF(p, to_write, void) : NULL;
			nbytes -= to_write;
			length -= to_write;
		}
		pa_stream_drop(impl->pa_stream);
	}
	spa_ringbuffer_write_update(&impl->ring, write_index);
}

static void stream_write_request_cb(pa_stream *s, size_t length, void *userdata)
{
	struct impl *impl = userdata;
	int32_t avail;
	uint32_t read_index, len, offset, l0, l1;

	avail = spa_ringbuffer_get_read_index(&impl->ring, &read_index);

	while (avail < (int32_t)length) {
		/* send silence for the data we don't have */
		len = SPA_MIN(length - avail, sizeof(impl->empty));
		pa_stream_write(impl->pa_stream,
				impl->empty, len,
				NULL, 0, PA_SEEK_RELATIVE);
		length -= len;
	}
	if (length > 0 && avail >= (int32_t)length) {
		/* always send as much as is requested */
		len = length;
		offset = read_index & RINGBUFFER_MASK;
		l0 = SPA_MIN(len, RINGBUFFER_SIZE - offset);
		l1 = len - l0;

		pa_stream_write(impl->pa_stream,
				SPA_PTROFF(impl->buffer, offset, void), l0,
				NULL, 0, PA_SEEK_RELATIVE);

		if (SPA_UNLIKELY(l1 > 0)) {
			pa_stream_write(impl->pa_stream,
					impl->buffer, l1,
					NULL, 0, PA_SEEK_RELATIVE);
		}
		read_index += len;
		spa_ringbuffer_read_update(&impl->ring, read_index);
	}
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
	const char *server_address, *remote_node_target;
	pa_proplist *props = NULL;
	pa_mainloop_api *api;
	char stream_name[1024];
	pa_buffer_attr bufferattr;
	int res = -EIO;

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

	snprintf(stream_name, sizeof(stream_name), _("Tunnel for %s@%s"),
			pw_get_user_name(), pw_get_host_name());

	if (!(impl->pa_stream = pa_stream_new(impl->pa_context, stream_name, &ss, NULL))) {
		res = pa_context_errno(impl->pa_context);
		goto error_unlock;
	}

	pa_stream_set_state_callback(impl->pa_stream, stream_state_cb, impl);
	pa_stream_set_read_callback(impl->pa_stream, stream_read_request_cb, impl);
	pa_stream_set_write_callback(impl->pa_stream, stream_write_request_cb, impl);
	pa_stream_set_latency_update_callback(impl->pa_stream, stream_latency_update_cb, impl);

	remote_node_target = pw_properties_get(impl->props, PW_KEY_NODE_TARGET);

	bufferattr.fragsize = (uint32_t) -1;
	bufferattr.minreq = (uint32_t) -1;
	bufferattr.maxlength = (uint32_t) -1;
	bufferattr.prebuf = (uint32_t) -1;

	if (impl->mode == MODE_CAPTURE) {
		bufferattr.fragsize = pa_usec_to_bytes(impl->latency_msec * SPA_USEC_PER_MSEC, &ss);

		res = pa_stream_connect_record(impl->pa_stream,
				remote_node_target, &bufferattr,
				PA_STREAM_INTERPOLATE_TIMING |
				PA_STREAM_ADJUST_LATENCY |
				PA_STREAM_AUTO_TIMING_UPDATE);
	} else {
		bufferattr.tlength = pa_usec_to_bytes(impl->latency_msec * SPA_USEC_PER_MSEC, &ss);

		res = pa_stream_connect_playback(impl->pa_stream,
				remote_node_target, &bufferattr,
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
	return -res;
}


static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

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

	pw_properties_free(impl->stream_props);
	pw_properties_free(impl->props);

	free(impl->buffer);
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

static inline uint32_t format_from_name(const char *name, size_t len)
{
	int i;
	for (i = 0; spa_type_audio_format[i].name; i++) {
		if (strncmp(name, spa_debug_type_short_name(spa_type_audio_format[i].name), len) == 0)
			return spa_type_audio_format[i].type;
	}
	return SPA_AUDIO_FORMAT_UNKNOWN;
}

static void parse_audio_info(struct pw_properties *props, struct spa_audio_info_raw *info)
{
	const char *str;

	*info = SPA_AUDIO_INFO_RAW_INIT(
			.rate = 48000,
			.channels = 2,
			.format = SPA_AUDIO_FORMAT_S16);

	if ((str = pw_properties_get(props, PW_KEY_AUDIO_FORMAT)) != NULL) {
		uint32_t id;

		id = format_from_name(str, strlen(str));
		if (id != SPA_AUDIO_FORMAT_UNKNOWN)
			info->format = id;
	}

	info->rate = pw_properties_get_uint32(props, PW_KEY_AUDIO_RATE, info->rate);
	info->channels = pw_properties_get_uint32(props, PW_KEY_AUDIO_CHANNELS, info->channels);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(info, str, strlen(str));
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

	spa_ringbuffer_init(&impl->ring);
	impl->buffer = calloc(1, RINGBUFFER_SIZE);

	if ((str = pw_properties_get(props, "tunnel.mode")) != NULL) {
		if (spa_streq(str, "capture")) {
			impl->mode = MODE_CAPTURE;
		} else if (spa_streq(str, "playback")) {
			impl->mode = MODE_PLAYBACK;
		} else {
			pw_log_error("invalid tunnel.mode '%s'", str);
			res = -EINVAL;
			goto error;
		}
	}

	impl->latency_msec = pw_properties_get_uint32(props, "pulse.latency", DEFAULT_LATENCY_MSEC);

	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_set(props, PW_KEY_NODE_GROUP, "pipewire.dummy");
	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, PW_KEY_NODE_NETWORK) == NULL)
		pw_properties_set(props, PW_KEY_NODE_NETWORK, "true");

	if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_CLASS,
				impl->mode == MODE_PLAYBACK ?
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
	copy_props(impl, props, PW_KEY_MEDIA_CLASS);

	parse_audio_info(impl->stream_props, &impl->info);

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
