/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <net/if.h>
#include <ctype.h>

#include <spa/utils/hook.h>
#include <spa/utils/result.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/json.h>
#include <spa/param/audio/format-utils.h>
#include <spa/debug/types.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

#include <module-vban/stream.h>

#ifndef IPTOS_DSCP
#define IPTOS_DSCP_MASK 0xfc
#define IPTOS_DSCP(x) ((x) & IPTOS_DSCP_MASK)
#endif

/** \page page_module_vban_send VBAN sender
 *
 * The `vban-send` module creates a PipeWire sink that sends
 * audio and midi [VBAN](https://vb-audio.com) packets.
 *
 * ## Module Name
 *
 * `libpipewire-module-vban-send`
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `source.ip =<str>`: source IP address, default "0.0.0.0"
 * - `destination.ip =<str>`: destination IP address, default "127.0.0.1"
 * - `destination.port =<int>`: destination port, default 6980
 * - `local.ifname = <str>`: interface name to use
 * - `net.mtu = <int>`: MTU to use, default 1500
 * - `net.ttl = <int>`: TTL to use, default 1
 * - `net.loop = <bool>`: loopback multicast, default false
 * - `sess.min-ptime = <int>`: minimum packet time in milliseconds, default 2
 * - `sess.max-ptime = <int>`: maximum packet time in milliseconds, default 20
 * - `sess.name = <str>`: a session name
 * - `sess.media = <string>`: the media type audio|midi, default audio
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
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_MEDIA_NAME
 * - \ref PW_KEY_NODE_GROUP
 * - \ref PW_KEY_NODE_LATENCY
 * - \ref PW_KEY_NODE_VIRTUAL
 * - \ref PW_KEY_MEDIA_CLASS
 *
 * ## Example configuration
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-vban-send
 *     args = {
 *         #local.ifname = "eth0"
 *         #source.ip = "0.0.0.0"
 *         #destination.ip = "127.0.0.1"
 *         #destination.port = 6980
 *         #net.mtu = 1500
 *         #net.ttl = 1
 *         #net.loop = false
 *         #sess.min-ptime = 2
 *         #sess.max-ptime = 20
 *         #sess.name = "PipeWire VBAN stream"
 *         #sess.media = "audio"
 *         #audio.format = "S16LE"
 *         #audio.rate = 44100
 *         #audio.channels = 2
 *         #audio.position = [ FL FR ]
 *         stream.props = {
 *             node.name = "vban-sender"
 *         }
 *     }
 *}
 *]
 *\endcode
 *
 * \since 0.3.76
 */

#define NAME "vban-send"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define DEFAULT_PORT		6980
#define DEFAULT_SOURCE_IP	"0.0.0.0"
#define DEFAULT_DESTINATION_IP	"127.0.0.1"
#define DEFAULT_TTL		1
#define DEFAULT_LOOP		false
#define DEFAULT_DSCP		34 /* Default to AES-67 AF41 (34) */

#define USAGE	"( source.ip=<source IP address, default:"DEFAULT_SOURCE_IP"> ) "			\
		"( destination.ip=<destination IP address, default:"DEFAULT_DESTINATION_IP"> ) "	\
 		"( destination.port=<int, default:"SPA_STRINGIFY(DEFAULT_PORT)"> ) "			\
		"( local.ifname=<local interface name to use> ) "					\
		"( net.mtu=<desired MTU, default:"SPA_STRINGIFY(DEFAULT_MTU)"> ) "			\
		"( net.ttl=<desired TTL, default:"SPA_STRINGIFY(DEFAULT_TTL)"> ) "			\
		"( net.loop=<desired loopback, default:"SPA_STRINGIFY(DEFAULT_LOOP)"> ) "		\
		"( net.dscp=<desired DSCP, default:"SPA_STRINGIFY(DEFAULT_DSCP)"> ) "			\
		"( sess.name=<a name for the session> ) "						\
		"( sess.min-ptime=<minimum packet time in milliseconds, default:2> ) "			\
		"( sess.max-ptime=<maximum packet time in milliseconds, default:20> ) "			\
 		"( sess.media=<string, the media type audio|midi, default audio> ) "			\
		"( audio.format=<format, default:"DEFAULT_FORMAT"> ) "					\
		"( audio.rate=<sample rate, default:"SPA_STRINGIFY(DEFAULT_RATE)"> ) "			\
		"( audio.channels=<number of channels, default:"SPA_STRINGIFY(DEFAULT_CHANNELS)"> ) "	\
		"( audio.position=<channel map, default:"DEFAULT_POSITION"> ) "				\
		"( stream.props= { key=value ... } ) "

static const struct spa_dict_item module_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "VBAN Sender" },
	{ PW_KEY_MODULE_USAGE, USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_context *context;

	struct pw_impl_module *module;
	struct spa_hook module_listener;
	struct pw_properties *props;

	struct pw_loop *loop;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;

	struct pw_properties *stream_props;
	struct vban_stream *stream;

	unsigned int do_disconnect:1;

	char *ifname;
	char *session_name;
	uint32_t ttl;
	bool mcast_loop;
	uint32_t dscp;

	struct sockaddr_storage src_addr;
	socklen_t src_len;

	uint16_t dst_port;
	struct sockaddr_storage dst_addr;
	socklen_t dst_len;

	int vban_fd;
};

static void stream_destroy(void *d)
{
	struct impl *impl = d;
	impl->stream = NULL;
}

static void stream_send_packet(void *data, struct iovec *iov, size_t iovlen)
{
	struct impl *impl = data;
	struct msghdr msg;
	ssize_t n;

	spa_zero(msg);
	msg.msg_iov = iov;
	msg.msg_iovlen = iovlen;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = 0;

	n = sendmsg(impl->vban_fd, &msg, MSG_NOSIGNAL);
	if (n < 0)
		pw_log_debug("sendmsg() failed: %m");
}

