/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans <wim.taymans@gmail.com> */
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

#include <module-vban/stream.h>
#include <module-vban/vban.h>
#include "network-utils.h"

/** \page page_module_vban_recv VBAN receiver
 *
 * The `vban-recv` module creates a PipeWire source that receives audio
 * and midi [VBAN](https://vb-audio.com) packets.
 *
 * The receive will listen on a specific port (6980) and create a stream for each
 * VBAN stream received on the port.
 *
 * ## Module Name
 *
 * `libpipewire-module-vban-recv`
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `local.ifname = <str>`: interface name to use
 * - `source.ip = <str>`: the source ip address to listen on, default 127.0.0.1
 * - `source.port = <int>`: the source port to listen on, default 6980
 * - `node.always-process = <bool>`: true to receive even when not running
 * - `sess.latency.msec = <str>`: target network latency in milliseconds, default 100
 * - `stream.props = {}`: properties to be passed to all the stream
 * - `stream.rules` = <rules>: match rules, use create-stream actions.
 *
 * ### stream.rules matches
 *
 *  - `vban.ip`: the IP address of the VBAN sender
 *  - `vban.port`: the port of the VBAN sender
 *  - `sess.name`: the name of the VBAN stream
 *
 * ### stream.rules create-stream
 *
 * In addition to all the properties that can be passed to a stream,
 * you can also set:
 *
 * - `sess.latency.msec = <str>`: target network latency in milliseconds, default 100
 *
 * ## General options
 *
 * Options with well-known behavior:
 *
 * - \ref PW_KEY_REMOTE_NAME
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
 * # ~/.config/pipewire/pipewire.conf.d/my-vban-recv.conf
 *
 * context.modules = [
 * {   name = libpipewire-module-vban-recv
 *     args = {
 *         #local.ifname = eth0
 *         #source.ip = 127.0.0.1
 *         #source.port = 6980
 *         sess.latency.msec = 100
 *         #node.always-process = false
 *         #audio.position = [ FL FR ]
 *         stream.props = {
 *            #media.class = "Audio/Source"
 *            #node.name = "vban-receiver"
 *         }
 *         stream.rules = [
 *             {   matches = [
 *                     {    sess.name = "~.*"
 *                          #sess.media = "audio" | "midi"
 *                          #vban.ip = ""
 *                          #vban.port = 1000
 *                          #audio.channels = 2
 *                          #audio.format = "U8" | "S16LE" | "S24LE" | "S32LE" | "F32LE" | "F64LE"
 *                          #audio.rate = 44100
 *                     }
 *                 ]
 *                 actions = {
 *                     create-stream = {
 *                         stream.props = {
 *                             #sess.latency.msec = 100
 *                             #target.object = ""
 *                             #audio.position = [ FL FR ]
 *                             #media.class = "Audio/Source"
 *                             #node.name = "vban-receiver"
 *                         }
 *                     }
 *                 }
 *             }
 *         ]
 *     }
 * }
 * ]
 *\endcode
 *
 * \since 0.3.76
 */

#define NAME "vban-recv"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define DEFAULT_CLEANUP_SEC		60
#define DEFAULT_SOURCE_IP		"127.0.0.1"
#define DEFAULT_SOURCE_PORT		6980

#define DEFAULT_CREATE_RULES	\
        "[ { matches = [ { sess.name = \"~.*\" } ] actions = { create-stream = { } } } ] "

#define USAGE   "( local.ifname=<local interface name to use> ) "						\
		"( source.ip=<source IP address, default:"DEFAULT_SOURCE_IP"> ) "				\
 		"( source.port=<int, source port, default:"SPA_STRINGIFY(DEFAULT_SOURCE_PORT)"> "		\
		"( sess.latency.msec=<target network latency, default "SPA_STRINGIFY(DEFAULT_SESS_LATENCY)"> ) "\
		"( audio.position=<channel map, default:"DEFAULT_POSITION"> ) "					\
		"( stream.props= { key=value ... } ) "								\
		"( stream.rules=<rules>, use create-stream actions )"

