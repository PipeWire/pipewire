/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

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
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <net/if.h>
#include <math.h>

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/debug/types.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-json.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/raw.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>

#include "module-netjack2/packets.h"
#include "module-netjack2/peer.c"
#include "network-utils.h"

#ifndef IPTOS_DSCP
#define IPTOS_DSCP_MASK 0xfc
#define IPTOS_DSCP(x) ((x) & IPTOS_DSCP_MASK)
#endif

/** \page page_module_netjack2_driver Netjack2 driver
 *
 * The netjack2-driver module provides a source or sink that is following a
 * netjack2 manager. It is meant to be used over stable (ethernet) network
 * connections with minimal latency and jitter.
 *
 * The driver normally decides how many ports it will send and receive from the
 * manager. By default however, these values are set to -1 so that the manager
 * decides on the number of ports.
 *
 * With the global or per stream audio.port and midi.ports properties this
 * behaviour can be adjusted.
 *
 * The driver will send out UDP messages on a (typically) multicast address to
 * inform the manager of the available driver. This will then instruct the manager
 * to configure and start the driver.
 *
 * On the driver side, a sink and/or source with the specified numner of audio and
 * midi ports will be created. On the manager side there will be a corresponding
 * source and/or sink created respectively.
 *
 * The driver will be scheduled with exactly the same period as the manager but with
 * a configurable number of periods of delay (see netjack2.latency, default 2).
 *
 * ## Module Name
 *
 * `libpipewire-module-netjack2-driver`
 *
 * ## Module Options
 *
 * - `driver.mode`: the driver mode, sink|source|duplex, default duplex. This set the
 *    per stream audio.port and midi.ports default from -1 to 0. sink mode defaults to
 *    no source ports, source mode to no sink ports and duplex leaves the defaults as
 *    they are.
 * - `local.ifname = <str>`: interface name to use
 * - `net.ip =<str>`: multicast IP address, default "225.3.19.154"
 * - `net.port =<int>`: control port, default 19000
 * - `net.mtu = <int>`: MTU to use, default 1500
 * - `net.ttl = <int>`: TTL to use, default 1
 * - `net.loop = <bool>`: loopback multicast, default false
 * - `source.ip =<str>`: IP address to bind to, default "0.0.0.0"
 * - `source.port =<int>`: port to bind to, default 0 (allocate)
 * - `netjack2.client-name`: the name of the NETJACK2 client.
 * - `netjack2.latency`: the latency in cycles, default 2
 * - `audio.ports`: the number of audio ports. Can also be added to the stream props.
 *      A value of -1 will configure to the number of audio ports on the manager.
 * - `midi.ports`: the number of midi ports. Can also be added to the stream props.
 *      A value of -1 will configure to the number of midi ports on the manager.
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
 * # ~/.config/pipewire/pipewire.conf.d/my-netjack2-driver.conf
 *
 * context.modules = [
 * {   name = libpipewire-module-netjack2-driver
 *     args = {
 *         #netjack2.client-name = PipeWire
 *         #netjack2.latency     = 2
 *         #midi.ports           = 0
 *         #audio.ports          = -1
 *         #audio.channels       = 2
 *         #audio.position       = [ FL FR ]
 *         source.props = {
 *             # extra source properties
 *         }
 *         sink.props = {
 *             # extra sink properties
 *         }
 *     }
 * }
 * ]
 *\endcode
 */

#define NAME "netjack2-driver"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MAX_PORTS	128

#define DEFAULT_NET_IP		"225.3.19.154"
#define DEFAULT_NET_PORT	19000
#define DEFAULT_NET_TTL		1
#define DEFAULT_NET_MTU		1500
#define DEFAULT_NET_LOOP	false
#define DEFAULT_NET_DSCP	34 /* Default to AES-67 AF41 (34) */
#define MAX_MTU			9000
#define DEFAULT_SOURCE_IP	"0.0.0.0"
#define DEFAULT_SOURCE_PORT	0

#define DEFAULT_NETWORK_LATENCY	2
#define NETWORK_MAX_LATENCY	30

#define DEFAULT_CLIENT_NAME	"PipeWire"
#define DEFAULT_MIDI_PORTS	-1
#define DEFAULT_AUDIO_PORTS	-1

#define FOLLOWER_INIT_TIMEOUT	1
#define FOLLOWER_INIT_RETRY	-1

#define MODULE_USAGE	"( remote.name=<remote> ) "				\
			"( driver.mode=<sink|source|duplex> ) "                 \
			"( local.ifname=<interface name> ) "			\
			"( net.ip=<ip address to use, default 225.3.19.154> ) "	\
			"( net.port=<port to use, default 19000> ) "		\
			"( net.mtu=<MTU to use, default 1500> ) "		\
			"( net.ttl=<TTL to use, default 1> ) "			\
			"( net.loop=<loopback, default false> ) "		\
			"( source.ip=<ip address to bind, default 0.0.0.0> ) "	\
			"( source.port=<port to bind, default 0> ) "		\
			"( netjack2.client-name=<name of the NETJACK2 client> ) "	\
			"( netjack2.latency=<latency in cycles, default 2> ) "	\
			"( audio.ports=<number of midi ports, default -1> ) "	\
			"( midi.ports=<number of midi ports, default -1> ) "	\
			"( audio.channels=<number of channels, default 0> ) "	\
			"( audio.position=<channel map, default null> ) "	\
			"( source.props=<properties> ) "			\
			"( sink.props=<properties> ) "


