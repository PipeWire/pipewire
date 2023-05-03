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
#include <semaphore.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/dll.h>
#include <spa/debug/types.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/raw.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>
#include <pipewire/private.h>

#include <jack/jack.h>

/** \page page_module_jack_tunnel PipeWire Module: JACK Tunnel
 *
 * The jack-tunnel module provides a source or sink that tunnels all audio to
 * a JACK server.
 *
 * This module is usually used together with module-jack-dbus that will
 * automatically load the tunnel with the right parameters based on dbus
 * information.
 *
 * ## Module Options
 *
 * - `jack.server`: the name of the JACK server to tunnel to.
 * - `tunnel.mode`: the tunnel mode, sink|source|duplex, default duplex
 * - `source.props`: Extra properties for the source stream.
 * - `sink.props`: Extra properties for the sink stream.
 *
 * ## General options
 *
 * Options with well-known behavior.
 *
 * - \ref PW_KEY_REMOTE_NAME
 * - \ref PW_KEY_AUDIO_CHANNELS
 * - \ref SPA_KEY_AUDIO_POSITION
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_NODE_GROUP
 * - \ref PW_KEY_NODE_VIRTUAL
 * - \ref PW_KEY_MEDIA_CLASS
 * - \ref PW_KEY_TARGET_OBJECT to specify the remote node.name or serial.id to link to
 *
 * ## Example configuration of a duplex sink/source
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-jack-tunnel
 *     args = {
 *         #jack.server    = null
 *         #tunnel.mode    = duplex
 *         #audio.channels = 2
 *         #audio.position = [ FL FR ]
 *         source.props = {
 *             # extra sink properties
 *         }
 *         sink.props = {
 *             # extra sink properties
 *         }
 *     }
 * }
 * ]
 *\endcode
 */

#define NAME "jack-tunnel"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define DEFAULT_CHANNELS 2
#define DEFAULT_POSITION "[ FL FR ]"

#define MODULE_USAGE	"( remote.name=<remote> ] "				\
			"( node.name=<name of the nodes> ] "			\
			"( node.description=<description of the nodes> ] "	\
			"( audio.channels=<number of channels> ] "		\
			"( audio.position=<channel map> ] "			\
			"( jack.server=<server name> ) "			\
			"( source.props=<properties> ) "			\
			"( sink.props=<properties> ) "


static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create a JACK tunnel" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_context *context;
	struct pw_loop *main_loop;
	struct spa_system *system;

#define MODE_SINK	(1<<0)
#define MODE_SOURCE	(1<<1)
#define MODE_DUPLEX	(MODE_SINK|MODE_SOURCE)
	uint32_t mode;
	struct pw_properties *props;

	struct pw_impl_module *module;

	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct spa_io_position *position;

	struct pw_properties *source_props;
	struct pw_stream *source;
	struct spa_hook source_listener;
	struct spa_audio_info_raw source_info;
	struct spa_latency_info source_latency[2];
	bool source_latency_changed[2];

	jack_client_t *client;
	jack_port_t *source_ports[SPA_AUDIO_MAX_CHANNELS];

	struct pw_properties *sink_props;
	struct pw_stream *sink;
	struct spa_hook sink_listener;
	struct spa_audio_info_raw sink_info;
	struct spa_latency_info sink_latency[2];
	bool sink_latency_changed[2];

	jack_port_t *sink_ports[SPA_AUDIO_MAX_CHANNELS];

	uint32_t nframes;
	uint32_t samplerate;

	jack_nframes_t frames;

	uint32_t pw_xrun;
	uint32_t jack_xrun;

	unsigned int do_disconnect:1;
	unsigned int source_running:1;
	unsigned int sink_running:1;
	unsigned int done:1;
	unsigned int new_xrun:1;
};

static void source_stream_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->source_listener);
	impl->source = NULL;
}

static void sink_stream_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->sink_listener);
	impl->sink = NULL;
}