static const struct spa_dict_item module_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "VBAN Receiver" },
	{ PW_KEY_MODULE_USAGE,	USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_impl_module *module;
	struct spa_hook module_listener;
	struct pw_properties *props;
	struct pw_context *context;

	struct pw_loop *main_loop;
	struct pw_loop *data_loop;
	struct pw_timer_queue *timer_queue;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;
	unsigned int do_disconnect:1;

	char *ifname;
	bool always_process;
	uint32_t cleanup_interval;

	struct pw_properties *stream_props;

	struct pw_timer timer;

	uint16_t src_port;
	struct sockaddr_storage src_addr;
	socklen_t src_len;
	struct spa_source *source;

	struct spa_list streams;
};

struct stream {
	struct spa_list link;
	struct impl *impl;

	struct vban_header header;
	struct sockaddr_storage sa;
	socklen_t salen;

	struct vban_stream *stream;

	bool active;
	bool receiving;
};

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

static void stream_destroy(void *d)
{
	struct stream *s = d;
	s->stream = NULL;
}

static void stream_state_changed(void *data, bool started, const char *error)
{
	struct stream *s = data;
	struct impl *impl = s->impl;

	if (error) {
		pw_log_error("stream error: %s", error);
		pw_impl_module_schedule_destroy(impl->module);
	} else if (started) {
		s->active = true;
	} else {
		s->active = false;
	}
}

static const struct vban_stream_events stream_events = {
	VBAN_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
};