static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create a netjack2 driver" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct port {
	enum spa_direction direction;
	struct spa_latency_info latency[2];
	bool latency_changed[2];
	unsigned int is_midi:1;
};

struct stream {
	struct impl *impl;

	enum spa_direction direction;
	struct pw_properties *props;
	struct pw_filter *filter;
	struct spa_hook listener;

	int32_t wanted_n_midi;
	int32_t wanted_n_audio;

	struct spa_io_position *position;

	struct spa_audio_info_raw info;

	uint32_t n_ports;
	struct port *ports[MAX_PORTS];

	struct volume volume;

	uint32_t active_audio_ports;
	uint32_t active_midi_ports;

	unsigned int running:1;
};

struct impl {
	struct pw_context *context;
	struct pw_loop *main_loop;
	struct pw_loop *data_loop;
	struct spa_system *system;
	struct pw_timer_queue *timer_queue;

#define MODE_SINK	(1<<0)
#define MODE_SOURCE	(1<<1)
#define MODE_DUPLEX	(MODE_SINK|MODE_SOURCE)
	uint32_t mode;
	struct pw_properties *props;

	bool loop;
	int ttl;
	int dscp;
	int mtu;
	uint32_t latency;
	uint32_t quantum_limit;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct stream source;
	struct stream sink;

	uint32_t period_size;
	uint32_t samplerate;
	uint64_t frame_time;

	uint32_t pw_xrun;
	uint32_t nj2_xrun;

	struct sockaddr_storage dst_addr;
	socklen_t dst_len;
	struct sockaddr_storage src_addr;
	socklen_t src_len;

	struct spa_source *setup_socket;
	struct spa_source *socket;
	struct pw_timer timer;
	int32_t init_retry;

	struct netjack2_peer peer;

	uint32_t driving;
	uint32_t received;

	unsigned int triggered:1;
	unsigned int do_disconnect:1;
	unsigned int done:1;
	unsigned int new_xrun:1;
	unsigned int started:1;
};

static void reset_volume(struct volume *vol, uint32_t n_volumes)
{
	uint32_t i;
	vol->mute = false;
	vol->n_volumes = n_volumes;
	for (i = 0; i < n_volumes; i++)
		vol->volumes[i] = 1.0f;
}

static void stream_destroy(void *d)
{
	struct stream *s = d;
	uint32_t i;

	spa_hook_remove(&s->listener);
	for (i = 0; i < s->n_ports; i++)
		s->ports[i] = NULL;
	s->filter = NULL;
}

static void stream_state_changed(void *d, enum pw_filter_state old,
		enum pw_filter_state state, const char *error)
{
	struct stream *s = d;
	struct impl *impl = s->impl;
	switch (state) {
	case PW_FILTER_STATE_ERROR:
		pw_log_warn("stream %p: error: %s", s, error);
		break;
	case PW_FILTER_STATE_UNCONNECTED:
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_FILTER_STATE_PAUSED:
		s->running = false;
		break;
	case PW_FILTER_STATE_STREAMING:
		s->running = true;
		break;
	default:
		break;
	}
}

static inline void set_info(struct stream *s, uint32_t nframes,
		struct data_info *midi, uint32_t *n_midi,
		struct data_info *audio, uint32_t *n_audio)
{
	uint32_t i, n_m, n_a;
	n_m = n_a = 0;
	for (i = 0; i < s->n_ports; i++) {
		struct port *p = s->ports[i];
		void *data = p ? pw_filter_get_dsp_buffer(p, nframes) : NULL;
		if (p && p->is_midi) {
			midi[n_m].data = data;
			midi[n_m].id = i;
			midi[n_m++].filled = false;
		} else if (data != NULL) {
			audio[n_a].data = data;
			audio[n_a].id = i;
			audio[n_a++].filled = false;
		}
	}
	*n_midi = n_m;
	*n_audio = n_a;
}

static void sink_process(void *d, struct spa_io_position *position)
{
	struct stream *s = d;
	struct impl *impl = s->impl;
	uint32_t nframes = position->clock.duration;
	struct data_info midi[s->n_ports];
	struct data_info audio[s->n_ports];
	uint32_t n_midi, n_audio;

	if (impl->driving == MODE_SINK && impl->triggered) {
		impl->triggered = false;
		return;
	}

	set_info(s, nframes, midi, &n_midi, audio, &n_audio);

	netjack2_send_data(&impl->peer, nframes, midi, n_midi, audio, n_audio);

	pw_log_trace_fp("done %"PRIu64, impl->frame_time);
	if (impl->driving == MODE_SINK)
		impl->done = true;
}

