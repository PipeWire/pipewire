/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <ctype.h>

#include <spa/utils/hook.h>
#include <spa/utils/result.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/defs.h>
#include <spa/utils/dll.h>
#include <spa/utils/json.h>
#include <spa/param/audio/format-utils.h>
#include <spa/control/control.h>
#include <spa/debug/types.h>
#include <spa/debug/mem.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

#include <module-rtp/stream.h>

#ifdef __FreeBSD__
#define ifr_ifindex ifr_index
#endif

/** \page page_module_rtp_source RTP source
 *
 * The `rtp-source` module creates a PipeWire source that receives audio
 * and midi RTP packets.
 *
 * This module is usually loaded from the \ref page_module_rtp_sap so that the
 * source.ip and source.port and format parameters matches that of the sender.
 *
 * ## Module Name
 *
 * `libpipewire-module-rtp-source`
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `local.ifname = <str>`: interface name to use
 * - `source.ip = <str>`: the source ip address, default 224.0.0.56
 * - `source.port = <int>`: the source port
 * - `node.always-process = <bool>`: true to receive even when not running
 * - `sess.latency.msec = <str>`: target network latency in milliseconds, default 100
 * - `sess.ignore-ssrc = <bool>`: ignore SSRC, default false
 * - `sess.media = <string>`: the media type audio|midi|opus, default audio
 * - `stream.props = {}`: properties to be passed to the stream
 *
 * ## General options
 *
 * Options with well-known behavior:
 *
 * - \ref PW_KEY_REMOTE_NAME
 * - \ref PW_KEY_AUDIO_FORMAT
 * - \ref PW_KEY_AUDIO_RATE
 * - \ref PW_KEY_AUDIO_CHANNELS
 * - \ref SPA_KEY_AUDIO_POSITION
 * - \ref PW_KEY_MEDIA_NAME
 * - \ref PW_KEY_MEDIA_CLASS
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_NODE_GROUP
 * - \ref PW_KEY_NODE_LATENCY
 * - \ref PW_KEY_NODE_VIRTUAL
 *
 * ## Example configuration
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-rtp-source
 *     args = {
 *         #local.ifname = eth0
 *         #source.ip = 224.0.0.56
 *         #source.port = 0
 *         sess.latency.msec = 100
 *         #sess.ignore-ssrc = false
 *         #node.always-process = false
 *         #sess.media = "audio"
 *         #audio.format = "S16BE"
 *         #audio.rate = 48000
 *         #audio.channels = 2
 *         #audio.position = [ FL FR ]
 *         stream.props = {
 *            #media.class = "Audio/Source"
 *            node.name = "rtp-source"
 *         }
 *     }
 * }
 * ]
 *\endcode
 *
 * \since 0.3.60
 */

#define NAME "rtp-source"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define DEFAULT_CLEANUP_SEC		60
#define DEFAULT_SOURCE_IP		"224.0.0.56"

#define DEFAULT_TS_OFFSET		-1

#define USAGE   "( local.ifname=<local interface name to use> ) "						\
		"( source.ip=<source IP address, default:"DEFAULT_SOURCE_IP"> ) "				\
 		"source.port=<int, source port> "								\
		"( sess.latency.msec=<target network latency, default "SPA_STRINGIFY(DEFAULT_SESS_LATENCY)"> ) "\
		"( sess.ignore-ssrc=<to ignore SSRC, default false> ) "\
 		"( sess.media=<string, the media type audio|midi|opus, default audio> ) "			\
		"( audio.format=<format, default:"DEFAULT_FORMAT"> ) "						\
		"( audio.rate=<sample rate, default:"SPA_STRINGIFY(DEFAULT_RATE)"> ) "				\
		"( audio.channels=<number of channels, default:"SPA_STRINGIFY(DEFAULT_CHANNELS)"> ) "		\
		"( audio.position=<channel map, default:"DEFAULT_POSITION"> ) "					\
		"( stream.props= { key=value ... } ) "

