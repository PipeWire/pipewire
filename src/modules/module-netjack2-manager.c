/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
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

#ifdef __FreeBSD__
#define ifr_ifindex ifr_index
#endif

#ifndef IPTOS_DSCP
#define IPTOS_DSCP_MASK 0xfc
#define IPTOS_DSCP(x) ((x) & IPTOS_DSCP_MASK)
#endif

/** \page page_module_netjack2_manager Netjack2 manager
 *
 * The netjack2 manager module listens for new netjack2 driver messages and will
 * start a communication channel with them.
 *
 * ## Module Name
 *
 * `libpipewire-module-netjack2-manager`
 *
 * ## Module Options
 *
 * - `local.ifname = <str>`: interface name to use
 * - `net.ip =<str>`: multicast IP address, default "225.3.19.154"
 * - `net.port =<int>`: control port, default "19000"
 * - `net.mtu = <int>`: MTU to use, default 1500
 * - `net.ttl = <int>`: TTL to use, default 1
 * - `net.loop = <bool>`: loopback multicast, default false
 * - `netjack2.connect`: if jack ports should be connected automatically. Can also be
 *                   placed per stream.
 * - `netjack2.sample-rate`: the sample rate to use, default 48000
 * - `netjack2.period-size`: the buffer size to use, default 1024
 * - `netjack2.encoding`: the encoding, float|opus|int, default float
 * - `netjack2.kbps`: the number of kilobits per second when encoding, default 64
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
 * {   name = libpipewire-module-netjack2-manager
 *     args = {
 *         #netjack2.connect     = true
 *         #netjack2.sample-rate = 48000
 *         #netjack2.period-size = 1024
 *         #netjack2.encoding    = float # float|opus
 *         #netjack2.kbps        = 64
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

#define NAME "netjack2-manager"

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


#define NETWORK_MAX_LATENCY	30

#define DEFAULT_SAMPLE_RATE	48000
#define DEFAULT_PERIOD_SIZE	1024
#define DEFAULT_ENCODING	"float"
#define DEFAULT_KBPS		64
#define DEFAULT_CHANNELS	2
#define DEFAULT_POSITION	"[ FL FR ]"
#define DEFAULT_MIDI_PORTS	1

#define MODULE_USAGE	"( remote.name=<remote> ) "				\
			"( local.ifname=<interface name> ) "			\
			"( net.ip=<ip address to use, default 225.3.19.154> ) "	\
			"( net.port=<port to use, default 19000> ) "		\
			"( net.mtu=<MTU to use, default 1500> ) "		\
			"( net.ttl=<TTL to use, default 1> ) "			\
			"( net.loop=<loopback, default false> ) "		\
			"( netjack2.connect=<bool, autoconnect ports> ) "	\
			"( netjack2.sample-rate=<sampl erate, default 48000> ) "\
			"( netjack2.period-size=<period size, default 1024> ) "	\
			"( midi.ports=<number of midi ports> ) "		\
			"( audio.channels=<number of channels> ) "		\
			"( audio.position=<channel map> ) "			\
			"( source.props=<properties> ) "			\
			"( sink.props=<properties> ) "


static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create a netjack2 manager" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static void parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info);

struct port {
	enum spa_direction direction;
	struct spa_latency_info latency[2];
	bool latency_changed[2];
	unsigned int is_midi:1;
};

struct stream {
	struct impl *impl;
	struct follower *follower;

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
	unsigned int ready:1;
};

struct follower {
	struct spa_list link;
	struct impl *impl;

	struct spa_io_position *position;

	struct stream source;
	struct stream sink;

	uint32_t id;
	struct sockaddr_storage dst_addr;
	socklen_t dst_len;

	uint32_t period_size;
	uint32_t samplerate;
	uint64_t frame_time;
	uint32_t cycle;

	uint32_t pw_xrun;
	uint32_t nj2_xrun;

	struct spa_source *setup_socket;
	struct spa_source *socket;

	struct netjack2_peer peer;

	unsigned int done:1;
	unsigned int new_xrun:1;
	unsigned int started:1;
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
	struct pw_properties *sink_props;
	struct pw_properties *source_props;

	uint32_t mtu;
	uint32_t ttl;
	bool loop;
	uint32_t dscp;
	uint32_t period_size;
	uint32_t samplerate;
	uint32_t encoding;
	uint32_t kbps;
	uint32_t quantum_limit;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct sockaddr_storage src_addr;
	socklen_t src_len;

	struct spa_source *setup_socket;
	struct spa_list follower_list;
	uint32_t follower_id;

	unsigned int do_disconnect:1;
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
	struct follower *follower = s->follower;
	uint32_t nframes = position->clock.duration;
	struct data_info midi[s->n_ports];
	struct data_info audio[s->n_ports];
	uint32_t n_midi, n_audio;

	set_info(s, nframes, midi, &n_midi, audio, &n_audio);

	follower->peer.cycle++;
	netjack2_send_data(&follower->peer, nframes, midi, n_midi, audio, n_audio);

	if (follower->socket)
		pw_loop_update_io(s->impl->data_loop, follower->socket, SPA_IO_IN);
}

static void source_process(void *d, struct spa_io_position *position)
{
	struct stream *s = d;
	struct follower *follower = s->follower;
	uint32_t nframes = position->clock.duration;
	struct data_info midi[s->n_ports];
	struct data_info audio[s->n_ports];
	uint32_t n_midi, n_audio;

	set_info(s, nframes, midi, &n_midi, audio, &n_audio);

	netjack2_manager_sync_wait(&follower->peer);
	netjack2_recv_data(&follower->peer, midi, n_midi, audio, n_audio);
}

static void follower_free(struct follower *follower)
{
	struct impl *impl = follower->impl;

	spa_list_remove(&follower->link);

	if (follower->source.filter)
		pw_filter_destroy(follower->source.filter);
	if (follower->sink.filter)
		pw_filter_destroy(follower->sink.filter);

	pw_properties_free(follower->source.props);
	pw_properties_free(follower->sink.props);

	if (follower->socket)
		pw_loop_destroy_source(impl->data_loop, follower->socket);
	if (follower->setup_socket)
		pw_loop_destroy_source(impl->main_loop, follower->setup_socket);

	netjack2_cleanup(&follower->peer);
	free(follower);
}

static int
do_stop_follower(struct spa_loop *loop,
	bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct follower *follower = user_data;
	follower->started = false;
	if (follower->source.filter)
		pw_filter_set_active(follower->source.filter, false);
	if (follower->sink.filter)
		pw_filter_set_active(follower->sink.filter, false);
	follower_free(follower);
	return 0;
}

static int start_follower(struct follower *follower)
{
	struct impl *impl = follower->impl;
	pw_log_info("start follower %s", follower->peer.params.name);
	follower->started = true;
	if (follower->source.filter && follower->source.ready)
		pw_filter_set_active(follower->source.filter, true);
	if (follower->sink.filter && follower->sink.ready)
		pw_filter_set_active(follower->sink.filter, true);
	pw_loop_update_io(impl->main_loop, follower->setup_socket, 0);
	return 0;
}

static void
on_setup_io(void *data, int fd, uint32_t mask)
{
	struct follower *follower = data;
	struct impl *impl = follower->impl;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_warn("error:%08x", mask);
		pw_loop_destroy_source(impl->main_loop, follower->setup_socket);
		follower->setup_socket = NULL;
		return;
	}
	if (mask & SPA_IO_IN) {
		ssize_t len;
		struct nj2_session_params params;

		if ((len = recv(fd, &params, sizeof(params), 0)) < 0)
			goto receive_error;

		if (len < (int)sizeof(params))
			goto short_packet;

		if (strncmp(params.type, "params", sizeof(params.type)) != 0)
			goto wrong_type;

		switch(ntohl(params.packet_id)) {
		case NJ2_ID_START_DRIVER:
			start_follower(follower);
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

static void
on_data_io(void *data, int fd, uint32_t mask)
{
	struct follower *follower = data;
	struct impl *impl = follower->impl;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_warn("error:%08x", mask);
		pw_loop_destroy_source(impl->data_loop, follower->socket);
		follower->socket = NULL;
		pw_loop_invoke(impl->main_loop, do_stop_follower, 1, NULL, 0, false, follower);
		return;
	}
	if (mask & SPA_IO_IN) {
		pw_loop_update_io(impl->data_loop, follower->socket, 0);

		pw_filter_trigger_process(follower->source.filter);
	}
}

static void stream_io_changed(void *data, void *port_data, uint32_t id, void *area, uint32_t size)
{
	struct stream *s = data;
	struct follower *follower = s->follower;
	if (port_data == NULL) {
		switch (id) {
		case SPA_IO_Position:
			follower->position = area;
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
	struct follower *follower = s->follower;
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
		spa_zero(latency);
		latency = SPA_LATENCY_INFO(s->direction,
				.min_quantum = follower->peer.params.network_latency,
				.max_quantum = follower->peer.params.network_latency);
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
			s->ready = true;
			if (s->follower->started)
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
	uint32_t flags;

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	s->filter = pw_filter_new(impl->core, name, s->props);
	s->props = NULL;
	if (s->filter == NULL)
		return -errno;

	flags = PW_FILTER_FLAG_INACTIVE |
			PW_FILTER_FLAG_RT_PROCESS |
			PW_FILTER_FLAG_CUSTOM_LATENCY;

	if (s->direction == PW_DIRECTION_INPUT) {
		pw_filter_add_listener(s->filter, &s->listener,
				&sink_events, s);
	} else {
		pw_filter_add_listener(s->filter, &s->listener,
				&source_events, s);
		flags |= PW_FILTER_FLAG_TRIGGER;
	}

	reset_volume(&s->volume, s->info.channels);

	n_params = 0;
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &s->info);
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_Format, &s->info);
	params[n_params++] = make_props_param(&b, &s->volume);

	return pw_filter_connect(s->filter, flags, params, n_params);
}

static int create_filters(struct follower *follower)
{
	struct impl *impl = follower->impl;
	int res = 0;

	if (impl->mode & MODE_SINK)
		res = make_stream(&follower->sink, "NETJACK2 Send");

	if (impl->mode & MODE_SOURCE)
		res = make_stream(&follower->source, "NETJACK2 Receive");

	return res;
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

static int make_data_socket(struct sockaddr_storage *sa, socklen_t salen,
		bool loop, int ttl, int dscp, char *ifname)
{
	int af, fd, val, res;
	struct timeval timeout;

	af = sa->ss_family;
	if ((fd = socket(af, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0) {
		pw_log_error("socket failed: %m");
		return -errno;
	}
	if (connect(fd, (struct sockaddr*)sa, salen) < 0) {
		res = -errno;
		pw_log_error("connect() failed: %m");
		goto error;
	}

	timeout.tv_sec = 2;
	timeout.tv_usec = 0;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
		pw_log_warn("setsockopt(SO_RCVTIMEO) failed: %m");

	if (dscp > 0) {
		val = IPTOS_DSCP(dscp << 2);
		if (setsockopt(fd, IPPROTO_IP, IP_TOS, &val, sizeof(val)) < 0)
			pw_log_warn("setsockopt(IP_TOS) failed: %m");
	}
	if (is_multicast((struct sockaddr*)sa, salen)) {
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

static int make_announce_socket(struct sockaddr_storage *sa, socklen_t salen,
		char *ifname)
{
	int af, fd, val, res;
	struct ifreq req;

	af = sa->ss_family;
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
	spa_zero(req);
	if (ifname) {
		snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", ifname);
		res = ioctl(fd, SIOCGIFINDEX, &req);
	        if (res < 0)
	                pw_log_warn("SIOCGIFINDEX %s failed: %m", ifname);
	}
	res = 0;
	if (af == AF_INET) {
		static const uint32_t ipv4_mcast_mask = 0xe0000000;
		struct sockaddr_in *sa4 = (struct sockaddr_in*)sa;
		if ((ntohl(sa4->sin_addr.s_addr) & ipv4_mcast_mask) == ipv4_mcast_mask) {
			struct ip_mreqn mr4;
			memset(&mr4, 0, sizeof(mr4));
			mr4.imr_multiaddr = sa4->sin_addr;
			mr4.imr_ifindex = req.ifr_ifindex;
			res = setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr4, sizeof(mr4));
		} else {
			sa4->sin_addr.s_addr = INADDR_ANY;
		}
	} else if (af == AF_INET6) {
		struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)sa;
		if (sa6->sin6_addr.s6_addr[0] == 0xff) {
			struct ipv6_mreq mr6;
			memset(&mr6, 0, sizeof(mr6));
			mr6.ipv6mr_multiaddr = sa6->sin6_addr;
			mr6.ipv6mr_interface = req.ifr_ifindex;
			res = setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mr6, sizeof(mr6));
		} else {
		        sa6->sin6_addr = in6addr_any;
		}
	} else {
		res = -EINVAL;
		goto error;
	}

	if (res < 0) {
		res = -errno;
		pw_log_error("join mcast failed: %m");
		goto error;
	}
	if (bind(fd, (struct sockaddr*)sa, salen) < 0) {
		res = -errno;
		pw_log_error("bind() failed: %m");
		goto error;
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
	} else {
		snprintf(ip, len, "invalid address");
	}
	return ip;
}

static int handle_follower_available(struct impl *impl, struct nj2_session_params *params,
	struct sockaddr_storage *addr, socklen_t addr_len)
{
	int res, fd;
	struct follower *follower;
	char buffer[256];
	struct netjack2_peer *peer;

	pw_log_info("got follower available");
	nj2_dump_session_params(params);

	if (ntohl(params->version) != NJ2_NETWORK_PROTOCOL) {
		pw_log_warn("invalid version");
		return -EINVAL;
	}

	follower = calloc(1, sizeof(*follower));
	if (follower == NULL)
		return -errno;

	follower->impl = impl;
	follower->id = impl->follower_id;
	spa_list_append(&impl->follower_list, &follower->link);

	peer = &follower->peer;

	follower->source.impl = impl;
	follower->source.follower = follower;
	follower->source.direction = PW_DIRECTION_OUTPUT;
	follower->source.props = pw_properties_copy(impl->source_props);
	follower->sink.impl = impl;
	follower->sink.follower = follower;
	follower->sink.direction = PW_DIRECTION_INPUT;
	follower->sink.props = pw_properties_copy(impl->sink_props);

	parse_audio_info(follower->source.props, &follower->source.info);
	parse_audio_info(follower->sink.props, &follower->sink.info);

	follower->source.n_midi = pw_properties_get_uint32(follower->source.props,
			"midi.ports", DEFAULT_MIDI_PORTS);
	follower->sink.n_midi = pw_properties_get_uint32(follower->sink.props,
			"midi.ports", DEFAULT_MIDI_PORTS);

	follower->samplerate = impl->samplerate;
	follower->period_size = impl->period_size;

	pw_properties_setf(follower->sink.props, PW_KEY_NODE_RATE,
			"1/%u", follower->samplerate);
	pw_properties_set(follower->sink.props, PW_KEY_NODE_FORCE_RATE, "0");
	pw_properties_setf(follower->sink.props, PW_KEY_NODE_FORCE_QUANTUM,
			"%u", follower->period_size);
	pw_properties_setf(follower->source.props, PW_KEY_NODE_RATE,
			"1/%u", follower->samplerate);
	pw_properties_set(follower->source.props, PW_KEY_NODE_FORCE_RATE, "0");
	pw_properties_setf(follower->source.props, PW_KEY_NODE_FORCE_QUANTUM,
			"%u", follower->period_size);

	nj2_session_params_ntoh(&peer->params, params);

	pw_properties_setf(follower->source.props, PW_KEY_NODE_DESCRIPTION, "%s NETJACK2 from %s",
			params->name, params->follower_name);
	pw_properties_setf(follower->sink.props, PW_KEY_NODE_DESCRIPTION, "%s NETJACK2 to %s",
			params->name, params->follower_name);

	peer->params.mtu = impl->mtu;
	peer->params.id = follower->id;
	snprintf(peer->params.driver_name, sizeof(peer->params.driver_name), "%s", pw_get_host_name());
	peer->params.sample_rate = follower->samplerate;
	peer->params.period_size = follower->period_size;
	peer->params.sample_encoder = impl->encoding;
	peer->params.kbps = impl->kbps;

	if (peer->params.send_audio_channels < 0)
		peer->params.send_audio_channels = follower->sink.info.channels;
	if (peer->params.recv_audio_channels < 0)
		peer->params.recv_audio_channels = follower->source.info.channels;
	if (peer->params.send_midi_channels < 0)
		peer->params.send_midi_channels = follower->sink.n_midi;
	if (peer->params.recv_midi_channels < 0)
		peer->params.recv_midi_channels = follower->source.n_midi;

	follower->source.n_ports = peer->params.send_audio_channels + peer->params.send_midi_channels;
	follower->source.info.rate =  peer->params.sample_rate;
	follower->source.info.channels =  peer->params.send_audio_channels;
	follower->sink.n_ports = peer->params.recv_audio_channels + peer->params.recv_midi_channels;
	follower->sink.info.rate =  peer->params.sample_rate;
	follower->sink.info.channels =  peer->params.recv_audio_channels;

	follower->source.n_ports = follower->source.n_midi + follower->source.info.channels;
	follower->sink.n_ports = follower->sink.n_midi + follower->sink.info.channels;
	if (follower->source.n_ports > MAX_PORTS || follower->sink.n_ports > MAX_PORTS) {
		pw_log_error("too many ports");
		res = -EINVAL;
		goto cleanup;
	}

	if ((res = create_filters(follower)) < 0)
		goto create_failed;

	fd = make_data_socket(addr, addr_len, impl->loop,
			impl->ttl, impl->dscp, NULL);
	if (fd < 0)
		goto socket_failed;

	follower->setup_socket = pw_loop_add_io(impl->main_loop, fd,
			0, true, on_setup_io, follower);
	if (follower->setup_socket == NULL) {
		res = -errno;
		pw_log_error("can't create setup source: %m");
		goto socket_failed;
	}

	follower->socket = pw_loop_add_io(impl->data_loop, fd,
			0, false, on_data_io, follower);
	if (follower->socket == NULL) {
		res = -errno;
		pw_log_error("can't create data source: %m");
		goto socket_failed;
	}
	peer->fd = fd;
	peer->our_stream = 's';
	peer->other_stream = 'r';
	peer->send_volume = &follower->sink.volume;
	peer->recv_volume = &follower->source.volume;
	peer->quantum_limit = impl->quantum_limit;
	netjack2_init(peer);

	int bufsize = NETWORK_MAX_LATENCY * (peer->params.mtu +
		follower->period_size * sizeof(float) *
		SPA_MAX(follower->source.n_ports, follower->sink.n_ports));

	pw_log_info("send/recv buffer %d", bufsize);
	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize)) < 0)
		pw_log_warn("setsockopt(SO_SNDBUF) failed: %m");
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize)) < 0)
		pw_log_warn("setsockopt(SO_SNDBUF) failed: %m");

	impl->follower_id++;

	pw_loop_update_io(impl->main_loop, follower->setup_socket, SPA_IO_IN);

	nj2_session_params_hton(params, &peer->params);
	params->packet_id = htonl(NJ2_ID_FOLLOWER_SETUP);

	pw_log_info("sending follower setup to %s", get_ip(addr, buffer, sizeof(buffer)));
	nj2_dump_session_params(params);
	send(follower->socket->fd, params, sizeof(*params), 0);

	return 0;

create_failed:
	pw_log_error("can't create streams: %s", spa_strerror(res));
	goto cleanup;
socket_failed:
	res = fd;
	pw_log_error("can't create socket: %s", spa_strerror(res));
	goto cleanup;
cleanup:
	follower_free(follower);
	return res;
}

static void
on_socket_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;

	if (mask & SPA_IO_IN) {
		ssize_t len;
		struct sockaddr_storage addr;
		socklen_t addr_len  = sizeof(addr);
		struct nj2_session_params params;

		if ((len = recvfrom(fd, &params, sizeof(params), 0,
				(struct sockaddr *)&addr, &addr_len)) < 0)
			goto receive_error;

		if (len < (int)sizeof(params))
			goto short_packet;

		if (strncmp(params.type, "params", sizeof(params.type)) != 0)
			goto wrong_type;

		switch(ntohl(params.packet_id)) {
		case NJ2_ID_FOLLOWER_AVAILABLE:
			handle_follower_available(impl, &params, &addr, addr_len);
			break;
		case NJ2_ID_STOP_DRIVER:
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

static int create_netjack2_socket(struct impl *impl)
{
	const char *str;
	uint32_t port;
	int fd, res;
	char buffer[256];

	port = pw_properties_get_uint32(impl->props, "net.port", 0);
	if (port == 0)
		port = DEFAULT_NET_PORT;
	if ((str = pw_properties_get(impl->props, "net.ip")) == NULL)
		str = DEFAULT_NET_IP;

	if ((res = parse_address(str, port, &impl->src_addr, &impl->src_len)) < 0) {
		pw_log_error("invalid net.ip %s: %s", str, spa_strerror(res));
		goto out;
	}

	impl->mtu = pw_properties_get_uint32(impl->props, "net.mtu", DEFAULT_NET_MTU);
	impl->ttl = pw_properties_get_uint32(impl->props, "net.ttl", DEFAULT_NET_TTL);
	impl->loop = pw_properties_get_bool(impl->props, "net.loop", DEFAULT_NET_LOOP);
	impl->dscp = pw_properties_get_uint32(impl->props, "net.dscp", DEFAULT_NET_DSCP);

	fd = make_announce_socket(&impl->src_addr, impl->src_len, NULL);
	if (fd < 0) {
		res = fd;
		pw_log_error("can't create socket: %s", spa_strerror(res));
		goto out;
	}

	impl->setup_socket = pw_loop_add_io(impl->main_loop, fd,
				SPA_IO_IN, true, on_socket_io, impl);
	if (impl->setup_socket == NULL) {
		res = -errno;
		pw_log_error("can't create setup source: %m");
		close(fd);
		goto out;
	}
	pw_log_info("listening for AVAILABLE on %s",
			get_ip(&impl->src_addr, buffer, sizeof(buffer)));
	return 0;
out:
	return res;
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
	struct follower *f;

	if (impl->setup_socket) {
		pw_loop_destroy_source(impl->main_loop, impl->setup_socket);
		impl->setup_socket = NULL;
	}
	spa_list_consume(f, &impl->follower_list, link)
		follower_free(f);

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
	struct pw_data_loop *data_loop;
	struct impl *impl;
	const char *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);
	spa_list_init(&impl->follower_list);

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
	impl->quantum_limit = pw_properties_get_uint32(
			pw_context_get_properties(context),
			"default.clock.quantum-limit", 8192u);

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
	impl->samplerate = pw_properties_get_uint32(impl->props, "netjack2.sample-rate",
			DEFAULT_SAMPLE_RATE);
	impl->period_size = pw_properties_get_uint32(impl->props, "netjack2.period-size",
			DEFAULT_PERIOD_SIZE);
	if ((str = pw_properties_get(impl->props, "netjack2.encoding")) == NULL)
		str = DEFAULT_ENCODING;
	if (spa_streq(str, "float")) {
		impl->encoding = NJ2_ENCODER_FLOAT;
	} else if (spa_streq(str, "opus")) {
#ifdef HAVE_OPUS
		impl->encoding = NJ2_ENCODER_OPUS;
#else
		pw_log_error("OPUS support is disabled");
		res = -EINVAL;
		goto error;
#endif
	} else if (spa_streq(str, "int")) {
		impl->encoding = NJ2_ENCODER_INT;
	} else {
			pw_log_error("invalid netjack2.encoding '%s'", str);
			res = -EINVAL;
			goto error;
	}
	impl->kbps = pw_properties_get_uint32(impl->props, "netjack2.kbps",
			DEFAULT_KBPS);

	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, PW_KEY_NODE_NETWORK) == NULL)
		pw_properties_set(props, PW_KEY_NODE_NETWORK, "true");
	if (pw_properties_get(props, PW_KEY_NODE_LINK_GROUP) == NULL)
		pw_properties_set(props, PW_KEY_NODE_LINK_GROUP, "jack-group");
	if (pw_properties_get(props, PW_KEY_NODE_ALWAYS_PROCESS) == NULL)
		pw_properties_set(props, PW_KEY_NODE_ALWAYS_PROCESS, "true");
	if (pw_properties_get(props, PW_KEY_NODE_LOCK_QUANTUM) == NULL)
		pw_properties_set(props, PW_KEY_NODE_LOCK_QUANTUM, "true");
	if (pw_properties_get(props, PW_KEY_NODE_LOCK_RATE) == NULL)
		pw_properties_set(props, PW_KEY_NODE_LOCK_RATE, "true");

	pw_properties_set(impl->sink_props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
	pw_properties_set(impl->sink_props, PW_KEY_NODE_NAME, "netjack2_manager_send");

	pw_properties_set(impl->source_props, PW_KEY_MEDIA_CLASS, "Audio/Source");
	pw_properties_set(impl->source_props, PW_KEY_NODE_NAME, "netjack2_manager_recv");

	if ((str = pw_properties_get(props, "sink.props")) != NULL)
		pw_properties_update_string(impl->sink_props, str, strlen(str));
	if ((str = pw_properties_get(props, "source.props")) != NULL)
		pw_properties_update_string(impl->source_props, str, strlen(str));

	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, PW_KEY_NODE_NETWORK);
	copy_props(impl, props, PW_KEY_NODE_LINK_GROUP);
	copy_props(impl, props, PW_KEY_NODE_ALWAYS_PROCESS);
	copy_props(impl, props, PW_KEY_NODE_LOCK_QUANTUM);
	copy_props(impl, props, PW_KEY_NODE_LOCK_RATE);
	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);

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