static void source_process(void *d, struct spa_io_position *position)
{
	struct stream *s = d;
	struct impl *impl = s->impl;
	uint32_t nframes = position->clock.duration;
	struct data_info midi[s->n_ports];
	struct data_info audio[s->n_ports];
	uint32_t n_midi, n_audio;

	if (impl->driving == MODE_SOURCE && !impl->triggered) {
		pw_log_trace_fp("done %"PRIu64, impl->frame_time);
		impl->done = true;
		return;
	}
	impl->triggered = false;

	set_info(s, nframes, midi, &n_midi, audio, &n_audio);

	netjack2_recv_data(&impl->peer, midi, n_midi, audio, n_audio);
}

static void stream_io_changed(void *data, void *port_data, uint32_t id, void *area, uint32_t size)
{
	struct stream *s = data;
	if (port_data == NULL) {
		switch (id) {
		case SPA_IO_Position:
			s->position = area;
			break;
		default:
			break;
		}
	}
}

static void param_latency_changed(struct stream *s, const struct spa_pod *param,
		struct port *port)
{
	struct spa_latency_info latency;
	bool update = false;
	enum spa_direction direction = port->direction;

	if (param == NULL || spa_latency_parse(param, &latency) < 0)
		return;

	if (spa_latency_info_compare(&port->latency[direction], &latency)) {
		port->latency[direction] = latency;
		port->latency_changed[direction] = update = true;
	}
}

static void make_stream_ports(struct stream *s)
{
	struct impl *impl = s->impl;
	uint32_t i;
	struct pw_properties *props;
	const char *str;
	char name[256];
	bool is_midi;
	uint8_t buffer[512];
	struct spa_pod_builder b;
	struct spa_latency_info latency;
	const struct spa_pod *params[1];

	for (i = 0; i < s->n_ports; i++) {
		struct port *port = s->ports[i];

		if (port != NULL) {
			s->ports[i] = NULL;
			pw_filter_remove_port(port);
		}

		if (i < s->info.channels) {
			str = spa_debug_type_find_short_name(spa_type_audio_channel,
					s->info.position[i % SPA_AUDIO_MAX_CHANNELS]);
			props = pw_properties_new(
					PW_KEY_FORMAT_DSP, "32 bit float mono audio",
					PW_KEY_AUDIO_CHANNEL, str ? str : "UNK",
					PW_KEY_PORT_PHYSICAL, "true",
					NULL);

			is_midi = false;
		} else {
			snprintf(name, sizeof(name), "midi%d", i - s->info.channels);
			props = pw_properties_new(
					PW_KEY_FORMAT_DSP, "8 bit raw midi",
					PW_KEY_AUDIO_CHANNEL, name,
					PW_KEY_PORT_PHYSICAL, "true",
					NULL);

			is_midi = true;
		}
		latency = SPA_LATENCY_INFO(s->direction,
				.min_quantum = impl->latency,
				.max_quantum = impl->latency);
		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		params[0] = spa_latency_build(&b, SPA_PARAM_Latency, &latency);

		port = pw_filter_add_port(s->filter,
                        s->direction,
                        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                        sizeof(struct port),
			props, params, 1);
		if (port == NULL) {
			pw_log_error("Can't create port: %m");
			return;
		}
		port->latency[s->direction] = latency;
		port->is_midi = is_midi;

		s->ports[i] = port;
	}
	pw_filter_set_active(s->filter, true);
}

static struct spa_pod *make_props_param(struct spa_pod_builder *b,
		struct volume *vol)
{
	return spa_pod_builder_add_object(b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
			SPA_PROP_mute, SPA_POD_Bool(vol->mute),
			SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float),
				SPA_TYPE_Float, vol->n_volumes, vol->volumes));
}

static void parse_props(struct stream *s, const struct spa_pod *param)
{
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	struct spa_pod_prop *prop;
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	const struct spa_pod *params[1];

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_mute:
		{
			bool mute;
			if (spa_pod_get_bool(&prop->value, &mute) == 0)
				s->volume.mute = mute;
			break;
		}
		case SPA_PROP_channelVolumes:
		{
			uint32_t n;
			float vols[MAX_CHANNELS];
			if ((n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					vols, SPA_AUDIO_MAX_CHANNELS)) > 0) {
				s->volume.n_volumes = n;
				for (n = 0; n < s->volume.n_volumes; n++)
					s->volume.volumes[n] = vols[n];
			}
			break;
		}
		default:
			break;
		}
	}
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[0] = make_props_param(&b, &s->volume);

	pw_filter_update_params(s->filter, NULL, params, 1);
}

static void stream_param_changed(void *data, void *port_data, uint32_t id,
		const struct spa_pod *param)
{
	struct stream *s = data;
	if (port_data != NULL) {
		switch (id) {
		case SPA_PARAM_Latency:
			param_latency_changed(s, param, port_data);
			break;
		}
	} else {
		switch (id) {
		case SPA_PARAM_PortConfig:
			pw_log_debug("PortConfig");
			make_stream_ports(s);
			break;
		case SPA_PARAM_Props:
			pw_log_debug("Props");
			parse_props(s, param);
			break;
		}
	}
}

static const struct pw_filter_events sink_events = {
	PW_VERSION_FILTER_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.param_changed = stream_param_changed,
	.io_changed = stream_io_changed,
	.process = sink_process
};