static void sink_stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = d;
	switch (state) {
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_STREAM_STATE_PAUSED:
		impl->sink_running = false;
		break;
	case PW_STREAM_STATE_STREAMING:
		impl->sink_running = true;
		break;
	default:
		break;
	}
}

static void sink_stream_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *buf;
	struct spa_data *bd;
	uint32_t i, offs, size;
	void *data;

	if ((buf = pw_stream_dequeue_buffer(impl->sink)) == NULL) {
		pw_log_warn("out of buffers: %m");
		goto done;
	}

	for (i = 0; i < buf->buffer->n_datas; i++) {
		if (impl->sink_ports[i] == NULL)
			break;

		bd = &buf->buffer->datas[i];
		offs = SPA_MIN(bd->chunk->offset, bd->maxsize);
		size = SPA_MIN(bd->chunk->size, bd->maxsize - offs);
		size = SPA_MIN(size, impl->nframes * sizeof(float));

		data = jack_port_get_buffer (impl->sink_ports[i], impl->nframes);
		memcpy(data, SPA_PTROFF(bd->data, offs, void), size);
	}
	pw_stream_queue_buffer(impl->sink, buf);
done:
	pw_log_trace_fp("done %u", impl->frames);
	impl->done = true;
	jack_cycle_signal(impl->client, 0);
}

static void source_stream_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *buf;
	struct spa_data *bd;
	uint32_t i, size;
	void *data;

	if ((buf = pw_stream_dequeue_buffer(impl->source)) == NULL) {
		pw_log_warn("out of buffers: %m");
		return;
	}

	for (i = 0; i < buf->buffer->n_datas; i++) {
		if (impl->source_ports[i] == NULL)
			break;

		bd = &buf->buffer->datas[i];
		size = SPA_MIN(bd->maxsize, impl->nframes * sizeof(float));

		data = jack_port_get_buffer (impl->source_ports[i], impl->nframes);
		memcpy(bd->data, data, size);

		bd->chunk->offset = 0;
		bd->chunk->size = size;
		bd->chunk->stride = sizeof(float);
	}
	pw_stream_queue_buffer(impl->source, buf);
	pw_log_trace_fp("done");
}

static void source_stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = d;
	switch (state) {
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_STREAM_STATE_PAUSED:
		impl->source_running = false;
		break;
	case PW_STREAM_STATE_STREAMING:
		impl->source_running = true;
		break;
	default:
		break;
	}
}
static void stream_io_changed(void *data, uint32_t id, void *area, uint32_t size)
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

static void param_latency_changed(struct impl *impl, const struct spa_pod *param,
		enum spa_direction direction)
{
	struct spa_latency_info latency;
	bool update = false;

	if (spa_latency_parse(param, &latency) < 0)
		return;

	if (direction == SPA_DIRECTION_OUTPUT) {
		if (spa_latency_info_compare(&impl->sink_latency[direction], &latency)) {
			impl->sink_latency[direction] = latency;
			impl->sink_latency_changed[direction] = update = true;
		}
	} else {
		if (spa_latency_info_compare(&impl->source_latency[direction], &latency)) {
			impl->source_latency[direction] = latency;
			impl->source_latency_changed[direction] = update = true;
		}
	}
	if (update)
		jack_recompute_total_latencies(impl->client);
}

static void sink_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct impl *impl = data;
	switch (id) {
	case SPA_PARAM_Latency:
		param_latency_changed(impl, param, SPA_DIRECTION_OUTPUT);
		break;
	}
}
static void source_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct impl *impl = data;
	switch (id) {
	case SPA_PARAM_Latency:
		param_latency_changed(impl, param, SPA_DIRECTION_INPUT);
		break;
	}
}

static const struct pw_stream_events sink_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = sink_stream_destroy,
	.state_changed = sink_stream_state_changed,
	.param_changed = sink_param_changed,
	.io_changed = stream_io_changed,
	.process = sink_stream_process
};

static const struct pw_stream_events source_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = source_stream_destroy,
	.state_changed = source_stream_state_changed,
	.param_changed = source_param_changed,
	.io_changed = stream_io_changed,
	.process = source_stream_process,
};

