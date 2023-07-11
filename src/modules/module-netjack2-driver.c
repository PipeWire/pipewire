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
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <net/if.h>
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

#include "module-netjack2/packets.h"
#include "module-netjack2/peer.c"

#ifndef IPTOS_DSCP
#define IPTOS_DSCP_MASK 0xfc
#define IPTOS_DSCP(x) ((x) & IPTOS_DSCP_MASK)
#endif

/** \page page_module_netjack2_driver PipeWire Module: Netjack2 driver
 *
 * The netjack2-driver module provides a source or sink that is following a
 * netjack2 driver.
 *
 * ## Module Options
 *
 * - `driver.mode`: the driver mode, sink|source|duplex, default duplex
 * - `local.ifname = <str>`: interface name to use
 * - `net.ip =<str>`: multicast IP address, default "225.3.19.154"
 * - `net.port =<int>`: control port, default "19000"
 * - `net.mtu = <int>`: MTU to use, default 1500
 * - `net.ttl = <int>`: TTL to use, default 1
 * - `net.loop = <bool>`: loopback multicast, default false
 * - `netjack2.client-name`: the name of the NETJACK2 client.
 * - `netjack2.save`: if jack port connections should be save automatically. Can also be
 *                   placed per stream.
 * - `netjack2.latency`: the latency in cycles, default 2
 * - `audio.channels`: the number of audio ports. Can also be added to the stream props.
 * - `midi.ports`: the number of midi ports. Can also be added to the stream props.
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
 * {   name = libpipewire-module-netjack2-driver
 *     args = {
 *         #driver.mode          = duplex
 *         #netjack2.client-name = PipeWire
 *         #netjack2.save        = false
 *         #netjack2.latency     = 2
 *         #midi.ports           = 0
 *         #audio.channels       = 2
 *         #audio.position       = [ FL FR ]
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

#define DEFAULT_NETWORK_LATENCY	2
#define NETWORK_MAX_LATENCY	30

#define DEFAULT_CLIENT_NAME	"PipeWire"
#define DEFAULT_CHANNELS	2
#define DEFAULT_POSITION	"[ FL FR ]"
#define DEFAULT_MIDI_PORTS	1

#define FOLLOWER_INIT_TIMEOUT	1
#define FOLLOWER_INIT_RETRY	-1

#define MODULE_USAGE	"( remote.name=<remote> ) "				\
			"( driver.mode=<sink|source|duplex> ) "			\
			"( local.ifname=<interface name> ) "			\
			"( net.ip=<ip address to use, default 225.3.19.154> ) "	\
			"( net.port=<port to use, default 19000> ) "		\
			"( net.mtu=<MTU to use, default 1500> ) "		\
			"( net.ttl=<TTL to use, default 1> ) "			\
			"( net.loop=<loopback, default false> ) "		\
			"( netjack2.client-name=<name of the NETJACK2 client> ) "	\
			"( netjack2.save=<bool, save ports> ) "			\
			"( netjack2.latency=<latency in cycles, default 2> ) "	\
			"( midi.ports=<number of midi ports> ) "		\
			"( audio.channels=<number of channels> ) "		\
			"( audio.position=<channel map> ) "			\
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

	struct spa_audio_info_raw info;

	uint32_t n_midi;
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

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct spa_io_position *position;

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
	struct spa_source *timer;
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
	struct impl *impl = s->impl;
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

static void param_latency_changed(struct stream *s, const struct spa_pod *param,
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
}

static void make_stream_ports(struct stream *s)
{
	struct impl *impl = s->impl;
	uint32_t i;
	struct pw_properties *props;
	const char *str, *prefix;
	char name[256];
	bool is_midi;
	uint8_t buffer[512];
	struct spa_pod_builder b;
	struct spa_latency_info latency;
	const struct spa_pod *params[1];

	if (s->direction == PW_DIRECTION_INPUT) {
		/* sink */
		prefix = "playback";
	} else {
		/* source */
		prefix = "capture";
	}

	for (i = 0; i < s->n_ports; i++) {
		struct port *port = s->ports[i];

		if (port != NULL) {
			s->ports[i] = NULL;
			pw_filter_remove_port(port);
		}

		if (i < s->info.channels) {
			str = spa_debug_type_find_short_name(spa_type_audio_channel,
					s->info.position[i]);
			if (str)
				snprintf(name, sizeof(name), "%s_%s", prefix, str);
			else
				snprintf(name, sizeof(name), "%s_%d", prefix, i);

			props = pw_properties_new(
					PW_KEY_FORMAT_DSP, "32 bit float mono audio",
					PW_KEY_AUDIO_CHANNEL, str ? str : "UNK",
					PW_KEY_PORT_PHYSICAL, "true",
					PW_KEY_PORT_NAME, name,
					NULL);

			is_midi = false;
		} else {
			snprintf(name, sizeof(name), "%s_%d", prefix, i - s->info.channels);
			props = pw_properties_new(
					PW_KEY_FORMAT_DSP, "8 bit raw midi",
					PW_KEY_PORT_NAME, name,
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
			float vols[SPA_AUDIO_MAX_CHANNELS];
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
			pw_filter_set_active(s->filter, true);
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

	return pw_filter_connect(s->filter,
			PW_FILTER_FLAG_INACTIVE |
			PW_FILTER_FLAG_DRIVER |
			PW_FILTER_FLAG_RT_PROCESS |
			PW_FILTER_FLAG_CUSTOM_LATENCY,
			params, n_params);
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


static inline uint64_t get_time_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return SPA_TIMESPEC_TO_NSEC(&ts);
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

		nsec = get_time_ns();

		if (!impl->done) {
			impl->pw_xrun++;
			impl->new_xrun = true;
		}
		impl->received++;

		source_running = impl->source.running;
		sink_running = impl->sink.running;

		impl->frame_time += nframes;

		pw_log_trace_fp("process %d %u %u %p %"PRIu64, nframes, source_running,
				sink_running, impl->position, impl->frame_time);

		if (impl->new_xrun) {
			pw_log_warn("Xrun netjack2:%u PipeWire:%u", impl->nj2_xrun, impl->pw_xrun);
			impl->new_xrun = false;
		}
		if (impl->position) {
			struct spa_io_clock *c = &impl->position->clock;

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
		if (!source_running)
			netjack2_recv_data(&impl->peer, NULL, 0, NULL, 0);

		if (impl->mode & MODE_SOURCE && source_running) {
			impl->done = false;
			impl->triggered = true;
			impl->driving = MODE_SOURCE;
			pw_filter_trigger_process(impl->source.filter);
		} else if (impl->mode == MODE_SINK && sink_running) {
			impl->done = false;
			impl->triggered = true;
			impl->driving = MODE_SINK;
			pw_filter_trigger_process(impl->sink.filter);
		} else {
			sink_running = false;
			impl->done = true;
		}
		if (!sink_running)
			netjack2_send_data(&impl->peer, nframes, NULL, 0, NULL, 0);
	}
}

static int parse_address(const char *address, uint16_t port,
		struct sockaddr_storage *addr, socklen_t *len)
{
	struct sockaddr_in *sa4 = (struct sockaddr_in*)addr;
	struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)addr;

	if (inet_pton(AF_INET, address, &sa4->sin_addr) > 0) {
		sa4->sin_family = AF_INET;
		sa4->sin_port = htons(port);
		*len = sizeof(*sa4);
	} else if (inet_pton(AF_INET6, address, &sa6->sin6_addr) > 0) {
		sa6->sin6_family = AF_INET6;
		sa6->sin6_port = htons(port);
		*len = sizeof(*sa6);
	} else
		return -EINVAL;

	return 0;
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
		bool loop, int ttl, int dscp)
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

static const char *get_ip(const struct sockaddr_storage *sa, char *ip, size_t len)
{
	if (sa->ss_family == AF_INET) {
		struct sockaddr_in *in = (struct sockaddr_in*)sa;
		inet_ntop(sa->ss_family, &in->sin_addr, ip, len);
	} else if (sa->ss_family == AF_INET6) {
		struct sockaddr_in6 *in = (struct sockaddr_in6*)sa;
		inet_ntop(sa->ss_family, &in->sin6_addr, ip, len);
	} else
		snprintf(ip, len, "invalid ip");
	return ip;
}

static void update_timer(struct impl *impl, uint64_t timeout)
{
	struct timespec value, interval;
	value.tv_sec = 0;
	value.tv_nsec = timeout ? 1 : 0;
	interval.tv_sec = timeout;
	interval.tv_nsec = 0;
	pw_loop_update_timer(impl->main_loop, impl->timer, &value, &interval, false);
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

	pw_log_info("got follower setup");
	nj2_dump_session_params(params);

	nj2_session_params_ntoh(&peer->params, params);
	SPA_SWAP(peer->params.send_audio_channels, peer->params.recv_audio_channels);
	SPA_SWAP(peer->params.send_midi_channels, peer->params.recv_midi_channels);

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

	pw_loop_update_io(impl->main_loop, impl->setup_socket, 0);

	impl->source.n_ports = peer->params.send_audio_channels + peer->params.send_midi_channels;
	impl->source.info.rate =  peer->params.sample_rate;
	impl->source.info.channels =  peer->params.send_audio_channels;
	impl->sink.n_ports = peer->params.recv_audio_channels + peer->params.recv_midi_channels;
	impl->sink.info.rate =  peer->params.sample_rate;
	impl->sink.info.channels =  peer->params.recv_audio_channels;
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

	if ((res = create_filters(impl)) < 0)
		return res;

	peer->fd = impl->socket->fd;
	peer->our_stream = 'r';
	peer->other_stream = 's';
	peer->send_volume = &impl->sink.volume;
	peer->recv_volume = &impl->source.volume;
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

		if (strcmp(params.type, "params") != 0)
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

	pw_log_info("sending AVAILABLE to %s", get_ip(&impl->dst_addr, buffer, sizeof(buffer)));

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
	params.send_audio_channels = htonl(-1);
	params.recv_audio_channels = htonl(-1);
	params.send_midi_channels = htonl(-1);
	params.recv_midi_channels = htonl(-1);
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
	if ((res = parse_address(str, port, &impl->dst_addr, &impl->dst_len)) < 0) {
		pw_log_error("invalid net.ip %s: %s", str, spa_strerror(res));
		goto out;
	}
	if ((res = parse_address("0.0.0.0", 0, &impl->src_addr, &impl->src_len)) < 0) {
		pw_log_error("invalid source.ip: %s", spa_strerror(res));
		goto out;
	}

	impl->mtu = pw_properties_get_uint32(impl->props, "net.mtu", DEFAULT_NET_MTU);
	impl->ttl = pw_properties_get_uint32(impl->props, "net.ttl", DEFAULT_NET_TTL);
	impl->loop = pw_properties_get_bool(impl->props, "net.loop", DEFAULT_NET_LOOP);
	impl->dscp = pw_properties_get_uint32(impl->props, "net.dscp", DEFAULT_NET_DSCP);

	fd = make_socket(&impl->src_addr, impl->src_len,
			&impl->dst_addr, impl->dst_len, impl->loop, impl->ttl, impl->dscp);
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

static void on_timer_event(void *data, uint64_t expirations)
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

	if (impl->timer)
		pw_loop_destroy_source(impl->main_loop, impl->timer);

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
	struct pw_data_loop *data_loop;
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
	data_loop = pw_context_get_data_loop(context);
	impl->data_loop = pw_data_loop_get_loop(data_loop);

	impl->sink.props = pw_properties_new(NULL, NULL);
	impl->source.props = pw_properties_new(NULL, NULL);
	if (impl->source.props == NULL || impl->sink.props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->module = module;
	impl->context = context;
	impl->main_loop = pw_context_get_main_loop(context);
	impl->system = impl->main_loop->system;

	impl->source.impl = impl;
	impl->source.direction = PW_DIRECTION_OUTPUT;
	impl->sink.impl = impl;
	impl->sink.direction = PW_DIRECTION_INPUT;

	impl->mode = MODE_DUPLEX;
	if ((str = pw_properties_get(props, "driver.mode")) != NULL) {
		if (spa_streq(str, "source")) {
			impl->mode = MODE_SOURCE;
		} else if (spa_streq(str, "sink")) {
			impl->mode = MODE_SINK;
		} else if (spa_streq(str, "duplex")) {
			impl->mode = MODE_DUPLEX;
		} else {
			pw_log_error("invalid driver.mode '%s'", str);
			res = -EINVAL;
			goto error;
		}
	}
	impl->latency = pw_properties_get_uint32(impl->props, "netjack2.latency",
			DEFAULT_NETWORK_LATENCY);

	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_set(props, PW_KEY_NODE_GROUP, "jack-group");
	if (pw_properties_get(props, PW_KEY_NODE_ALWAYS_PROCESS) == NULL)
		pw_properties_set(props, PW_KEY_NODE_ALWAYS_PROCESS, "true");

	pw_properties_set(impl->sink.props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
	pw_properties_set(impl->sink.props, PW_KEY_PRIORITY_DRIVER, "40000");
	pw_properties_set(impl->sink.props, PW_KEY_NODE_NAME, "netjack2_driver_send");

	pw_properties_set(impl->source.props, PW_KEY_MEDIA_CLASS, "Audio/Source");
	pw_properties_set(impl->source.props, PW_KEY_PRIORITY_DRIVER, "40001");
	pw_properties_set(impl->source.props, PW_KEY_NODE_NAME, "netjack2_driver_receive");

	if ((str = pw_properties_get(props, "sink.props")) != NULL)
		pw_properties_update_string(impl->sink.props, str, strlen(str));
	if ((str = pw_properties_get(props, "source.props")) != NULL)
		pw_properties_update_string(impl->source.props, str, strlen(str));

	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_ALWAYS_PROCESS);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);

	parse_audio_info(impl->source.props, &impl->source.info);
	parse_audio_info(impl->sink.props, &impl->sink.info);

	impl->source.n_midi = pw_properties_get_uint32(impl->source.props,
			"midi.ports", DEFAULT_MIDI_PORTS);
	impl->sink.n_midi = pw_properties_get_uint32(impl->sink.props,
			"midi.ports", DEFAULT_MIDI_PORTS);

	impl->source.n_ports = impl->source.n_midi + impl->source.info.channels;
	impl->sink.n_ports = impl->sink.n_midi + impl->sink.info.channels;
	if (impl->source.n_ports > MAX_PORTS || impl->sink.n_ports > MAX_PORTS) {
		pw_log_error("too many ports");
		res = -EINVAL;
		goto error;
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

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener,
			&core_proxy_events, impl);
	pw_core_add_listener(impl->core,
			&impl->core_listener,
			&core_events, impl);

	impl->timer = pw_loop_add_timer(impl->main_loop, on_timer_event, impl);
	if (impl->timer == NULL) {
		res = -errno;
		pw_log_error("can't create timer source: %m");
		goto error;
	}

	if ((res = create_netjack2_socket(impl)) < 0)
		goto error;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	impl_destroy(impl);
	return res;
}