static const struct pw_filter_events source_events = {
	PW_VERSION_FILTER_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.param_changed = stream_param_changed,
	.io_changed = stream_io_changed,
	.process = source_process,
};

static int make_stream(struct stream *s, const char *name)
{
	struct impl *impl = s->impl;
	uint32_t n_params;
	const struct spa_pod *params[4];
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	int res;

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	s->filter = pw_filter_new(impl->core, name, pw_properties_copy(s->props));
	if (s->filter == NULL)
		return -errno;

	if (s->direction == PW_DIRECTION_INPUT) {
		pw_filter_add_listener(s->filter, &s->listener,
				&sink_events, s);
	} else {
		pw_filter_add_listener(s->filter, &s->listener,
				&source_events, s);
	}

	reset_volume(&s->volume, s->info.channels);

	n_params = 0;
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &s->info);
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_Format, &s->info);
	params[n_params++] = make_props_param(&b, &s->volume);

	if ((res = pw_filter_connect(s->filter,
			PW_FILTER_FLAG_INACTIVE |
			PW_FILTER_FLAG_DRIVER |
			PW_FILTER_FLAG_RT_PROCESS |
			PW_FILTER_FLAG_CUSTOM_LATENCY,
			params, n_params)) < 0)
		return res;

	if (s->info.channels == 0)
		make_stream_ports(s);

	return res;
}

static int create_filters(struct impl *impl)
{
	int res = 0;

	if (impl->mode & MODE_SINK)
		res = make_stream(&impl->sink, "NETJACK2 Sink");

	if (impl->mode & MODE_SOURCE)
		res = make_stream(&impl->source, "NETJACK2 Source");

	return res;
}

static inline uint64_t get_time_nsec(struct impl *impl)
{
	uint64_t nsec;
	if (impl->sink.filter)
		nsec = pw_filter_get_nsec(impl->sink.filter);
	else if (impl->source.filter)
		nsec = pw_filter_get_nsec(impl->source.filter);
	else
		nsec = 0;
	return nsec;
}

static void update_clock(struct impl *impl, struct stream *s, uint64_t nsec, uint32_t nframes)
{
	if (s->position) {
		struct spa_io_clock *c = &s->position->clock;

		c->nsec = nsec;
		c->rate = SPA_FRACTION(1, impl->samplerate);
		c->position = impl->frame_time;
		c->duration = nframes;
		c->delay = 0;
		c->rate_diff = 1.0;
		c->next_nsec = nsec;

		c->target_rate = c->rate;
		c->target_duration = c->duration;
	}
}

static void
on_data_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_warn("error:%08x", mask);
		pw_loop_update_io(impl->data_loop, impl->socket, 0);
		return;
	}
	if (mask & SPA_IO_IN) {
		bool source_running, sink_running;
		uint32_t nframes;
		uint64_t nsec;

		nframes = netjack2_driver_sync_wait(&impl->peer);
		if (nframes == 0)
			return;

		nsec = get_time_nsec(impl);

		if (!impl->done) {
			impl->pw_xrun++;
			impl->new_xrun = true;
		}
		impl->received++;

		source_running = impl->source.running;
		sink_running = impl->sink.running;

		impl->frame_time += nframes;

		pw_log_trace_fp("process %d %u %u %"PRIu64, nframes, source_running,
				sink_running, impl->frame_time);

		if (impl->new_xrun) {
			pw_log_warn("Xrun netjack2:%u PipeWire:%u", impl->nj2_xrun, impl->pw_xrun);
			impl->new_xrun = false;
		}
		if (!source_running)
			netjack2_recv_data(&impl->peer, NULL, 0, NULL, 0);

		if (impl->mode & MODE_SOURCE && source_running) {
			impl->done = false;
			impl->triggered = true;
			impl->driving = MODE_SOURCE;
			update_clock(impl, &impl->source, nsec, nframes);
			if (pw_filter_trigger_process(impl->source.filter) < 0)
				pw_log_warn("source not ready");
		} else if (impl->mode == MODE_SINK && sink_running) {
			impl->done = false;
			impl->triggered = true;
			impl->driving = MODE_SINK;
			update_clock(impl, &impl->sink, nsec, nframes);
			if (pw_filter_trigger_process(impl->sink.filter) < 0)
				pw_log_warn("sink not ready");
		} else {
			sink_running = false;
			impl->done = true;
		}
		if (!sink_running)
			netjack2_send_data(&impl->peer, nframes, NULL, 0, NULL, 0);
	}
}

static bool is_multicast(struct sockaddr *sa, socklen_t salen)
{
	if (sa->sa_family == AF_INET) {
		static const uint32_t ipv4_mcast_mask = 0xe0000000;
		struct sockaddr_in *sa4 = (struct sockaddr_in*)sa;
		return (ntohl(sa4->sin_addr.s_addr) & ipv4_mcast_mask) == ipv4_mcast_mask;
	} else if (sa->sa_family == AF_INET6) {
		struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)sa;
		return sa6->sin6_addr.s6_addr[0] == 0xff;
	}
	return false;
}