static const struct spa_dict_item module_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "RTP Source" },
	{ PW_KEY_MODULE_USAGE,	USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_impl_module *module;
	struct spa_hook module_listener;
	struct pw_properties *props;
	struct pw_context *context;

	struct pw_loop *loop;
	struct pw_loop *data_loop;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;
	unsigned int do_disconnect:1;

	char *ifname;
	bool always_process;
	uint32_t cleanup_interval;

	struct spa_source *timer;

	struct pw_properties *stream_props;
	struct rtp_stream *stream;

	uint16_t src_port;
	struct sockaddr_storage src_addr;
	socklen_t src_len;
	struct spa_source *source;

	unsigned receiving:1;
};

static void
on_rtp_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;
	ssize_t len;
	uint8_t buffer[2048];

	if (mask & SPA_IO_IN) {
		if ((len = recv(fd, buffer, sizeof(buffer), 0)) < 0)
			goto receive_error;

		if (len < 12)
			goto short_packet;

		if (SPA_LIKELY(impl->stream))
			rtp_stream_receive_packet(impl->stream, buffer, len);

		impl->receiving = true;
	}
	return;

receive_error:
	pw_log_warn("recv error: %m");
	return;
short_packet:
	pw_log_warn("short packet received");
	return;
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

static int make_socket(const struct sockaddr* sa, socklen_t salen, char *ifname)
{
	int af, fd, val, res;
	struct ifreq req;

	af = sa->sa_family;
	if ((fd = socket(af, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
		pw_log_error("socket failed: %m");
		return -errno;
	}
#ifdef SO_TIMESTAMP
	val = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &val, sizeof(val)) < 0) {
		res = -errno;
		pw_log_error("setsockopt failed: %m");
		goto error;
	}
#endif
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

	if (bind(fd, sa, salen) < 0) {
		res = -errno;
		pw_log_error("bind() failed: %m");
		goto error;
	}
	return fd;
error:
	close(fd);
	return res;
}

static int stream_start(struct impl *impl)
{
	int fd;

	if (impl->source != NULL)
		return 0;

	pw_log_info("starting RTP listener");

	if ((fd = make_socket((const struct sockaddr *)&impl->src_addr,
					impl->src_len, impl->ifname)) < 0) {
		pw_log_error("failed to create socket: %m");
		return fd;
	}

	impl->source = pw_loop_add_io(impl->data_loop, fd,
				SPA_IO_IN, true, on_rtp_io, impl);
	if (impl->source == NULL) {
		pw_log_error("can't create io source: %m");
		close(fd);
		return -errno;
	}
	return 0;
}

static void stream_stop(struct impl *impl)
{
	if (!impl->source)
		return;

	pw_log_info("stopping RTP listener");

	pw_loop_destroy_source(impl->data_loop, impl->source);
	impl->source = NULL;
}

static void stream_destroy(void *d)
{
	struct impl *impl = d;
	impl->stream = NULL;
}

static void stream_state_changed(void *data, bool started, const char *error)
{
	struct impl *impl = data;

	if (error) {
		pw_log_error("stream error: %s", error);
		pw_impl_module_schedule_destroy(impl->module);
	} else if (started) {
		if ((errno = -stream_start(impl)) < 0)
			pw_log_error("failed to start RTP stream: %m");
	} else {
		if (!impl->always_process)
			stream_stop(impl);
	}
}

static const struct rtp_stream_events stream_events = {
	RTP_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
};

static void on_timer_event(void *data, uint64_t expirations)
{
	struct impl *impl = data;

	if (!impl->receiving) {
		pw_log_info("timeout, inactive RTP source");
		//pw_impl_module_schedule_destroy(impl->module);
	} else {
		pw_log_debug("timeout, keeping active RTP source");
	}
	impl->receiving = false;
}

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
	if (impl->stream)
		rtp_stream_destroy(impl->stream);
	if (impl->source)
		pw_loop_destroy_source(impl->data_loop, impl->source);

	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	if (impl->timer)
		pw_loop_destroy_source(impl->loop, impl->timer);

	pw_properties_free(impl->stream_props);
	pw_properties_free(impl->props);

	free(impl->ifname);
	free(impl);
}