static int create_streams(struct impl *impl)
{
	int res;
	uint32_t n_params;
	const struct spa_pod *params[2];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	struct spa_latency_info latency;

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	spa_zero(latency);

	if (impl->mode & MODE_SINK) {
		impl->sink = pw_stream_new(impl->core, "JACK Sink", impl->sink_props);
		impl->sink_props = NULL;
		if (impl->sink == NULL)
			return -errno;

		pw_stream_add_listener(impl->sink,
				&impl->sink_listener,
				&sink_stream_events, impl);

		params[n_params++] = spa_format_audio_raw_build(&b,
				SPA_PARAM_EnumFormat, &impl->sink_info);

		if ((res = pw_stream_connect(impl->sink,
				PW_DIRECTION_INPUT,
				PW_ID_ANY,
				PW_STREAM_FLAG_AUTOCONNECT |
				PW_STREAM_FLAG_MAP_BUFFERS |
				PW_STREAM_FLAG_DRIVER |
				PW_STREAM_FLAG_RT_PROCESS,
				params, n_params)) < 0)
			return res;
	}
	if (impl->mode & MODE_SOURCE) {
		impl->source = pw_stream_new(impl->core, "JACK Source", impl->source_props);
		impl->source_props = NULL;
		if (impl->source == NULL)
			return -errno;

		pw_stream_add_listener(impl->source,
				&impl->source_listener,
				&source_stream_events, impl);

		params[n_params++] = spa_format_audio_raw_build(&b,
				SPA_PARAM_EnumFormat, &impl->source_info);

		if ((res = pw_stream_connect(impl->source,
				PW_DIRECTION_OUTPUT,
				PW_ID_ANY,
				PW_STREAM_FLAG_AUTOCONNECT |
				PW_STREAM_FLAG_MAP_BUFFERS |
				PW_STREAM_FLAG_DRIVER |
				PW_STREAM_FLAG_RT_PROCESS,
				params, n_params)) < 0)
			return res;
	}
	return 0;
}

static void *jack_process_thread(void *arg)
{
	struct impl *impl = arg;
	bool source_running, sink_running;
	jack_nframes_t nframes;

	while (true) {
		nframes = jack_cycle_wait (impl->client);

		source_running = impl->source_running;
		sink_running = impl->sink_running;

		impl->frames = jack_frame_time(impl->client);

		pw_log_trace_fp("process %d %u %u %p %d", nframes, source_running,
				sink_running, impl->position, impl->frames);

		if (impl->new_xrun) {
			pw_log_warn("Xrun JACK:%u PipeWire:%u", impl->jack_xrun, impl->pw_xrun);
			impl->new_xrun = false;
		}

		if (impl->position) {
			struct spa_io_clock *c = &impl->position->clock;
			jack_nframes_t current_frames;
			jack_time_t current_usecs;
			jack_time_t next_usecs;
			float period_usecs;
			jack_position_t pos;

			jack_get_cycle_times(impl->client,
					&current_frames, &current_usecs,
					&next_usecs, &period_usecs);

			c->nsec = current_usecs * SPA_NSEC_PER_USEC;
			c->rate = SPA_FRACTION(1, impl->samplerate);
			c->position = current_frames;
			c->duration = nframes;
			c->delay = 0;
			c->rate_diff = 1.0;
			c->next_nsec = next_usecs * SPA_NSEC_PER_USEC;

			c->target_rate = c->rate;
			c->target_duration = c->duration;

			jack_transport_query (impl->client, &pos);
		}
		impl->nframes = nframes;

		if (sink_running && source_running) {
			impl->done = false;
			pw_stream_trigger_process(impl->sink);
		} else {
			jack_cycle_signal(impl->client, 0);
		}
	}
	return NULL;
}

static int jack_xrun(void *arg)
{
	struct impl *impl = arg;
	if (impl->done)
		impl->jack_xrun++;
	else
		impl->pw_xrun++;
	impl->new_xrun = true;
	return 0;
}