static int make_socket(struct sockaddr_storage *src, socklen_t src_len,
		struct sockaddr_storage *dst, socklen_t dst_len,
		bool loop, int ttl, int dscp, const char *ifname)
{
	int af, fd, val, res;
	struct timeval timeout;

	af = src->ss_family;
	if ((fd = socket(af, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0) {
		pw_log_error("socket failed: %m");
		return -errno;
	}
	val = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
		res = -errno;
		pw_log_error("setsockopt failed: %m");
		goto error;
	}
#ifdef SO_BINDTODEVICE
	if (ifname && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname)) < 0) {
		res = -errno;
		pw_log_error("setsockopt(SO_BINDTODEVICE) failed: %m");
		goto error;
	}
#endif
#ifdef SO_PRIORITY
	val = 6;
	if (setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &val, sizeof(val)) < 0)
		pw_log_warn("setsockopt(SO_PRIORITY) failed: %m");
#endif
	timeout.tv_sec = 2;
	timeout.tv_usec = 0;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
		pw_log_warn("setsockopt(SO_RCVTIMEO) failed: %m");

	if (dscp > 0) {
		val = IPTOS_DSCP(dscp << 2);
		if (setsockopt(fd, IPPROTO_IP, IP_TOS, &val, sizeof(val)) < 0)
			pw_log_warn("setsockopt(IP_TOS) failed: %m");
	}
	if (bind(fd, (struct sockaddr*)src, src_len) < 0) {
		res = -errno;
		pw_log_error("bind() failed: %m");
		goto error;
	}
	if (is_multicast((struct sockaddr*)dst, dst_len)) {
		val = loop;
		if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &val, sizeof(val)) < 0)
			pw_log_warn("setsockopt(IP_MULTICAST_LOOP) failed: %m");

		val = ttl;
		if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &val, sizeof(val)) < 0)
			pw_log_warn("setsockopt(IP_MULTICAST_TTL) failed: %m");
	}

	return fd;
error:
	close(fd);
	return res;
}

static void on_timer_event(void *data);

static void update_timer(struct impl *impl, uint64_t timeout)
{
	pw_timer_queue_cancel(&impl->timer);
	pw_timer_queue_add(impl->timer_queue, &impl->timer,
			NULL, timeout * SPA_NSEC_PER_SEC,
			on_timer_event, impl);
}

static bool encoding_supported(uint32_t encoder)
{
	switch (encoder) {
	case NJ2_ENCODER_FLOAT:
	case NJ2_ENCODER_INT:
		return true;
#ifdef HAVE_OPUS
	case NJ2_ENCODER_OPUS:
		return true;
#endif
	}
	return false;
}

static int handle_follower_setup(struct impl *impl, struct nj2_session_params *params,
	struct sockaddr_storage *addr, socklen_t addr_len)
{
	int res;
	struct netjack2_peer *peer = &impl->peer;
	uint32_t i;
	const char *media;

	pw_log_info("got follower setup");
	nj2_dump_session_params(params);

	nj2_session_params_ntoh(&peer->params, params);

	if (peer->params.send_audio_channels < 0 ||
	    peer->params.recv_audio_channels < 0 ||
	    peer->params.send_midi_channels < 0 ||
	    peer->params.recv_midi_channels < 0 ||
	    peer->params.sample_rate == 0 ||
	    peer->params.period_size == 0 ||
	    !encoding_supported(peer->params.sample_encoder)) {
		pw_log_warn("invalid follower setup");
		return -EINVAL;
	}
	/* the params are from the perspective of the manager, so send is our
	 * receive (source) and recv is our send (sink) */
	SPA_SWAP(peer->params.send_audio_channels, peer->params.recv_audio_channels);
	SPA_SWAP(peer->params.send_midi_channels, peer->params.recv_midi_channels);

	pw_loop_update_io(impl->main_loop, impl->setup_socket, 0);

	impl->sink.n_ports = peer->params.send_audio_channels + peer->params.send_midi_channels;
	if (impl->sink.n_ports > MAX_PORTS) {
		pw_log_warn("Too many follower sink ports %d > %d", impl->sink.n_ports, MAX_PORTS);
		return -EINVAL;
	}
	impl->sink.info.rate =  peer->params.sample_rate;
	if ((uint32_t)peer->params.send_audio_channels != impl->sink.info.channels) {
		impl->sink.info.channels = SPA_MIN(peer->params.send_audio_channels, (int)SPA_AUDIO_MAX_CHANNELS);
		for (i = 0; i < impl->sink.info.channels; i++)
			impl->sink.info.position[i] = SPA_AUDIO_CHANNEL_AUX0 + i;
	}
	impl->source.n_ports = peer->params.recv_audio_channels + peer->params.recv_midi_channels;
	if (impl->source.n_ports > MAX_PORTS) {
		pw_log_warn("Too many follower source ports %d > %d", impl->source.n_ports, MAX_PORTS);
		return -EINVAL;
	}
	impl->source.info.rate =  peer->params.sample_rate;
	if ((uint32_t)peer->params.recv_audio_channels != impl->source.info.channels) {
		impl->source.info.channels = SPA_MIN(peer->params.recv_audio_channels, (int)SPA_AUDIO_MAX_CHANNELS);
		for (i = 0; i < impl->source.info.channels; i++)
			impl->source.info.position[i] = SPA_AUDIO_CHANNEL_AUX0 + i;
	}
	impl->samplerate = peer->params.sample_rate;
	impl->period_size = peer->params.period_size;

