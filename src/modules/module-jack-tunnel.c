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
#include <spa/debug/types.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/raw.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>
#include <pipewire/private.h>

#include "module-jack-tunnel/weakjack.h"

/** \page page_module_jack_tunnel PipeWire Module: JACK Tunnel
 *
 * The jack-tunnel module provides a source or sink that tunnels all audio to
 * a JACK server.
 *
 * This module is usually used together with \ref page_module_jackdbus_detect that will
 * automatically load the tunnel with the right parameters based on dbus
 * information.
 *
 * ## Module Options
 *
 * - `jack.library`: the libjack to load, by default libjack.so.0 is searched in
 *			JACK_PATH directories and then some standard library paths.
 *			Can be an absolute path.
 * - `jack.server`: the name of the JACK server to tunnel to.
 * - `jack.client-name`: the name of the JACK client.
 * - `jack.connect`: if jack ports should be connected automatically can also be
 *                   placed per stream.
 * - `tunnel.mode`: the tunnel mode, sink|source|duplex, default duplex
 * - `source.props`: Extra properties for the source filter.
 * - `sink.props`: Extra properties for the sink filter.
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
 *         #jack.library     = libjack.so.0
 *         #jack.server      = null
 *         #jack.client-name = PipeWire
 *         #jack.connect     = true
 *         #tunnel.mode      = duplex
 *         #audio.channels   = 2
 *         #audio.position   = [ FL FR ]
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

#define DEFAULT_CLIENT_NAME	"PipeWire"
#define DEFAULT_CHANNELS	2
#define DEFAULT_POSITION	"[ FL FR ]"

#define MODULE_USAGE	"( remote.name=<remote> ] "				\
			"( jack.library=<jack library path> ) "			\
			"( jack.server=<server name> ) "			\
			"( jack.client-name=<name of the JACK client> ] "	\
			"( jack.connect=<bool, autoconnect ports> ] "		\
			"( tunnel.mode=<sink|source|duplex> ] "			\
			"( audio.channels=<number of channels> ] "		\
			"( audio.position=<channel map> ] "			\
			"( source.props=<properties> ) "			\
			"( sink.props=<properties> ) "


static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create a JACK tunnel" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static struct weakjack jack;

struct port {
	jack_port_t *jack_port;

	enum spa_direction direction;
	struct spa_latency_info latency[2];
	bool latency_changed[2];
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
	struct pw_filter *source;
	struct spa_hook source_listener;
	struct spa_audio_info_raw source_info;
	struct port *source_ports[SPA_AUDIO_MAX_CHANNELS];

	struct pw_properties *sink_props;
	struct pw_filter *sink;
	struct spa_hook sink_listener;
	struct spa_audio_info_raw sink_info;
	struct port *sink_ports[SPA_AUDIO_MAX_CHANNELS];

	uint32_t samplerate;

	jack_client_t *client;
	jack_nframes_t frame_time;

	uint32_t pw_xrun;
	uint32_t jack_xrun;

	unsigned int do_disconnect:1;
	unsigned int source_running:1;
	unsigned int sink_running:1;
	unsigned int done:1;
	unsigned int new_xrun:1;
	unsigned int source_connect:1;
	unsigned int sink_connect:1;
};

static void source_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->source_listener);
	impl->source = NULL;
}

static void sink_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->sink_listener);
	impl->sink = NULL;
}

static void sink_state_changed(void *d, enum pw_filter_state old,
		enum pw_filter_state state, const char *error)
{
	struct impl *impl = d;
	switch (state) {
	case PW_FILTER_STATE_ERROR:
	case PW_FILTER_STATE_UNCONNECTED:
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_FILTER_STATE_PAUSED:
		impl->sink_running = false;
		break;
	case PW_FILTER_STATE_STREAMING:
		impl->sink_running = true;
		break;
	default:
		break;
	}
}

static void sink_process(void *d, struct spa_io_position *position)
{
	struct impl *impl = d;
	uint32_t i, n_samples = position->clock.duration;

	for (i = 0; i < impl->sink_info.channels; i++) {
		struct port *p = impl->sink_ports[i];
		float *src, *dst;
		if (p == NULL)
			continue;
		src = pw_filter_get_dsp_buffer(p, n_samples);
		if (src == NULL || p->jack_port == NULL)
			continue;

		dst = jack.port_get_buffer(p->jack_port, n_samples);
		memcpy(dst, src, n_samples * sizeof(float));
	}
	pw_log_trace_fp("done %u", impl->frame_time);
	if (impl->mode & MODE_SINK) {
		impl->done = true;
		jack.cycle_signal(impl->client, 0);
	}
}

static void source_process(void *d, struct spa_io_position *position)
{
	struct impl *impl = d;
	uint32_t i, n_samples = position->clock.duration;

	for (i = 0; i < impl->source_info.channels; i++) {
		struct port *p = impl->source_ports[i];
		float *src, *dst;

		if (p == NULL)
			continue;

		dst = pw_filter_get_dsp_buffer(p, n_samples);
		if (dst == NULL || p->jack_port == NULL)
			continue;

		src = jack.port_get_buffer (p->jack_port, n_samples);
		memcpy(dst, src, n_samples * sizeof(float));
	}
	pw_log_trace_fp("done %u", impl->frame_time);
	if (impl->mode == MODE_SOURCE) {
		impl->done = true;
		jack.cycle_signal(impl->client, 0);
	}
}

static void source_state_changed(void *d, enum pw_filter_state old,
		enum pw_filter_state state, const char *error)
{
	struct impl *impl = d;
	switch (state) {
	case PW_FILTER_STATE_ERROR:
	case PW_FILTER_STATE_UNCONNECTED:
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_FILTER_STATE_PAUSED:
		impl->source_running = false;
		break;
	case PW_FILTER_STATE_STREAMING:
		impl->source_running = true;
		break;
	default:
		break;
	}
}
static void io_changed(void *data, void *port_data, uint32_t id, void *area, uint32_t size)
{
	struct impl *impl = data;
	if (port_data == NULL) {
		switch (id) {
		case SPA_IO_Position:
			impl->position = area;
			break;
		default:
			break;
		}
	}
}

static void param_latency_changed(struct impl *impl, const struct spa_pod *param,
		struct port *port)
{
	struct spa_latency_info latency;
	bool update = false;
	enum spa_direction direction = port->direction;

	if (spa_latency_parse(param, &latency) < 0)
		return;

	if (spa_latency_info_compare(&port->latency[direction], &latency)) {
		port->latency[direction] = latency;
		port->latency_changed[direction] = update = true;
	}
	if (update)
		jack.recompute_total_latencies(impl->client);
}

static void make_sink_ports(struct impl *impl)
{
	uint32_t i;
	struct pw_properties *props;
	const char *str;
	char name[256];
	const char **ports = NULL;

	if (impl->sink_connect)
		ports = jack.get_ports(impl->client, NULL, NULL,
	                                JackPortIsPhysical|JackPortIsInput);

	for (i = 0; i < impl->sink_info.channels; i++) {
		struct port *port = impl->sink_ports[i];
		if (port != NULL) {
			impl->sink_ports[i] = NULL;
			if (port->jack_port)
				jack.port_unregister(impl->client, port->jack_port);
			pw_filter_remove_port(port);
		}

		props = pw_properties_new(
				PW_KEY_FORMAT_DSP, "32 bit float mono audio",
				NULL);
		str = spa_debug_type_find_short_name(spa_type_audio_channel,
				impl->sink_info.position[i]);
		pw_properties_setf(props,
				PW_KEY_AUDIO_CHANNEL, "%s", str ? str : "UNK");
		port = pw_filter_add_port(impl->sink,
                        PW_DIRECTION_INPUT,
                        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                        sizeof(struct port),
			props, NULL, 0);

		if (str)
			snprintf(name, sizeof(name), "output_%s", str);
		else
			snprintf(name, sizeof(name), "output_%d", i);

		port->jack_port = jack.port_register (impl->client, name,
					JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

		impl->sink_ports[i] = port;

		if (ports != NULL && ports[i] != NULL) {
			if (jack.connect(impl->client, jack.port_name(port->jack_port), ports[i]))
				pw_log_warn("cannot connect output ports");
		}
	}
	if (ports)
		jack.free(ports);
}

static void sink_param_changed(void *data, void *port_data, uint32_t id,
		const struct spa_pod *param)
{
	struct impl *impl = data;
	if (port_data != NULL) {
		switch (id) {
		case SPA_PARAM_Latency:
			param_latency_changed(impl, param, port_data);
			break;
		}
	} else {
		switch (id) {
		case SPA_PARAM_PortConfig:
			pw_log_info("PortConfig");
			make_sink_ports(impl);
			break;
		}
	}
}
static void make_source_ports(struct impl *impl)
{
	uint32_t i;
	struct pw_properties *props;
	const char *str;
	char name[256];
	const char **ports = NULL;

	if (impl->source_connect)
		ports = jack.get_ports(impl->client, NULL, NULL,
					JackPortIsPhysical|JackPortIsOutput);

	for (i = 0; i < impl->source_info.channels; i++) {
		struct port *port = impl->source_ports[i];
		if (port != NULL) {
			impl->source_ports[i] = NULL;
			if (port->jack_port)
				jack.port_unregister(impl->client, port->jack_port);
			pw_filter_remove_port(port);
		}

		props = pw_properties_new(
				PW_KEY_FORMAT_DSP, "32 bit float mono audio",
				NULL);
		str = spa_debug_type_find_short_name(spa_type_audio_channel,
				impl->source_info.position[i]);
		pw_properties_setf(props,
				PW_KEY_AUDIO_CHANNEL, "%s", str ? str : "UNK");
		port = pw_filter_add_port(impl->source,
                        PW_DIRECTION_OUTPUT,
                        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                        sizeof(struct port),
			props, NULL, 0);

		if (str)
			snprintf(name, sizeof(name), "input_%s", str);
		else
			snprintf(name, sizeof(name), "input_%d", i);

		port->jack_port = jack.port_register (impl->client, name,
					JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);

		impl->source_ports[i] = port;

		if (ports != NULL && ports[i] != NULL) {
			if (jack.connect(impl->client, ports[i], jack.port_name(port->jack_port)))
				pw_log_warn("cannot connect input ports");
		}
	}
	if (ports)
		jack.free(ports);
}

static void source_param_changed(void *data, void *port_data, uint32_t id,
		const struct spa_pod *param)
{
	struct impl *impl = data;
	if (port_data != NULL) {
		switch (id) {
		case SPA_PARAM_Latency:
			param_latency_changed(impl, param, port_data);
			break;
		}
	} else {
		switch (id) {
		case SPA_PARAM_PortConfig:
			pw_log_info("PortConfig");
			make_source_ports(impl);
			break;
		}
	}
}

static const struct pw_filter_events sink_events = {
	PW_VERSION_FILTER_EVENTS,
	.destroy = sink_destroy,
	.state_changed = sink_state_changed,
	.param_changed = sink_param_changed,
	.io_changed = io_changed,
	.process = sink_process
};

static const struct pw_filter_events source_events = {
	PW_VERSION_FILTER_EVENTS,
	.destroy = source_destroy,
	.state_changed = source_state_changed,
	.param_changed = source_param_changed,
	.io_changed = io_changed,
	.process = source_process,
};

static int create_filters(struct impl *impl)
{
	int res;
	uint32_t n_params;
	const struct spa_pod *params[4];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	struct spa_latency_info latency;

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	spa_zero(latency);

	if (impl->mode & MODE_SINK) {
		impl->sink = pw_filter_new(impl->core,
				"JACK Sink",
				impl->sink_props);
		impl->sink_props = NULL;
		if (impl->sink == NULL)
			return -errno;

		pw_filter_add_listener(impl->sink,
				&impl->sink_listener,
				&sink_events, impl);

		n_params = 0;
		params[n_params++] = spa_format_audio_raw_build(&b,
				SPA_PARAM_EnumFormat, &impl->sink_info);
		params[n_params++] = spa_format_audio_raw_build(&b,
				SPA_PARAM_Format, &impl->sink_info);

		if ((res = pw_filter_connect(impl->sink,
				PW_FILTER_FLAG_DRIVER |
				PW_FILTER_FLAG_RT_PROCESS |
				PW_FILTER_FLAG_CUSTOM_LATENCY,
				params, n_params)) < 0)
			return res;
	}
	if (impl->mode & MODE_SOURCE) {
		impl->source = pw_filter_new(impl->core,
				"JACK Source",
				impl->source_props);
		impl->source_props = NULL;
		if (impl->source == NULL)
			return -errno;

		pw_filter_add_listener(impl->source,
				&impl->source_listener,
				&source_events, impl);

		n_params = 0;
		params[n_params++] = spa_format_audio_raw_build(&b,
				SPA_PARAM_EnumFormat, &impl->source_info);
		params[n_params++] = spa_format_audio_raw_build(&b,
				SPA_PARAM_Format, &impl->source_info);

		if ((res = pw_filter_connect(impl->source,
				PW_FILTER_FLAG_DRIVER |
				PW_FILTER_FLAG_RT_PROCESS |
				PW_FILTER_FLAG_CUSTOM_LATENCY,
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
		nframes = jack.cycle_wait (impl->client);

		source_running = impl->source_running;
		sink_running = impl->sink_running;

		impl->frame_time = jack.frame_time(impl->client);

		pw_log_trace_fp("process %d %u %u %p %d", nframes, source_running,
				sink_running, impl->position, impl->frame_time);

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

			jack.get_cycle_times(impl->client,
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

			jack.transport_query (impl->client, &pos);
		}
		if (impl->mode & MODE_SINK && sink_running) {
			impl->done = false;
			pw_filter_trigger_process(impl->sink);
		} else if (impl->mode == MODE_SOURCE && source_running) {
			impl->done = false;
			pw_filter_trigger_process(impl->source);
		} else {
			jack.cycle_signal(impl->client, 0);
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
	uint32_t i, n_params = 0;
	uint8_t buffer[1024];
	struct spa_pod_builder b;

	if ((impl->mode & MODE_SINK)) {
		for (i = 0; i < impl->sink_info.channels; i++) {
			struct port *port = impl->sink_ports[i];
			if (port == NULL)
				continue;
			spa_pod_builder_init(&b, buffer, sizeof(buffer));
			n_params = 0;
			if (port->latency_changed[SPA_DIRECTION_INPUT]) {
				params[n_params++] = spa_latency_build(&b,
					SPA_PARAM_Latency, &port->latency[SPA_DIRECTION_INPUT]);
				port->latency_changed[SPA_DIRECTION_INPUT] = false;
			}
			if (impl->sink)
				pw_filter_update_params(impl->sink, port, params, n_params);
		}
	}
	if ((impl->mode & MODE_SOURCE)) {
		for (i = 0; i < impl->source_info.channels; i++) {
			struct port *port = impl->source_ports[i];
			if (port == NULL)
				continue;
			spa_pod_builder_init(&b, buffer, sizeof(buffer));
			n_params = 0;
			if (port->latency_changed[SPA_DIRECTION_OUTPUT]) {
				params[n_params++] = spa_latency_build(&b,
					SPA_PARAM_Latency, &port->latency[SPA_DIRECTION_OUTPUT]);
				port->latency_changed[SPA_DIRECTION_OUTPUT] = false;
			}
			if (impl->source)
				pw_filter_update_params(impl->source, port, params, n_params);
		}
	}
	return 0;
}

static void jack_latency(jack_latency_callback_mode_t mode, void *arg)
{
	struct impl *impl = arg;
	uint32_t i;
	struct spa_latency_info latency;
	jack_latency_range_t range;
	bool update = false;

	spa_zero(latency);

	if ((impl->mode & MODE_SINK)) {
		if (mode == JackPlaybackLatency) {
			for (i = 0; i < impl->sink_info.channels; i++) {
				struct port *port = impl->sink_ports[i];
				if (port == NULL || port->jack_port == NULL)
					continue;

				jack.port_get_latency_range(port->jack_port, mode, &range);

				latency.direction = PW_DIRECTION_INPUT;
				latency.min_rate = range.min;
				latency.max_rate = range.max;
				pw_log_debug("port latency %d %d %d", mode, range.min, range.max);

				if (spa_latency_info_compare(&latency, &port->latency[PW_DIRECTION_INPUT])) {
					port->latency[PW_DIRECTION_INPUT] = latency;
					port->latency_changed[PW_DIRECTION_INPUT] = update = true;
				}
			}
		} else if (mode == JackCaptureLatency) {
			for (i = 0; i < impl->sink_info.channels; i++) {
				struct port *port = impl->sink_ports[i];
				if (port == NULL || port->jack_port == NULL)
					continue;
				if (port->latency_changed[PW_DIRECTION_OUTPUT]) {
					range.min = port->latency[PW_DIRECTION_OUTPUT].min_rate;
					range.max = port->latency[PW_DIRECTION_OUTPUT].max_rate;
					jack.port_set_latency_range(port->jack_port, mode, &range);
					port->latency_changed[PW_DIRECTION_OUTPUT] = false;
				}
			}
		}
	}
	if ((impl->mode & MODE_SOURCE) && mode == JackCaptureLatency) {
		if (mode == JackCaptureLatency) {
			for (i = 0; i < impl->source_info.channels; i++) {
				struct port *port = impl->source_ports[i];
				if (port == NULL || port->jack_port == NULL)
					continue;
				jack.port_get_latency_range(port->jack_port, mode, &range);

				latency.direction = PW_DIRECTION_OUTPUT;
				latency.min_rate = range.min;
				latency.max_rate = range.max;
				pw_log_debug("port latency %d %d %d", mode, range.min, range.max);

				if (spa_latency_info_compare(&latency, &port->latency[PW_DIRECTION_OUTPUT])) {
					port->latency[PW_DIRECTION_OUTPUT] = latency;
					port->latency_changed[PW_DIRECTION_OUTPUT] = update = true;
				}
			}
		} else if (mode == JackPlaybackLatency) {
			for (i = 0; i < impl->sink_info.channels; i++) {
				struct port *port = impl->sink_ports[i];
				if (port == NULL || port->jack_port == NULL)
					continue;
				if (port->latency_changed[PW_DIRECTION_INPUT]) {
					range.min = port->latency[PW_DIRECTION_INPUT].min_rate;
					range.max = port->latency[PW_DIRECTION_INPUT].max_rate;
					jack.port_set_latency_range(port->jack_port, mode, &range);
					port->latency_changed[PW_DIRECTION_INPUT] = false;
				}
			}
		}
	}
	if (update)
		pw_loop_invoke(impl->main_loop, do_update_latency, 0, NULL, 0, false, impl);
}

static int create_jack_client(struct impl *impl)
{
	const char *server_name, *client_name;
	jack_options_t options = JackNullOption;
	jack_status_t status;

	server_name = pw_properties_get(impl->props, "jack.server");
	if (server_name != NULL)
		options |= JackServerName;

	client_name = pw_properties_get(impl->props, "jack.client-name");
	if (client_name == NULL)
		client_name = DEFAULT_CLIENT_NAME;

	impl->client = jack.client_open(client_name, options, &status, server_name);
	if (impl->client == NULL) {
		pw_log_error ("jack_client_open() failed 0x%2.0x\n", status);
		return -EIO;
	}
	jack.on_info_shutdown(impl->client, jack_info_shutdown, impl);
	jack.set_process_thread(impl->client, jack_process_thread, impl);
	jack.set_xrun_callback(impl->client, jack_xrun, impl);
	jack.set_latency_callback(impl->client, jack_latency, impl);

	impl->samplerate = jack.get_sample_rate(impl->client);
	impl->source_info.rate = impl->samplerate;
	impl->sink_info.rate = impl->samplerate;

	return 0;
}

static int start_jack_clients(struct impl *impl)
{
	jack.activate(impl->client);
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
		jack.deactivate(impl->client);
		jack.client_close(impl->client);
	}
	if (impl->source)
		pw_filter_destroy(impl->source);
	if (impl->sink)
		pw_filter_destroy(impl->sink);
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

	if ((str = pw_properties_get(props, "jack.library")) == NULL)
		str = "libjack.so.0";

	if ((res = weakjack_load(&jack, str)) < 0) {
		pw_log_error( "can't load '%s': %s", str, spa_strerror(res));
		goto error;
	}

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

	pw_properties_set(impl->source_props, PW_KEY_MEDIA_CLASS, "Audio/Source");
	pw_properties_set(impl->source_props, PW_KEY_PRIORITY_DRIVER, "30000");
	pw_properties_set(impl->source_props, PW_KEY_NODE_NAME, "jack_source");
	pw_properties_set(impl->source_props, PW_KEY_NODE_DESCRIPTION, "JACK Source");

	if ((str = pw_properties_get(props, "sink.props")) != NULL)
		pw_properties_update_string(impl->sink_props, str, strlen(str));
	if ((str = pw_properties_get(props, "source.props")) != NULL)
		pw_properties_update_string(impl->source_props, str, strlen(str));

	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_ALWAYS_PROCESS);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, "jack.connect");

	parse_audio_info(impl->source_props, &impl->source_info);
	parse_audio_info(impl->sink_props, &impl->sink_info);

	impl->source_connect = pw_properties_get_bool(impl->source_props,
			"jack.connect", true);
	impl->sink_connect = pw_properties_get_bool(impl->sink_props,
			"jack.connect", true);

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

	if ((res = create_filters(impl)) < 0)
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