static int
do_schedule_destroy(struct spa_loop *loop,
	bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	pw_impl_module_schedule_destroy(impl->module);
	return 0;
}

void module_schedule_destroy(struct impl *impl)
{
	pw_loop_invoke(impl->main_loop, do_schedule_destroy, 1, NULL, 0, false, impl);
}

static void jack_info_shutdown(jack_status_t code, const char* reason, void *arg)
{
	struct impl *impl = arg;
	pw_log_warn("shutdown: %s (%08x)", reason, code);
	module_schedule_destroy(impl);
}

static int
do_update_latency(struct spa_loop *loop,
		bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	const struct spa_pod *params[2];
	uint32_t n_params = 0;
	uint8_t buffer[1024];
	struct spa_pod_builder b;

	if ((impl->mode & MODE_SINK)) {
		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		if (impl->sink_latency_changed[SPA_DIRECTION_INPUT]) {
			params[n_params++] = spa_latency_build(&b,
					SPA_PARAM_Latency, &impl->sink_latency[SPA_DIRECTION_INPUT]);
			impl->sink_latency_changed[SPA_DIRECTION_INPUT] = false;
		}
		if (impl->sink)
			pw_stream_update_params(impl->sink, params, n_params);
	}

	if ((impl->mode & MODE_SOURCE)) {
		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		if (impl->source_latency_changed[SPA_DIRECTION_OUTPUT]) {
			params[n_params++] = spa_latency_build(&b,
					SPA_PARAM_Latency, &impl->source_latency[SPA_DIRECTION_OUTPUT]);
			impl->source_latency_changed[SPA_DIRECTION_OUTPUT] = false;
		}
		if (impl->source)
			pw_stream_update_params(impl->source, params, n_params);
	}
	return 0;
}

static void jack_latency(jack_latency_callback_mode_t mode, void *arg)
{
	struct impl *impl = arg;
	uint32_t i;
	jack_latency_range_t range, total;
	struct spa_latency_info latency;
	bool update = false;

	spa_zero(latency);

	if ((impl->mode & MODE_SINK)) {
		if (mode == JackPlaybackLatency) {
			total.min = total.max = 0;
			for (i = 0; i < impl->sink_info.channels; i++) {
				jack_port_get_latency_range(impl->sink_ports[i], mode, &range);
				if (total.min == 0 || range.min < total.min)
					total.min = range.min;
				if (total.max == 0 || range.max > total.max)
					total.max = range.max;
			}
			pw_log_debug("sink latency %d %d %d", mode, total.min, total.max);

			latency.direction = PW_DIRECTION_INPUT;
			latency.min_rate = total.min;
			latency.max_rate = total.max;

			if (spa_latency_info_compare(&latency, &impl->sink_latency[PW_DIRECTION_INPUT])) {
				impl->sink_latency[PW_DIRECTION_INPUT] = latency;
				impl->sink_latency_changed[PW_DIRECTION_INPUT] = update = true;
			}
		} else if (mode == JackCaptureLatency) {
			if (impl->sink_latency_changed[PW_DIRECTION_OUTPUT]) {
				range.min = impl->sink_latency[PW_DIRECTION_OUTPUT].min_rate;
				range.max = impl->sink_latency[PW_DIRECTION_OUTPUT].max_rate;
				for (i = 0; i < impl->sink_info.channels; i++)
					jack_port_set_latency_range(impl->sink_ports[i], mode, &range);
				impl->sink_latency_changed[PW_DIRECTION_OUTPUT] = false;
			}
		}
	}
	if ((impl->mode & MODE_SOURCE) && mode == JackCaptureLatency) {
		if (mode == JackCaptureLatency) {
			total.min = total.max = 0;
			for (i = 0; i < impl->source_info.channels; i++) {
				jack_port_get_latency_range(impl->source_ports[i], mode, &range);
				if (total.min == 0 || range.min < total.min)
					total.min = range.min;
				if (total.max == 0 || range.max > total.max)
					total.max = range.max;
			}
			pw_log_debug("source latency %d %d %d", mode, total.min, total.max);

			latency.direction = PW_DIRECTION_OUTPUT;
			latency.min_rate = total.min;
			latency.max_rate = total.max;

			if (spa_latency_info_compare(&latency, &impl->source_latency[PW_DIRECTION_OUTPUT])) {
				impl->source_latency[PW_DIRECTION_OUTPUT] = latency;
				impl->source_latency_changed[PW_DIRECTION_OUTPUT] = update = true;
			}
		} else if (mode == JackPlaybackLatency) {
			if (impl->source_latency_changed[PW_DIRECTION_INPUT]) {
				range.min = impl->source_latency[PW_DIRECTION_INPUT].min_rate;
				range.max = impl->source_latency[PW_DIRECTION_INPUT].max_rate;
				for (i = 0; i < impl->sink_info.channels; i++)
					jack_port_set_latency_range(impl->source_ports[i], mode, &range);
				impl->source_latency_changed[PW_DIRECTION_INPUT] = false;
			}
		}
	}
	if (update)
		pw_loop_invoke(impl->main_loop, do_update_latency, 0, NULL, 0, false, impl);
}