	pw_properties_setf(impl->sink.props, PW_KEY_NODE_DESCRIPTION, "NETJACK2 to %s",
			peer->params.driver_name);
	pw_properties_setf(impl->source.props, PW_KEY_NODE_DESCRIPTION, "NETJACK2 from %s",
			peer->params.driver_name);

	pw_properties_setf(impl->sink.props, PW_KEY_NODE_RATE,
			"1/%u", impl->samplerate);
	pw_properties_set(impl->sink.props, PW_KEY_NODE_FORCE_RATE, "0");
	pw_properties_setf(impl->sink.props, PW_KEY_NODE_FORCE_QUANTUM,
			"%u", impl->period_size);
	pw_properties_setf(impl->source.props, PW_KEY_NODE_RATE,
			"1/%u", impl->samplerate);
	pw_properties_set(impl->source.props, PW_KEY_NODE_FORCE_RATE, "0");
	pw_properties_setf(impl->source.props, PW_KEY_NODE_FORCE_QUANTUM,
			"%u", impl->period_size);

	media = impl->sink.info.channels > 0 ? "Audio" : "Midi";
	if (pw_properties_get(impl->sink.props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_setf(impl->sink.props, PW_KEY_MEDIA_CLASS, "%s/Sink", media);

	media = impl->source.info.channels > 0 ? "Audio" : "Midi";
	if (pw_properties_get(impl->source.props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_setf(impl->source.props, PW_KEY_MEDIA_CLASS, "%s/Source", media);

	impl->mode = 0;
	if (impl->source.n_ports > 0)
		impl->mode |= MODE_SOURCE;
	if (impl->sink.n_ports > 0)
		impl->mode |= MODE_SINK;

	if ((res = create_filters(impl)) < 0)
		return res;

	peer->fd = impl->socket->fd;
	peer->our_stream = 'r';
	peer->other_stream = 's';
	peer->send_volume = &impl->sink.volume;
	peer->recv_volume = &impl->source.volume;
	peer->quantum_limit = impl->quantum_limit;
	netjack2_init(peer);

	int bufsize = NETWORK_MAX_LATENCY * (peer->params.mtu +
		peer->params.period_size * sizeof(float) *
		SPA_MAX(impl->source.n_ports, impl->sink.n_ports));

	pw_log_info("send/recv buffer %d", bufsize);
	if (setsockopt(impl->socket->fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize)) < 0)
		pw_log_warn("setsockopt(SO_SNDBUF) failed: %m");
	if (setsockopt(impl->socket->fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize)) < 0)
		pw_log_warn("setsockopt(SO_SNDBUF) failed: %m");

	if (connect(impl->socket->fd, (struct sockaddr*)addr, addr_len) < 0)
		goto connect_error;

	impl->started = true;
	params->packet_id = htonl(NJ2_ID_START_DRIVER);
	send(impl->socket->fd, params, sizeof(*params), 0);

	impl->done = true;
	pw_loop_update_io(impl->data_loop, impl->socket, SPA_IO_IN);

	return 0;
connect_error:
	pw_log_error("connect() failed: %m");
	return -errno;
}

static void
on_socket_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;

	if (mask & SPA_IO_IN) {
		struct sockaddr_storage addr;
		socklen_t addr_len = sizeof(addr);
		ssize_t len;
		struct nj2_session_params params;

		if ((len = recvfrom(fd, &params, sizeof(params), 0,
				(struct sockaddr *)&addr, &addr_len)) < 0)
			goto receive_error;

		if (len < (int)sizeof(struct nj2_session_params))
			goto short_packet;

		if (strncmp(params.type, "params", sizeof(params.type)) != 0)
			goto wrong_type;

		switch(ntohl(params.packet_id)) {
		case NJ2_ID_FOLLOWER_SETUP:
			handle_follower_setup(impl, &params, &addr, addr_len);
			break;
		}
	}
	return;

receive_error:
	pw_log_warn("recv error: %m");
	return;
short_packet:
	pw_log_warn("short packet received");
	return;
wrong_type:
	pw_log_warn("wrong packet type received");
	return;
}

static int send_follower_available(struct impl *impl)
{
	char buffer[256];
	struct nj2_session_params params;
	const char *client_name;

	pw_loop_update_io(impl->main_loop, impl->setup_socket, SPA_IO_IN);

	pw_log_info("sending AVAILABLE to %s", pw_net_get_ip_fmt(&impl->dst_addr, buffer, sizeof(buffer)));

	client_name = pw_properties_get(impl->props, "netjack2.client-name");
	if (client_name == NULL)
		client_name = DEFAULT_CLIENT_NAME;

	spa_zero(params);
	strcpy(params.type, "params");
	params.version = htonl(NJ2_NETWORK_PROTOCOL);
	params.packet_id = htonl(NJ2_ID_FOLLOWER_AVAILABLE);
	snprintf(params.name, sizeof(params.name), "%s", client_name);
	snprintf(params.follower_name, sizeof(params.follower_name), "%s", pw_get_host_name());
	params.mtu = htonl(impl->mtu);
	params.transport_sync = htonl(0);
	/* send/recv is from the perspective of the manager, so what we send (sink)
	 * is recv on the manager and vice versa. */
	params.recv_audio_channels = htonl(impl->sink.wanted_n_audio);
	params.send_audio_channels = htonl(impl->source.wanted_n_audio);
	params.recv_midi_channels = htonl(impl->sink.wanted_n_midi);
	params.send_midi_channels = htonl(impl->source.wanted_n_midi);
	params.sample_encoder = htonl(NJ2_ENCODER_FLOAT);
	params.follower_sync_mode = htonl(1);
        params.network_latency = htonl(impl->latency);
	sendto(impl->setup_socket->fd, &params, sizeof(params), 0,
			(struct sockaddr*)&impl->dst_addr, impl->dst_len);
	return 0;
}

static int create_netjack2_socket(struct impl *impl)
{
	const char *str;
	uint32_t port;
	int fd, res;

	port = pw_properties_get_uint32(impl->props, "net.port", 0);
	if (port == 0)
		port = DEFAULT_NET_PORT;
	if ((str = pw_properties_get(impl->props, "net.ip")) == NULL)
		str = DEFAULT_NET_IP;
	if ((res = pw_net_parse_address(str, port, &impl->dst_addr, &impl->dst_len)) < 0) {
		pw_log_error("invalid net.ip:%s port:%d: %s", str, port, spa_strerror(res));
		goto out;
	}

	port = pw_properties_get_uint32(impl->props, "source.port", DEFAULT_SOURCE_PORT);
	if ((str = pw_properties_get(impl->props, "source.ip")) == NULL)
		str = DEFAULT_SOURCE_IP;
	if ((res = pw_net_parse_address(str, port, &impl->src_addr, &impl->src_len)) < 0) {
		pw_log_error("invalid source.ip:%s port:%d: %s", str, port, spa_strerror(res));
		goto out;
	}

	impl->mtu = pw_properties_get_uint32(impl->props, "net.mtu", DEFAULT_NET_MTU);
	impl->ttl = pw_properties_get_uint32(impl->props, "net.ttl", DEFAULT_NET_TTL);
	impl->loop = pw_properties_get_bool(impl->props, "net.loop", DEFAULT_NET_LOOP);
	impl->dscp = pw_properties_get_uint32(impl->props, "net.dscp", DEFAULT_NET_DSCP);
	str = pw_properties_get(impl->props, "local.ifname");

	fd = make_socket(&impl->src_addr, impl->src_len,
			&impl->dst_addr, impl->dst_len, impl->loop, impl->ttl, impl->dscp,
			str);
	if (fd < 0) {
		res = -errno;
		pw_log_error("can't create socket: %s", spa_strerror(res));
		goto out;
	}

	impl->setup_socket = pw_loop_add_io(impl->main_loop, fd,
				0, true, on_socket_io, impl);
	if (impl->setup_socket == NULL) {
		res = -errno;
		pw_log_error("can't create setup source: %m");
		close(fd);
		goto out;
	}

	impl->socket = pw_loop_add_io(impl->data_loop, fd,
			0, false, on_data_io, impl);
	if (impl->socket == NULL) {
		res = -errno;
		pw_log_error("can't create data source: %m");
		goto out;
	}

	impl->init_retry = -1;
	update_timer(impl, FOLLOWER_INIT_TIMEOUT);

	return 0;
out:
	return res;
}

static int send_stop_driver(struct impl *impl)
{
	struct nj2_session_params params;

	impl->started = false;
	if (impl->socket)
		pw_loop_update_io(impl->data_loop, impl->socket, 0);

	pw_log_info("sending STOP_DRIVER");
	nj2_session_params_hton(&params, &impl->peer.params);
	params.packet_id = htonl(NJ2_ID_STOP_DRIVER);
	sendto(impl->setup_socket->fd, &params, sizeof(params), 0,
			(struct sockaddr*)&impl->dst_addr, impl->dst_len);

	if (impl->source.filter)
		pw_filter_destroy(impl->source.filter);
	if (impl->sink.filter)
		pw_filter_destroy(impl->sink.filter);

	netjack2_cleanup(&impl->peer);
	return 0;
}

static int destroy_netjack2_socket(struct impl *impl)
{
	update_timer(impl, 0);

	if (impl->socket) {
		pw_loop_destroy_source(impl->data_loop, impl->socket);
		impl->socket = NULL;
	}
	if (impl->setup_socket) {
		send_stop_driver(impl);
		pw_loop_destroy_source(impl->main_loop, impl->setup_socket);
		impl->setup_socket = NULL;
	}
	return 0;
}

static void restart_netjack2_socket(struct impl *impl)
{
	destroy_netjack2_socket(impl);
	create_netjack2_socket(impl);
}

static void on_timer_event(void *data)
{
	struct impl *impl = data;

	if (impl->started) {
		if (impl->received == 0) {
			pw_log_warn("receive timeout, restarting");
			restart_netjack2_socket(impl);
		}
		impl->received = 0;
	}
	if (!impl->started) {
		if (impl->init_retry > 0 && --impl->init_retry == 0) {
			pw_log_error("timeout in connect");
			update_timer(impl, 0);
			pw_impl_module_schedule_destroy(impl->module);
			return;
		}
		send_follower_available(impl);
	}
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
	destroy_netjack2_socket(impl);

	if (impl->source.filter)
		pw_filter_destroy(impl->source.filter);
	if (impl->sink.filter)
		pw_filter_destroy(impl->sink.filter);
	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	pw_timer_queue_cancel(&impl->timer);

	if (impl->data_loop)
		pw_context_release_loop(impl->context, impl->data_loop);

	pw_properties_free(impl->sink.props);
	pw_properties_free(impl->source.props);
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

static void parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
	spa_audio_info_raw_init_dict_keys(info,
			&SPA_DICT_ITEMS(
				 SPA_DICT_ITEM(SPA_KEY_AUDIO_FORMAT, "F32P")),
			&props->dict,
			SPA_KEY_AUDIO_CHANNELS,
			SPA_KEY_AUDIO_POSITION, NULL);
}

static void copy_props(struct impl *impl, struct pw_properties *props, const char *key)
{
	const char *str;
	if ((str = pw_properties_get(props, key)) != NULL) {
		if (pw_properties_get(impl->sink.props, key) == NULL)
			pw_properties_set(impl->sink.props, key, str);
		if (pw_properties_get(impl->source.props, key) == NULL)
			pw_properties_set(impl->source.props, key, str);
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

	impl->module = module;
	impl->context = context;

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
	impl->data_loop = pw_context_acquire_loop(context, &props->dict);
	impl->quantum_limit = pw_properties_get_uint32(
			pw_context_get_properties(context),
			"default.clock.quantum-limit", 8192u);

	impl->sink.props = pw_properties_new(NULL, NULL);
	impl->source.props = pw_properties_new(NULL, NULL);
	if (impl->source.props == NULL || impl->sink.props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->main_loop = pw_context_get_main_loop(context);
	impl->timer_queue = pw_context_get_timer_queue(context);
	impl->system = impl->main_loop->system;

	impl->source.impl = impl;
	impl->source.direction = PW_DIRECTION_OUTPUT;
	impl->sink.impl = impl;
	impl->sink.direction = PW_DIRECTION_INPUT;

	if ((str = pw_properties_get(props, "driver.mode")) != NULL) {
		if (spa_streq(str, "source")) {
			pw_properties_set(impl->sink.props, "audio.ports", "0");
			pw_properties_set(impl->sink.props, "midi.ports", "0");
		} else if (spa_streq(str, "sink")) {
			pw_properties_set(impl->source.props, "audio.ports", "0");
			pw_properties_set(impl->source.props, "midi.ports", "0");
		} else if (!spa_streq(str, "duplex")) {
			pw_log_error("invalid driver.mode '%s'", str);
			res = -EINVAL;
			goto error;
		}
	}

	impl->latency = pw_properties_get_uint32(impl->props, "netjack2.latency",
			DEFAULT_NETWORK_LATENCY);

	pw_properties_set(props, PW_KEY_NODE_LOOP_NAME, impl->data_loop->name);
	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_set(props, PW_KEY_NODE_GROUP, "jack-group");
	if (pw_properties_get(props, PW_KEY_NODE_ALWAYS_PROCESS) == NULL)
		pw_properties_set(props, PW_KEY_NODE_ALWAYS_PROCESS, "true");

	pw_properties_set(impl->sink.props, PW_KEY_PRIORITY_DRIVER, "40000");
	pw_properties_set(impl->sink.props, PW_KEY_NODE_NAME, "netjack2_driver_send");

	pw_properties_set(impl->source.props, PW_KEY_PRIORITY_DRIVER, "40001");
	pw_properties_set(impl->source.props, PW_KEY_NODE_NAME, "netjack2_driver_receive");

	if ((str = pw_properties_get(props, "sink.props")) != NULL)
		pw_properties_update_string(impl->sink.props, str, strlen(str));
	if ((str = pw_properties_get(props, "source.props")) != NULL)
		pw_properties_update_string(impl->source.props, str, strlen(str));

	copy_props(impl, props, PW_KEY_NODE_LOOP_NAME);
	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_ALWAYS_PROCESS);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, "midi.ports");
	copy_props(impl, props, "audio.ports");

	parse_audio_info(impl->source.props, &impl->source.info);
	parse_audio_info(impl->sink.props, &impl->sink.info);

	impl->source.wanted_n_midi = pw_properties_get_int32(impl->source.props,
			"midi.ports", DEFAULT_MIDI_PORTS);
	impl->sink.wanted_n_midi = pw_properties_get_int32(impl->sink.props,
			"midi.ports", DEFAULT_MIDI_PORTS);
	impl->source.wanted_n_audio = pw_properties_get_int32(impl->source.props,
			"audio.ports", DEFAULT_AUDIO_PORTS);
	impl->sink.wanted_n_audio = pw_properties_get_int32(impl->sink.props,
			"audio.ports", DEFAULT_AUDIO_PORTS);

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

	if ((res = create_netjack2_socket(impl)) < 0)
		goto error;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	impl_destroy(impl);
	return res;
}