static int create_stream(struct stream *s, struct pw_properties *props)
{
	struct impl *impl = s->impl;
	const char *sess_name, *ip, *port;

	ip = pw_properties_get(props, "vban.ip");
	port = pw_properties_get(props, "vban.port");
	sess_name = pw_properties_get(props, "sess.name");

	if (pw_properties_get(props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_NAME, "vban_session.%s.%s.%s", sess_name, ip, port);
	if (pw_properties_get(props, PW_KEY_NODE_DESCRIPTION) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION, "%s from %s", sess_name, ip);
	if (pw_properties_get(props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(props, PW_KEY_MEDIA_NAME, "VBAN %s from %s",
				sess_name, ip);

	s->stream = vban_stream_new(impl->core,
			PW_DIRECTION_OUTPUT, spa_steal_ptr(props),
			&stream_events, s);
	if (s->stream == NULL) {
		pw_log_error("can't create stream: %m");
		return -errno;
	}
	return 0;
}

struct match_info {
	struct stream *stream;
	const struct pw_properties *props;
	bool matched;
};

static int rule_matched(void *data, const char *location, const char *action,
                        const char *str, size_t len)
{
	struct match_info *i = data;
	int res = 0;

	i->matched = true;
	if (spa_streq(action, "create-stream")) {
		struct pw_properties *p = pw_properties_copy(i->props);
		pw_properties_update_string(p, str, len);
		create_stream(i->stream, p);
	}
	return res;
}


static int
do_setup_stream(struct spa_loop *loop,
	bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
        struct stream *s = user_data;
	struct impl *impl = s->impl;
	struct pw_properties *props;
	int res;
	const char *str;
	char addr[128];
	uint16_t port = 0;

	props = pw_properties_copy(impl->stream_props);

	pw_net_get_ip(&s->sa, addr, sizeof(addr), NULL, &port);

	pw_properties_setf(props, "sess.name", "%s", s->header.stream_name);
	pw_properties_setf(props, "vban.ip", "%s", addr);
	pw_properties_setf(props, "vban.port", "%u", port);

	if ((s->header.format_SR & 0xE0) == VBAN_PROTOCOL_AUDIO &&
	    (s->header.format_bit & 0xF0) == VBAN_CODEC_PCM) {
		const char *fmt;
		pw_properties_set(props, "sess.media", "audio");
		pw_properties_setf(props, PW_KEY_AUDIO_CHANNELS, "%u",s->header.format_nbc + 1);
		pw_properties_setf(props, PW_KEY_AUDIO_RATE, "%u", vban_SR[s->header.format_SR & 0x1f]);
		switch(s->header.format_bit & 0x07) {
		case VBAN_DATATYPE_BYTE8:
			fmt = "U8";
			break;
		case VBAN_DATATYPE_INT16:
			fmt = "S16LE";
			break;
		case VBAN_DATATYPE_INT24:
			fmt = "S24LE";
			break;
		case VBAN_DATATYPE_INT32:
			fmt = "S32LE";
			break;
		case VBAN_DATATYPE_FLOAT32:
			fmt = "F32LE";
			break;
		case VBAN_DATATYPE_FLOAT64:
			fmt = "F64LE";
			break;
			break;
		case VBAN_DATATYPE_12BITS:
		case VBAN_DATATYPE_10BITS:
		default:
			pw_log_error("stream format %08x:%08x not supported",
					s->header.format_SR, s->header.format_bit);
			res = -ENOTSUP;
			goto error;
		}
		pw_properties_set(props, PW_KEY_AUDIO_FORMAT, fmt);
	} else if ((s->header.format_SR & 0xE0) == VBAN_PROTOCOL_SERIAL &&
	    (s->header.format_bit & 0xF0) == VBAN_SERIAL_MIDI) {
		pw_properties_set(props, "sess.media", "midi");
	} else {
		pw_log_error("stream format %08x:%08x not supported",
				s->header.format_SR, s->header.format_bit);
		res = -ENOTSUP;
		goto error;
	}

	if ((str = pw_properties_get(impl->props, "stream.rules")) == NULL)
		str = DEFAULT_CREATE_RULES;
	if (str != NULL) {
		struct match_info minfo = {
			.stream = s,
			.props = props,
		};
		pw_conf_match_rules(str, strlen(str), NAME, &props->dict,
				rule_matched, &minfo);

		if (!minfo.matched)
			pw_log_info("unmatched stream found %s", str);
	}
	res = 0;
error:
	pw_properties_free(props);
	return res;
}

static struct stream *make_stream(struct impl *impl, const struct vban_header *hdr,
		struct sockaddr_storage *sa, socklen_t salen)
{
	struct stream *stream;

	stream = calloc(1, sizeof(*stream));
	if (stream == NULL)
		return NULL;

	stream->impl = impl;
	stream->header = *hdr;
	stream->sa = *sa;
	stream->salen = salen;
	spa_list_append(&impl->streams, &stream->link);

	pw_loop_invoke(impl->main_loop, do_setup_stream, 1, NULL, 0, false, stream);

	return stream;
}

static struct stream *find_stream(struct impl *impl, const char *name)
{
	struct stream *s;
	spa_list_for_each(s, &impl->streams, link) {
		if (strncmp(s->header.stream_name, name, VBAN_STREAM_NAME_SIZE) == 0)
			return s;
	}
	return NULL;
}

static void
on_vban_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;
	ssize_t len;
	uint8_t buffer[2048];

	if (mask & SPA_IO_IN) {
		struct vban_header *hdr;
		struct stream *s;
		struct sockaddr_storage sa;
		socklen_t salen = sizeof(sa);

		if ((len = recvfrom(fd, buffer, sizeof(buffer), 0,
						(struct sockaddr*)&sa, &salen)) < 0)
			goto receive_error;

		if (len < VBAN_HEADER_SIZE)
			goto short_packet;

		hdr = (struct vban_header *)buffer;
		if (strncmp(hdr->vban, "VBAN", 4))
			goto invalid_version;

		s = find_stream(impl, hdr->stream_name);
		if (SPA_UNLIKELY(s == NULL))
			s = make_stream(impl, hdr, &sa, salen);
		if (SPA_LIKELY(s != NULL && s->active)) {
			s->receiving = true;
			vban_stream_receive_packet(s->stream, buffer, len);
		}
	}
	return;

receive_error:
	pw_log_warn("recv error: %m");
	return;
short_packet:
	pw_log_warn("short packet received");
	return;
invalid_version:
	pw_log_warn("invalid VBAN version");
	return;
}

static int listen_start(struct impl *impl)
{
	int fd;

	if (impl->source != NULL)
		return 0;

	pw_log_info("starting VBAN listener");

	if ((fd = make_socket((const struct sockaddr *)&impl->src_addr,
					impl->src_len, impl->ifname)) < 0) {
		pw_log_error("failed to create socket: %m");
		return fd;
	}

	impl->source = pw_loop_add_io(impl->data_loop, fd,
				SPA_IO_IN, true, on_vban_io, impl);
	if (impl->source == NULL) {
		pw_log_error("can't create io source: %m");
		close(fd);
		return -errno;
	}
	return 0;
}

static void listen_stop(struct impl *impl)
{
	if (!impl->source)
		return;

	pw_log_info("stopping VBAN listener");

	pw_loop_destroy_source(impl->data_loop, impl->source);
	impl->source = NULL;
}


static void destroy_stream(struct stream *s)
{
	spa_list_remove(&s->link);
	if (s->stream)
		vban_stream_destroy(s->stream);
	free(s);
}

static void on_timer_event(void *data)
{
	struct impl *impl = data;
	struct stream *s;

	spa_list_for_each(s, &impl->streams, link) {
		if (!s->receiving) {
			pw_log_info("timeout, inactive VBAN source");
			//pw_impl_module_schedule_destroy(impl->module);
		} else {
			pw_log_debug("timeout, keeping active VBAN source");
		}
		s->receiving = false;
	}
	pw_timer_queue_add(impl->timer_queue, &impl->timer,
			&impl->timer.timeout, impl->cleanup_interval * SPA_NSEC_PER_SEC,
			on_timer_event, impl);
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
	struct stream *s;

	listen_stop(impl);

	spa_list_consume(s, &impl->streams, link)
		destroy_stream(s);

	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	pw_timer_queue_cancel(&impl->timer);

	if (impl->data_loop)
		pw_context_release_loop(impl->context, impl->data_loop);

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
	const char *str;
	struct pw_properties *props, *stream_props;
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
	impl->main_loop = pw_context_get_main_loop(context);
	impl->timer_queue = pw_context_get_timer_queue(context);
	impl->data_loop = pw_context_acquire_loop(context, &props->dict);
	spa_list_init(&impl->streams);

	pw_properties_set(props, PW_KEY_NODE_LOOP_NAME, impl->data_loop->name);

	if ((str = pw_properties_get(props, "stream.props")) != NULL)
		pw_properties_update_string(stream_props, str, strlen(str));

	copy_props(impl, props, PW_KEY_NODE_LOOP_NAME);
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
	copy_props(impl, props, "sess.latency.msec");

	str = pw_properties_get(props, "local.ifname");
	impl->ifname = str ? strdup(str) : NULL;

	impl->src_port = pw_properties_get_uint32(props, "source.port", DEFAULT_SOURCE_PORT);
	if (impl->src_port == 0) {
		pw_log_error("invalid source.port");
		goto out;
	}
	if ((str = pw_properties_get(props, "source.ip")) == NULL)
		str = DEFAULT_SOURCE_IP;
	if ((res = pw_net_parse_address(str, impl->src_port, &impl->src_addr, &impl->src_len)) < 0) {
		pw_log_error("invalid source.ip %s: %s", str, spa_strerror(res));
		goto out;
	}

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

	if ((res = pw_timer_queue_add(impl->timer_queue, &impl->timer,
			NULL, impl->cleanup_interval * SPA_NSEC_PER_SEC,
			on_timer_event, impl)) < 0) {
		pw_log_error("can't add timer: %s", spa_strerror(res));
		goto out;
	}

	if ((res = listen_start(impl)) < 0) {
		pw_log_error("failed to start VBAN stream: %s", spa_strerror(res));
		goto out;
	}

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_info));

	pw_log_info("Successfully loaded module-vban-recv");

	return 0;
out:
	impl_destroy(impl);
	return res;
}