static void module_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static void on_core_error(void *d, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = d;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

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
	struct impl *impl;
	const char *str, *sess_name;
	struct timespec value, interval;
	struct pw_properties *props, *stream_props;
	int64_t ts_offset;
	int res = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	if (args == NULL)
		args = "";

	props = impl->props = pw_properties_new_string(args);
	stream_props = impl->stream_props = pw_properties_new(NULL, NULL);
	if (props == NULL || stream_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto out;
	}

	impl->module = module;
	impl->context = context;
	impl->loop = pw_context_get_main_loop(context);
	impl->data_loop = pw_data_loop_get_loop(pw_context_get_data_loop(context));

	if ((sess_name = pw_properties_get(props, "sess.name")) == NULL)
		sess_name = pw_get_host_name();

	if (pw_properties_get(props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_NAME, "rtp_session.%s", sess_name);
	if (pw_properties_get(props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION, "%s", sess_name);
	if (pw_properties_get(props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_MEDIA_NAME, "RTP Session with %s",
				sess_name);

	if ((str = pw_properties_get(props, "stream.props")) != NULL)
		pw_properties_update_string(stream_props, str, strlen(str));

	copy_props(impl, props, PW_KEY_AUDIO_FORMAT);
	copy_props(impl, props, PW_KEY_AUDIO_RATE);
	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_NAME);
	copy_props(impl, props, PW_KEY_NODE_DESCRIPTION);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LATENCY);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, PW_KEY_NODE_CHANNELNAMES);
	copy_props(impl, props, PW_KEY_MEDIA_NAME);
	copy_props(impl, props, PW_KEY_MEDIA_CLASS);
	copy_props(impl, props, "net.mtu");
	copy_props(impl, props, "sess.media");
	copy_props(impl, props, "sess.name");
	copy_props(impl, props, "sess.min-ptime");
	copy_props(impl, props, "sess.max-ptime");
	copy_props(impl, props, "sess.latency.msec");
	copy_props(impl, props, "sess.ts-direct");
	copy_props(impl, props, "sess.ignore-ssrc");

	str = pw_properties_get(props, "local.ifname");
	impl->ifname = str ? strdup(str) : NULL;

	impl->src_port = pw_properties_get_uint32(props, "source.port", 0);
	if (impl->src_port == 0) {
		pw_log_error("invalid source.port");
		goto out;
	}
	if ((str = pw_properties_get(props, "source.ip")) == NULL)
		str = DEFAULT_SOURCE_IP;
	if ((res = parse_address(str, impl->src_port, &impl->src_addr, &impl->src_len)) < 0) {
		pw_log_error("invalid source.ip %s: %s", str, spa_strerror(res));
		goto out;
	}

	ts_offset = pw_properties_get_int64(props, "sess.ts-offset", DEFAULT_TS_OFFSET);
	if (ts_offset == -1)
		ts_offset = pw_rand32();
	pw_properties_setf(stream_props, "rtp.receiver-ts-offset", "%u", (uint32_t)ts_offset);

	impl->always_process = pw_properties_get_bool(stream_props,
			PW_KEY_NODE_ALWAYS_PROCESS, true);

	impl->cleanup_interval = pw_properties_get_uint32(props,
			"cleanup.sec", DEFAULT_CLEANUP_SEC);

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
		goto out;
	}

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener,
			&core_proxy_events, impl);
	pw_core_add_listener(impl->core,
			&impl->core_listener,
			&core_events, impl);

	impl->timer = pw_loop_add_timer(impl->loop, on_timer_event, impl);
	if (impl->timer == NULL) {
		res = -errno;
		pw_log_error("can't create timer source: %m");
		goto out;
	}
	value.tv_sec = impl->cleanup_interval;
	value.tv_nsec = 0;
	interval.tv_sec = impl->cleanup_interval;
	interval.tv_nsec = 0;
	pw_loop_update_timer(impl->loop, impl->timer, &value, &interval, false);

	impl->stream = rtp_stream_new(impl->core,
			PW_DIRECTION_OUTPUT, pw_properties_copy(stream_props),
			&stream_events, impl);
	if (impl->stream == NULL) {
		res = -errno;
		pw_log_error("can't create stream: %m");
		goto out;
	}

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_info));

	pw_log_info("Successfully loaded module-rtp-source");

	return 0;
out:
	impl_destroy(impl);
	return res;
}
