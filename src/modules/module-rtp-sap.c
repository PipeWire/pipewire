/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
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
#include <spa/debug/types.h>
#include <spa/debug/dict.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

#include <module-rtp/sap.h>
#include <module-rtp/rtp.h>


/** \page page_module_rtp_sap PipeWire Module: Announce and receive RTP streams
 *
 * The `rtp-sap` module announces RTP stream with the sess.sap.announce property
 * set to true.
 *
 * It will also create source rtp streams that are announced with SAP when they
 * match the pattern.
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `sap.ip = <str>`: IP address of the SAP messages, default "224.0.0.56"
 * - `sap.port = <int>`: port of the SAP messages, default 9875
 * - `source.ip =<str>`: source IP address, default "0.0.0.0"
 * - `destination.ip =<str>`: destination IP address, default "224.0.0.56"
 * - `destination.port =<int>`: destination port, default random beteen 46000 and 47024
 * - `local.ifname = <str>`: interface name to use
 * - `net.ttl = <int>`: TTL to use, default 1
 * - `net.loop = <bool>`: loopback multicast, default false
 *
 * ## General options
 *
 * Options with well-known behavior:
 *
 * - \ref PW_KEY_REMOTE_NAME
 *
 * ## Example configuration
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-rtp-sap
 *     args = {
 *         #sap.ip = "224.0.0.56"
 *         #sap.port = 9875
 *         #source.ip = "0.0.0.0"
 *         #local.ifname = "eth0"
 *         #net.ttl = 1
 *         #net.loop = false
 *         stream.props = {
 *            #media.class = "Audio/Source"
 *            #node.name = "rtp-source"
 *         }
 *         stream.rules = [
 *             {   matches = [
 *                     # any of the items in matches needs to match, if one does,
 *                     # actions are emited.
 *                     {   # all keys must match the value. ~ in value starts regex.
 *                         #rtp.origin = "wim 3883629975 0 IN IP4 0.0.0.0"
 *                         #rtp.payload = "127"
 *                         #rtp.fmt = "L16/48000/2"
 *                         #rtp.session = "PipeWire RTP Stream on fedora"
 *                         #rtp.ts-offset = 0
 *                         #rtp.ts-refclk = "private"
 *                     }
 *                 ]
 *                 actions = {
 *                     create-stream = {
 *                         #sess.latency.msec = 100
 *                         #sess.ts-direct = false
 *                         #target.object = ""
 *                     }
 *                 }
 *             }
 *         ]
 *     }
 *     }
 *}
 *]
 *\endcode
 *
 * \since 0.3.67
 */

#define NAME "rtp-sap"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define SAP_INTERVAL_SEC	5
#define SAP_MIME_TYPE		"application/sdp"

#define DEFAULT_SAP_IP		"224.0.0.56"
#define DEFAULT_SAP_PORT	9875

#define DEFAULT_PORT		46000
#define DEFAULT_SOURCE_IP	"0.0.0.0"
#define DEFAULT_DESTINATION_IP	"224.0.0.56"
#define DEFAULT_TTL		1
#define DEFAULT_LOOP		false

#define USAGE	"sap.ip=<SAP IP address to send announce, default:"DEFAULT_SAP_IP"> "		\
		"sap.port=<SAP port to send on, default:"SPA_STRINGIFY(DEFAULT_SAP_PORT)"> "	\
		"source.ip=<source IP address, default:"DEFAULT_SOURCE_IP"> "			\
		"destination.ip=<destination IP address, default:"DEFAULT_DESTINATION_IP"> "	\
		"local.ifname=<local interface name to use> "					\
		"net.ttl=<desired TTL, default:"SPA_STRINGIFY(DEFAULT_TTL)"> "			\
		"net.loop=<desired loopback, default:"SPA_STRINGIFY(DEFAULT_LOOP)"> "