static void stream_state_changed(void *data, bool started, const char *error)
{
	struct impl *impl = data;

	if (error) {
		pw_log_error("stream error: %s", error);
		pw_impl_module_schedule_destroy(impl->module);
	}
}

static const struct vban_stream_events stream_events = {
	VBAN_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.send_packet = stream_send_packet,
};

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

	af = src->ss_family;
	if ((fd = socket(af, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
		pw_log_error("socket failed: %m");
		return -errno;
	}
	if (bind(fd, (struct sockaddr*)src, src_len) < 0) {
		res = -errno;
		pw_log_error("bind() failed: %m");
		goto error;
	}
	if (connect(fd, (struct sockaddr*)dst, dst_len) < 0) {
		res = -errno;
		pw_log_error("connect() failed: %m");
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
#ifdef SO_PRIORITY
	val = 6;
	if (setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &val, sizeof(val)) < 0)
		pw_log_warn("setsockopt(SO_PRIORITY) failed: %m");
#endif
	if (dscp > 0) {
		val = IPTOS_DSCP(dscp << 2);
		if (setsockopt(fd, IPPROTO_IP, IP_TOS, &val, sizeof(val)) < 0)
			pw_log_warn("setsockopt(IP_TOS) failed: %m");
	}


	return fd;
error:
	close(fd);
	return res;
}

static int get_ip(const struct sockaddr_storage *sa, char *ip, size_t len)
{
	if (sa->ss_family == AF_INET) {
		struct sockaddr_in *in = (struct sockaddr_in*)sa;
		inet_ntop(sa->ss_family, &in->sin_addr, ip, len);
	} else if (sa->ss_family == AF_INET6) {
		struct sockaddr_in6 *in = (struct sockaddr_in6*)sa;
		inet_ntop(sa->ss_family, &in->sin6_addr, ip, len);
	} else
		return -EIO;
	return 0;
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
		vban_stream_destroy(impl->stream);

	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	if (impl->vban_fd != -1)
		close(impl->vban_fd);

	pw_properties_free(impl->stream_props);
	pw_properties_free(impl->props);

	free(impl->ifname);
	free(impl->session_name);
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
	struct pw_properties *props = NULL, *stream_props = NULL;
	char addr[64];
	const char *str, *sess_name;
	int res = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->vban_fd = -1;

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto out;
	}
	impl->props = props;

	stream_props = pw_properties_new(NULL, NULL);
	if (stream_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto out;
	}
	impl->stream_props = stream_props;

	impl->module = module;
	impl->context = context;
	impl->loop = pw_context_get_main_loop(context);

	if ((sess_name = pw_properties_get(props, "sess.name")) == NULL)
		sess_name = pw_get_host_name();

	if (pw_properties_get(props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_NAME, "vban_session.%s", sess_name);
	if (pw_properties_get(props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION, "%s", sess_name);
	if (pw_properties_get(props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_MEDIA_NAME, "VBAN Session with %s",
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
	copy_props(impl, props, "sess.ts-refclk");

	str = pw_properties_get(props, "local.ifname");
	impl->ifname = str ? strdup(str) : NULL;

	if ((str = pw_properties_get(props, "source.ip")) == NULL)
		str = DEFAULT_SOURCE_IP;
	if ((res = parse_address(str, 0, &impl->src_addr, &impl->src_len)) < 0) {
		pw_log_error("invalid source.ip %s: %s", str, spa_strerror(res));
		goto out;
	}

	impl->dst_port = pw_properties_get_uint32(props, "destination.port", DEFAULT_PORT);
	if ((str = pw_properties_get(props, "destination.ip")) == NULL)
		str = DEFAULT_DESTINATION_IP;
	if ((res = parse_address(str, impl->dst_port, &impl->dst_addr, &impl->dst_len)) < 0) {
		pw_log_error("invalid destination.ip %s: %s", str, spa_strerror(res));
		goto out;
	}

	impl->ttl = pw_properties_get_uint32(props, "net.ttl", DEFAULT_TTL);
	impl->mcast_loop = pw_properties_get_bool(props, "net.loop", DEFAULT_LOOP);
	impl->dscp = pw_properties_get_uint32(props, "net.dscp", DEFAULT_DSCP);

	get_ip(&impl->src_addr, addr, sizeof(addr));
	pw_properties_set(stream_props, "vban.source.ip", addr);
	get_ip(&impl->dst_addr, addr, sizeof(addr));
	pw_properties_set(stream_props, "vban.destination.ip", addr);
	pw_properties_setf(stream_props, "vban.destination.port", "%u", impl->dst_port);
	pw_properties_setf(stream_props, "vban.ttl", "%u", impl->ttl);
	pw_properties_setf(stream_props, "vban.dscp", "%u", impl->dscp);

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

	if ((res = make_socket(&impl->src_addr, impl->src_len,
					&impl->dst_addr, impl->dst_len,
					impl->mcast_loop, impl->ttl, impl->dscp)) < 0) {
		pw_log_error("can't make socket: %s", spa_strerror(res));
		goto out;
	}
	impl->vban_fd = res;

	impl->stream = vban_stream_new(impl->core,
			PW_DIRECTION_INPUT, pw_properties_copy(stream_props),
			&stream_events, impl);
	if (impl->stream == NULL) {
		res = -errno;
		pw_log_error("can't create stream: %m");
		goto out;
	}

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_info));

	pw_log_info("Successfully loaded module-vban-send");

	return 0;
out:
	impl_destroy(impl);
	return res;
}