static int create_jack_client(struct impl *impl)
{
	const char *server_name;
	jack_options_t options = JackNullOption;
	jack_status_t status;
	uint32_t i;
	char name[256];

	server_name = pw_properties_get(impl->props, "jack.server");

	if (server_name != NULL)
		options |= JackServerName;

	impl->client = jack_client_open("pipewire", options, &status, server_name);
	if (impl->client == NULL) {
		pw_log_error ("jack_client_open() failed 0x%2.0x\n", status);
		return -EIO;
	}
	jack_on_info_shutdown(impl->client, jack_info_shutdown, impl);
	jack_set_process_thread(impl->client, jack_process_thread, impl);
	jack_set_xrun_callback(impl->client, jack_xrun, impl);
	jack_set_latency_callback(impl->client, jack_latency, impl);

	if (impl->mode & MODE_SINK) {
		for (i = 0; i < impl->sink_info.channels; i++) {
			snprintf(name, sizeof(name), "output%d", i);
			impl->sink_ports[i] = jack_port_register (impl->client, name,
						JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
		}

	}
	if (impl->mode & MODE_SOURCE) {
		for (i = 0; i < impl->source_info.channels; i++) {
			snprintf(name, sizeof(name), "input%d", i);
			impl->source_ports[i] = jack_port_register (impl->client, name,
						JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
		}
	}

	impl->samplerate = jack_get_sample_rate(impl->client);
	impl->source_info.rate = impl->samplerate;
	impl->sink_info.rate = impl->samplerate;
	return 0;
}

static int start_jack_clients(struct impl *impl)
{
	const char **ports;
	uint32_t i;

	jack_activate(impl->client);

	ports = jack_get_ports(impl->client, NULL, NULL,
                                JackPortIsPhysical|JackPortIsInput);
	if (ports != NULL) {
		for (i = 0; i < impl->sink_info.channels; i++) {
			if (ports[i] == NULL)
				break;
			if (jack_connect(impl->client, jack_port_name(impl->sink_ports[i]), ports[i])) {
				pw_log_error("cannot connect output ports");
				break;
			}
		}
		jack_free(ports);
        }

	ports = jack_get_ports(impl->client, NULL, NULL,
                                JackPortIsPhysical|JackPortIsOutput);
	if (ports != NULL) {
		for (i = 0; i < impl->source_info.channels; i++) {
			if (ports[i] == NULL)
				break;
			if (jack_connect(impl->client, ports[i], jack_port_name(impl->source_ports[i]))) {
				pw_log_error("cannot connect input ports");
				break;
			}
		}
		jack_free(ports);
        }
	return 0;
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
	if (impl->client) {
		jack_deactivate(impl->client);
		jack_client_close(impl->client);
	}
	if (impl->source)
		pw_stream_destroy(impl->source);
	if (impl->sink)
		pw_stream_destroy(impl->sink);
	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	pw_properties_free(impl->sink_props);
	pw_properties_free(impl->source_props);
	pw_properties_free(impl->props);

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

static void parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
	const char *str;

	spa_zero(*info);
	info->format = SPA_AUDIO_FORMAT_F32P;
	info->rate = 0;
	info->channels = pw_properties_get_uint32(props, PW_KEY_AUDIO_CHANNELS, info->channels);
	info->channels = SPA_MIN(info->channels, SPA_AUDIO_MAX_CHANNELS);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(info, str, strlen(str));
	if (info->channels == 0)
		parse_position(info, DEFAULT_POSITION, strlen(DEFAULT_POSITION));
}

static void copy_props(struct impl *impl, struct pw_properties *props, const char *key)
{
	const char *str;
	if ((str = pw_properties_get(props, key)) != NULL) {
		if (pw_properties_get(impl->sink_props, key) == NULL)
			pw_properties_set(impl->sink_props, key, str);
		if (pw_properties_get(impl->source_props, key) == NULL)
			pw_properties_set(impl->source_props, key, str);
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

	impl->sink_props = pw_properties_new(NULL, NULL);
	impl->source_props = pw_properties_new(NULL, NULL);
	if (impl->source_props == NULL || impl->sink_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->module = module;
	impl->context = context;
	impl->main_loop = pw_context_get_main_loop(context);
	impl->system = impl->main_loop->system;


	impl->mode = MODE_DUPLEX;
	if ((str = pw_properties_get(props, "tunnel.mode")) != NULL) {
		if (spa_streq(str, "source")) {
			impl->mode = MODE_SOURCE;
		} else if (spa_streq(str, "sink")) {
			impl->mode = MODE_SINK;
		} else if (spa_streq(str, "duplex")) {
			impl->mode = MODE_DUPLEX;
		} else {
			pw_log_error("invalid tunnel.mode '%s'", str);
			res = -EINVAL;
			goto error;
		}
	}

	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_set(props, PW_KEY_NODE_GROUP, "jack-group");
	if (pw_properties_get(props, PW_KEY_NODE_ALWAYS_PROCESS) == NULL)
		pw_properties_set(props, PW_KEY_NODE_ALWAYS_PROCESS, "true");

	pw_properties_set(impl->sink_props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
	pw_properties_set(impl->sink_props, PW_KEY_PRIORITY_DRIVER, "30001");
	pw_properties_set(impl->sink_props, PW_KEY_NODE_NAME, "jack_sink");
	pw_properties_set(impl->sink_props, PW_KEY_NODE_DESCRIPTION, "JACK Sink");
	pw_properties_set(impl->source_props, "resample.disable", "true");

	pw_properties_set(impl->source_props, PW_KEY_MEDIA_CLASS, "Audio/Source");
	pw_properties_set(impl->source_props, PW_KEY_PRIORITY_DRIVER, "30000");
	pw_properties_set(impl->source_props, PW_KEY_NODE_NAME, "jack_source");
	pw_properties_set(impl->source_props, PW_KEY_NODE_DESCRIPTION, "JACK Source");
	pw_properties_set(impl->source_props, "resample.disable", "true");

	if ((str = pw_properties_get(props, "sink.props")) != NULL)
		pw_properties_update_string(impl->sink_props, str, strlen(str));
	if ((str = pw_properties_get(props, "source.props")) != NULL)
		pw_properties_update_string(impl->source_props, str, strlen(str));

	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_ALWAYS_PROCESS);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);

	parse_audio_info(impl->source_props, &impl->source_info);
	parse_audio_info(impl->sink_props, &impl->sink_info);

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

	if ((res = create_jack_client(impl)) < 0)
		goto error;

	if ((res = create_streams(impl)) < 0)
		goto error;

	if ((res = start_jack_clients(impl)) < 0)
		goto error;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	impl_destroy(impl);
	return res;
}