static const struct spa_dict_item module_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "RTP Sink" },
	{ PW_KEY_MODULE_USAGE, USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct session {
	struct spa_list link;

	struct impl *impl;
	struct node *node;

	uint16_t msg_id_hash;
	uint32_t ntp;

	uint32_t ts_offset;
	char *ts_refclk;

	char *media_type;
	char *mime_type;
	char *session_name;
	int payload;
	uint32_t rate;
	uint32_t channels;
	float ptime;

	uint16_t dst_port;
	struct sockaddr_storage dst_addr;
	socklen_t dst_len;
	uint16_t ttl;

	unsigned has_sent_sap:1;

	struct pw_properties *props;
	struct pw_impl_module *module;
};

struct node {
	struct impl *impl;

	uint32_t id;

	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	struct spa_hook node_listener;

	struct pw_node_info *info;
	struct session *session;
};


struct impl {
	struct pw_properties *props;

	struct pw_loop *loop;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct spa_source *timer;

	unsigned int do_disconnect:1;

	char *ifname;
	bool ttl;
	bool mcast_loop;

	struct sockaddr_storage src_addr;
	socklen_t src_len;

	uint16_t sap_port;
	struct sockaddr_storage sap_addr;
	socklen_t sap_len;

	int sap_fd;

	struct spa_list sessions;
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
		bool loop, int ttl)
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
static void send_sap(struct impl *impl, struct session *sess, bool bye)
{
	char buffer[2048], src_addr[64], dst_addr[64], dst_ttl[8];
	const char *user_name, *af;
	struct sockaddr *sa = (struct sockaddr*)&impl->src_addr;
	struct sap_header header;
	struct iovec iov[4];
	struct msghdr msg;
	struct spa_strbuf buf;

	if (!sess->has_sent_sap && bye)
		return;

	spa_zero(header);
	header.v = 1;
	header.t = bye;
	header.msg_id_hash = sess->msg_id_hash;

	iov[0].iov_base = &header;
	iov[0].iov_len = sizeof(header);

	if (sa->sa_family == AF_INET) {
		iov[1].iov_base = &((struct sockaddr_in*) sa)->sin_addr;
		iov[1].iov_len = 4U;
		af = "IP4";
	} else {
		iov[1].iov_base = &((struct sockaddr_in6*) sa)->sin6_addr;
		iov[1].iov_len = 16U;
		header.a = 1;
		af = "IP6";
	}
	iov[2].iov_base = SAP_MIME_TYPE;
	iov[2].iov_len = sizeof(SAP_MIME_TYPE);

	get_ip(&impl->src_addr, src_addr, sizeof(src_addr));
	get_ip(&sess->dst_addr, dst_addr, sizeof(dst_addr));

	if ((user_name = pw_get_user_name()) == NULL)
		user_name = "-";

	spa_zero(dst_ttl);
	if (is_multicast((struct sockaddr*)&sess->dst_addr, sess->dst_len))
		snprintf(dst_ttl, sizeof(dst_ttl), "/%d", sess->ttl);

	spa_strbuf_init(&buf, buffer, sizeof(buffer));
	spa_strbuf_append(&buf,
			"v=0\n"
			"o=%s %u 0 IN %s %s\n"
			"s=%s\n"
			"c=IN %s %s%s\n"
			"t=%u 0\n"
			"a=recvonly\n"
			"a=tool:PipeWire %s\n"
			"a=type:broadcast\n",
			user_name, sess->ntp, af, src_addr,
			sess->session_name,
			af, dst_addr, dst_ttl,
			sess->ntp,
			pw_get_library_version());
	spa_strbuf_append(&buf,
			"m=%s %u RTP/AVP %i\n",
			sess->media_type,
			sess->dst_port, sess->payload);

	if (sess->channels) {
		spa_strbuf_append(&buf,
			"a=rtpmap:%i %s/%u/%u\n",
				sess->payload, sess->mime_type,
				sess->rate, sess->channels);
	} else {
		spa_strbuf_append(&buf,
			"a=rtpmap:%i %s/%u\n",
				sess->payload, sess->mime_type, sess->rate);
	}
	if (sess->ptime != 0)
		spa_strbuf_append(&buf,
			"a=ptime:%f\n", sess->ptime);

	if (sess->ts_refclk != NULL) {
		spa_strbuf_append(&buf,
				"a=ts-refclk:%s\n"
				"a=mediaclk:direct=%u\n",
				sess->ts_refclk,
				sess->ts_offset);
	} else {
		spa_strbuf_append(&buf, "a=mediaclk:sender\n");
	}

	iov[3].iov_base = buffer;
	iov[3].iov_len = strlen(buffer);

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = iov;
	msg.msg_iovlen = 4;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = 0;

	sendmsg(impl->sap_fd, &msg, MSG_NOSIGNAL);

	sess->has_sent_sap = true;
}

static void on_timer_event(void *data, uint64_t expirations)
{
	struct impl *impl = data;
	struct session *sess;

	spa_list_for_each(sess, &impl->sessions, link) {
		send_sap(impl, sess, 0);
	}
}

static int start_sap_announce(struct impl *impl)
{
	int fd, res;
	struct timespec value, interval;

	if ((fd = make_socket(&impl->src_addr, impl->src_len,
					&impl->sap_addr, impl->sap_len,
					impl->mcast_loop, impl->ttl)) < 0)
		return fd;

	impl->sap_fd = fd;

	pw_log_info("starting SAP timer");
	impl->timer = pw_loop_add_timer(impl->loop, on_timer_event, impl);
	if (impl->timer == NULL) {
		res = -errno;
		pw_log_error("can't create timer source: %m");
		goto error;
	}
	value.tv_sec = 0;
	value.tv_nsec = 1;
	interval.tv_sec = SAP_INTERVAL_SEC;
	interval.tv_nsec = 0;
	pw_loop_update_timer(impl->loop, impl->timer, &value, &interval, false);

	return 0;
error:
	close(fd);
	return res;
}

static struct session *session_create(struct impl *impl, struct node *node)
{
	struct session *sess = NULL;
	const char *str;
	uint32_t port;
	int res;

	sess = calloc(1, sizeof(struct session));
	if (sess == NULL)
		return NULL;

	sess->impl = impl;
	sess->node = node;
	sess->msg_id_hash = rand();
	sess->ntp = (uint32_t) time(NULL) + 2208988800U;

	sess->props = pw_properties_new_dict(node->info->props);
	if (sess->props == NULL)
		goto error_free;

	if ((str = pw_properties_get(sess->props, "rtp.session")) != NULL)
		sess->session_name = strdup(str);

	if ((str = pw_properties_get(sess->props, "rtp.destination.port")) == NULL)
		goto error_free;
	if (!spa_atou32(str, &port, 0))
		goto error_free;
	sess->dst_port = port;

	if ((str = pw_properties_get(sess->props, "rtp.destination.ip")) == NULL)
		goto error_free;
	if ((res = parse_address(str, sess->dst_port, &sess->dst_addr, &sess->dst_len)) < 0) {
		pw_log_error("invalid destination.ip %s: %s", str, spa_strerror(res));
		goto error_free;
	}
	sess->ttl = pw_properties_get_int32(sess->props, "rtp.ttl", DEFAULT_TTL);
	sess->payload = pw_properties_get_int32(sess->props, "rtp.payload", 127);

	if ((str = pw_properties_get(sess->props, "rtp.media")) != NULL)
		sess->media_type = strdup(str);
	if ((str = pw_properties_get(sess->props, "rtp.mime")) != NULL)
		sess->mime_type = strdup(str);
	if ((str = pw_properties_get(sess->props, "rtp.rate")) != NULL)
		sess->rate = atoi(str);
	if ((str = pw_properties_get(sess->props, "rtp.channels")) != NULL)
		sess->channels = atoi(str);
	if ((str = pw_properties_get(sess->props, "rtp.ts-offset")) != NULL)
		sess->ts_offset = atoi(str);
	if ((str = pw_properties_get(sess->props, "rtp.ts-refclk")) != NULL)
		sess->ts_refclk = strdup(str);

	spa_list_append(&impl->sessions, &sess->link);
	return sess;

error_free:
	pw_log_warn("invalid session props");
	pw_properties_free(sess->props);
	free(sess->session_name);
	free(sess);
	return NULL;
}

static void session_free(struct session *sess)
{
	struct impl *impl = sess->impl;

	send_sap(impl, sess, 1);

	spa_list_remove(&sess->link);

	free(sess->session_name);
	free(sess);
}

static void node_event_info(void *data, const struct pw_node_info *info)
{
	struct node *n = data;
	const char *str;

	pw_log_info("node %d added %p", n->id, n->session);

	if (n->session != NULL || info == NULL)
		return;

	n->info = pw_node_info_merge(n->info, info, true);
	if (n->info == NULL)
		return;

	spa_debug_dict(0, info->props);

	if ((str = spa_dict_lookup(info->props, "sess.sap.announce")) == NULL ||
	    !pw_properties_parse_bool(str))
		return;

	session_create(n->impl, n);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_event_info,
};

static void
proxy_removed(void *data)
{
	struct node *n = data;
	pw_log_info("node %d removed", n->id);
	if (n->session != NULL)
		session_free(n->session);
	pw_proxy_destroy(n->proxy);
}

static void
proxy_destroy(void *data)
{
	struct node *n = data;
	pw_log_info("node %d destroy", n->id);
	spa_hook_remove(&n->node_listener);
	spa_hook_remove(&n->proxy_listener);
	n->proxy = NULL;
	if (n->info)
		pw_node_info_free(n->info);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = proxy_removed,
	.destroy = proxy_destroy,
};

static void registry_event_global(void *data, uint32_t id,
			uint32_t permissions, const char *type, uint32_t version,
			const struct spa_dict *props)
{
	struct impl *impl = data;
	struct pw_proxy *proxy;
	struct node *node;

	if (!spa_streq(type, PW_TYPE_INTERFACE_Node))
		return;

	proxy = pw_registry_bind(impl->registry, id, type, PW_VERSION_NODE, sizeof(struct node));
	if (proxy == NULL)
		return;

	node = pw_proxy_get_user_data(proxy);
	node->impl = impl;
	node->id = id;
	node->proxy = proxy;

	pw_proxy_add_object_listener(proxy,
			&node->node_listener, &node_events, node);
        pw_proxy_add_listener(proxy,
                        &node->proxy_listener, &proxy_events, node);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
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
	struct session *sess;

	spa_list_consume(sess, &impl->sessions, link)
		session_free(sess);

	if (impl->registry) {
		spa_hook_remove(&impl->registry_listener);
		pw_proxy_destroy((struct pw_proxy*)impl->registry);
		impl->registry = NULL;
	}
	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	if (impl->timer)
		pw_loop_destroy_source(impl->loop, impl->timer);

	if (impl->sap_fd != -1)
		close(impl->sap_fd);

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

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	struct pw_properties *props = NULL;
	uint32_t port;
	const char *str;
	int res = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->sap_fd = -1;
	spa_list_init(&impl->sessions);

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto out;
	}
	impl->props = props;

	impl->module = module;
	impl->loop = pw_context_get_main_loop(context);

	str = pw_properties_get(props, "local.ifname");
	impl->ifname = str ? strdup(str) : NULL;

	if ((str = pw_properties_get(props, "sap.ip")) == NULL)
		str = DEFAULT_SAP_IP;
	port = pw_properties_get_uint32(props, "sap.port", DEFAULT_SAP_PORT);
	if ((res = parse_address(str, port, &impl->sap_addr, &impl->sap_len)) < 0) {
		pw_log_error("invalid sap.ip %s: %s", str, spa_strerror(res));
		goto out;
	}

	if ((str = pw_properties_get(props, "source.ip")) == NULL)
		str = DEFAULT_SOURCE_IP;
	if ((res = parse_address(str, 0, &impl->src_addr, &impl->src_len)) < 0) {
		pw_log_error("invalid source.ip %s: %s", str, spa_strerror(res));
		goto out;
	}

	impl->ttl = pw_properties_get_uint32(props, "net.ttl", DEFAULT_TTL);
	impl->mcast_loop = pw_properties_get_bool(props, "net.loop", DEFAULT_LOOP);

	impl->core = pw_context_get_object(context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		impl->core = pw_context_connect(context,
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

	if ((res = start_sap_announce(impl)) < 0)
		goto out;

	impl->registry = pw_core_get_registry(impl->core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(impl->registry, &impl->registry_listener,
			&registry_events, impl);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_info));

	pw_log_info("Successfully loaded module-rtp-sink");

	return 0;
out:
	impl_destroy(impl);
	return res;
}
