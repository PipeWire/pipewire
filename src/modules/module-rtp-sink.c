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

#include <spa/utils/atomic.h>
#include <spa/utils/defs.h>
#include <spa/utils/hook.h>
#include <spa/utils/list.h>
#include <spa/utils/result.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/json.h>
#include <spa/utils/ratelimit.h>
#include <spa/param/audio/format-utils.h>
#include <spa/debug/types.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

#include <module-rtp/stream.h>
#include "network-utils.h"

#ifndef IPTOS_DSCP
#define IPTOS_DSCP_MASK 0xfc
#define IPTOS_DSCP(x) ((x) & IPTOS_DSCP_MASK)
#endif

/** \page page_module_rtp_sink RTP sink
 *
 * The `rtp-sink` module creates a PipeWire sink that sends audio
 * RTP packets.
 *
 * For the internal design of the shared RTP stream implementation (ring buffer,
 * buffer modes, threading model, and the separate PTP sender mechanism), see
 * \ref page_rtp_module_internals .
 *
 * ## Module Name
 *
 * `libpipewire-module-rtp-sink`
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `source.ip =<str>`: source IP address, default "0.0.0.0"
 * - `destination.ip =<str>`: destination IP address, default "224.0.0.56"
 * - `destination.port =<int>`: destination port, default random between 46000 and 47024
 * - `local.ifname = <str>`: interface name to use
 * - `net.mtu = <int>`: MTU to use, default 1280
 * - `net.ttl = <int>`: TTL to use, default 1
 * - `net.loop = <bool>`: loopback multicast, default false
 * - `sess.min-ptime = <float>`: minimum packet time in milliseconds, default 2
 * - `sess.max-ptime = <float>`: maximum packet time in milliseconds, default 20
 * - `sess.name = <str>`: a session name
 * - `rtp.ptime = <float>`: size of the packets in milliseconds, default up to MTU but
 *       between sess.min-ptime and sess.max-ptime
 * - `rtp.framecount = <int>`: number of samples per packet, default up to MTU but
 *       between sess.min-ptime and sess.max-ptime
 * - `sess.latency.msec = <float>`: target node latency in milliseconds, default as rtp.ptime
 * - `sess.ts-offset = <int>`: an offset to apply to the timestamp, default -1 = random offset
 * - `sess.ts-refclk = <string>`: the name of a reference clock
 * - `sess.media = <string>`: the media type audio|midi|opus, default audio
 * - `sess.ts-direct = <bool>`: use direct timestamp mode, default false.
 *                     \note RTP sources that use direct timestamp mode expect the
 *                     associated RTP sink to use direct timestamp mode as well. See the
 *                     `sess.ts-direct` documentation in \ref page_module_rtp_source for more.
 * - `stream.props = {}`: properties to be passed to the stream
 * - `aes67.driver-group = <string>`: for AES67 streams, can be specified in order to allow
 *       the sink to be driven by a different node than the PTP driver.
 *
 * ### Additional information about `source.ip` and `local.ifname`
 *
 * The default (ANY, 0.0.0.0 or ::) lets the kernel choose the local egress interface
 * (and, from it, the source address) based on the route to `destination.ip`.
 * Setting a concrete `source.ip` address instead of ANY alters how the source-address
 * field in the outgoing packets is populated, and interacts with routing.
 *
 * In the unicast case, `source.ip` binds the socket to that local IP, setting the
 * source-address field that will appear in outgoing packets. Egress is still determined
 * by the kernel's routing lookup for the destination rather than by this address, thoug
 * source-based policy routing (if configured in the OS) can factor it into the lookup.
 *
 * \important In the multicast case, do not rely on `source.ip` to choose the outgoing
 * interface. The sockets API makes no guarantee that the source address selects multicast
 * egress, and what happens is not portable across address families (it differs between
 * IPv4 and IPv6) or operating systems. For example, the Linux kernel implicitly uses a
 * bound IPv4 source to pin the egress device for legacy compatibility, but does not do
 * so for IPv6. To control which interface multicast packets leave on, set `local.ifname`.
 *
 * (No corresponding `source.port` property exists, because the kernel
 * automatically picks an ephemeral local egress port during bind.)
 *
 * Should `local.ifname` be set, egress is strictly forced out of that named interface
 * via SO_BINDTODEVICE. If `source.ip` is left at ANY, the kernel auto-selects a source
 * address belonging to that interface, and uses that source address as the value of the
 * source-address field in the outgoing packets.
 *
 * These two properties can be combined. `local.ifname` chooses the physical interface,
 * while `source.ip` fixates the exact value of the source-address field in outgoing packets.
 * Setting them inconsistently (a `source.ip` that belongs to a different interface
 * than `local.ifname`) is not rejected at setup, but it is almost always a
 * misconfiguration. The packet then leaves via the `local.ifname` device carrying a
 * source address from another interface, which is a common cause of reverse-path
 * filtering (rp_filter) drops at the receiver or an intermediate hop.
 *
 * ## General options
 *
 * Options with well-known behavior:
 *
 * - \ref PW_KEY_REMOTE_NAME
 * - \ref PW_KEY_AUDIO_FORMAT
 * - \ref PW_KEY_AUDIO_RATE
 * - \ref PW_KEY_AUDIO_CHANNELS
 * - \ref SPA_KEY_AUDIO_LAYOUT
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
 * # ~/.config/pipewire/pipewire.conf.d/my-rtp-sink.conf
 *
 * context.modules = [
 * {   name = libpipewire-module-rtp-sink
 *     args = {
 *         #local.ifname = "eth0"
 *         #source.ip = "0.0.0.0"
 *         #destination.ip = "224.0.0.56"
 *         #destination.port = 46000
 *         #net.mtu = 1280
 *         #net.ttl = 1
 *         #net.loop = false
 *         #sess.min-ptime = 2
 *         #sess.max-ptime = 20
 *         #sess.name = "PipeWire RTP stream"
 *         #sess.media = "audio"
 *         #audio.format = "S16BE"
 *         #audio.rate = 48000
 *         #audio.channels = 2
 *         #audio.position = [ FL FR ]
 *         stream.props = {
 *             node.name = "rtp-sink"
 *         }
 *     }
 *}
 *]
 *\endcode
 *
 * ## Adding and removing receivers through commands
 *
 * The following commands can be sent to the RTP sink node via `pw_node_send_command()`:
 *
 * * `add-receiver` : Adds a receiver to the sink's list. If the given
 *   IP address <-> port combination was already added, the command is
 *   logged, but otherwise ignored. Arguments:
 * - `destination.ip` : IP address to send data to. Can be a uni- or
 *   multicast address, but must be a valid address.
 * - `destination.port` : Port to send data to. Must be valid.
 * - `local.ifname`, `source.ip`, `net.ttl`, `net.dscp`, `net.loop` :
 *   These are all optional, and work just like in the RTP sink
 *   module's properties.
 *
 * * `remove-receiver` : Removes a receiver from the sink's list. The
 *   receiver is identified by the given IP address. A port can optionally
 *   be specified as well. If it isn't, then the first receiver with that IP
 *   address is removed. If no matching receiver is in the sink's list,
 *   this command does nothing. Arguments:
 *   - `destination.ip` : IP address to send data to. Can be a uni- or
 *     multicast address, but must be a valid address.
 *   - `destination.port` : Port to send data to. This is optional. But, if
 *     it is set, it must be a valid port number.
 *
 * * `clear-receivers` : Removes all receivers from the sink's list. If the
 *   list is empty, this does nothing. This command has no arguments.
 *
 * If the RTP sink module is created with the `destination.ip` and
 * `destination.port` properties set, it behaves as if `add-receiver` were
 * called right after the module was initialized. This means that if none
 * of these commands are used, the module behaves just as it did prior to
 * this patch. Note that the `remove-receivers` command can remove this
 * initial receiver as well.
 *
 * If no receivers are added, the module continues to work normally.
 * Adding and removing receivers mid-operation is supported.
 *
 * Example pw-cli calls (56 is the ID of the RTP sink node):
 *
 * \code{.unparsed}
 * pw-cli c 56 User '{ extra="{ \"command.id\" : \"add-receiver\" , \"destination.ip\" : \"10.42.0.1\", \"destination.port\" : 55001 }" }'
 * pw-cli c 56 User '{ extra="{ \"command.id\" : \"remove-receiver\", \"destination.ip\" : \"10.42.0.1\" }" }'
 * pw-cli c 56 User '{ extra="{ \"command.id\" : \"clear-receivers\" }" }'
 * \endcode
 *
 * ## Separate PTP sender
 *
 * For AES67-style streams, the sink can be driven by a graph driver that is
 * separate from the main graph, decoupling RTP transmission timing from whatever
 * drives the rest of the graph. This is the "separate PTP sender".
 *
 * This feature is only available on the sink (sending) side; receivers cannot use
 * it. It is activated by setting `aes67.driver-group` to a non-empty string. The
 * value may be given either directly in the module's properties (in which case the
 * module copies it into `stream.props`) or in `stream.props` directly.
 *
 * `aes67.driver-group` is the name of a node group. The graph driver that shall be
 * used for sending out RTP packets and generating RTP timestamps must have its node
 * group set to that same name. It is called the "PTP sender" because that driver
 * typically synchronizes itself using PTP, but any time-synchronization method works
 * as long as the driver keeps \ref spa_io_clock::position synchronized.
 *
 * The benefits of decoupling the main graph from the synchronized driver are:
 *
 * 1. Any discontinuities and resynchronizations in the time-sync protocol do not
 *    affect the entire graph, just the separate sender.
 * 2. Local audio sinks running in parallel to the RTP sink do not have to rate-match
 *    to follow the synchronized graph driver, so their local output is left unaltered
 *    (rate matching would otherwise be done with an ASRC or a tweakable PLL).
 * 3. Graph clock rate changes (for example, playing audio at a rate that does not
 *    match the current one) no longer affect the synchronized driver's time sync.
 * 4. Linking/unlinking the RTP sink does not trigger a graph driver renegotiation,
 *    which otherwise can cause subtle bugs if not handled carefully.
 *
 * The main downsides are:
 *
 * 1. Increased complexity, and thus more places where something can go wrong. In
 *    particular, the fill-level-based control loop can suffer from over/underruns,
 *    making it an additional potential source of audible dropouts.
 * 2. Increased latency. Since the control loop keeps the fill level at the target,
 *    the separate PTP sender adds roughly `sess.latency.msec` minus one quantum of
 *    latency (the last quantum's worth of data is already being used to produce the
 *    current graph cycle).
 * 3. It only benefits the sender. The receiver still has to use the synchronized
 *    graph driver for its entire graph.
 *
 * The internal mechanism (the dedicated DLL, the refilling state machine, and the
 * clock-drift computation) is described in \ref page_rtp_module_internals .
 *
 * \since 0.3.60
 */

#define NAME "rtp-sink"

PW_LOG_TOPIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define DEFAULT_PORT		46000
#define DEFAULT_SOURCE_IP	"0.0.0.0"
#define DEFAULT_SOURCE_IP6	"::"
#define DEFAULT_DESTINATION_IP	"224.0.0.56"
#define DEFAULT_TTL		1
#define DEFAULT_LOOP		false
#define DEFAULT_DSCP		34 /* Default to AES-67 AF41 (34) */

#define DEFAULT_TS_OFFSET	-1

#define USAGE	"( source.ip=<source IP address, default:"DEFAULT_SOURCE_IP"> ) "			\
		"( destination.ip=<destination IP address, default:"DEFAULT_DESTINATION_IP"> ) "	\
		"( destination.port=<int, default random between 46000 and 47024> ) "			\
		"( local.ifname=<local interface name to use> ) "					\
		"( net.mtu=<desired MTU, default:"SPA_STRINGIFY(DEFAULT_MTU)"> ) "			\
		"( net.ttl=<desired TTL, default:"SPA_STRINGIFY(DEFAULT_TTL)"> ) "			\
		"( net.loop=<desired loopback, default:"SPA_STRINGIFY(DEFAULT_LOOP)"> ) "		\
		"( net.dscp=<desired DSCP, default:"SPA_STRINGIFY(DEFAULT_DSCP)"> ) "			\
		"( sess.name=<a name for the session> ) "						\
		"( sess.min-ptime=<minimum packet time in milliseconds, default:2> ) "			\
		"( sess.max-ptime=<maximum packet time in milliseconds, default:20> ) "			\
		"( sess.media=<string, the media type audio|midi|opus, default audio> ) "		\
		"( audio.format=<format, default:"DEFAULT_RAW_AUDIO_FORMAT"> ) "			\
		"( audio.rate=<sample rate, default:"SPA_STRINGIFY(DEFAULT_RATE)"> ) "			\
		"( audio.channels=<number of channels, default:"SPA_STRINGIFY(DEFAULT_CHANNELS)"> ) "	\
		"( audio.position=<channel map, default:"DEFAULT_POSITION"> ) "				\
		"( audio.layout=<channel layout, default:"DEFAULT_LAYOUT"> ) "				\
		"( aes67.driver-group=<driver driving the PTP send> ) "					\
		"( stream.props= { key=value ... } ) "

static const struct spa_dict_item module_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "RTP Sink" },
	{ PW_KEY_MODULE_USAGE, USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

enum rtp_target_type {
	RTP_TARGET_TYPE_UNICAST,
	RTP_TARGET_TYPE_MULTICAST
};

struct rtp_target {
	struct spa_list link;

	/* Common multicast and unicast fields */

	enum rtp_target_type type;

	uint16_t dest_port;
	struct sockaddr_storage dest_addr;
	socklen_t dest_addrlen;

	int socket_fd;

	uint32_t ttl;
	uint32_t dscp;

	/* Multicast specific fields */

	char *ifname;
	bool mcast_loop;

	struct sockaddr_storage src_addr;
	socklen_t src_addrlen;
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
	struct rtp_stream *stream;

	struct spa_ratelimit rate_limit;

	unsigned int do_disconnect_core:1;

	struct spa_list rtp_targets;
	size_t num_rtp_targets;

	/* This flag is needed to know whether on_add_receiver() shall connect
	 * the socket immediately, or keep the target disconnected. The latter
	 * case is done when the PW node is not running; then, once it does start
	 * running, the stream.c stream_start() call will in turn call
	 * stream_open_connection(), which will connect the socket. */
	bool stream_connected;
};

static int make_socket(struct sockaddr_storage *src, socklen_t src_len,
		struct sockaddr_storage *dst, socklen_t dst_len,
		bool loop, int ttl, int dscp, char *ifname)
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
#ifdef SO_BINDTODEVICE
	if (ifname && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname)) < 0) {
		res = -errno;
		pw_log_error("setsockopt(SO_BINDTODEVICE) failed: %m");
		goto error;
	}
#endif
	if (connect(fd, (struct sockaddr*)dst, dst_len) < 0) {
		res = -errno;
		pw_log_error("connect() failed: %m");
		goto error;
	}
	if (pw_net_is_multicast(dst)) {
		if (dst->ss_family == AF_INET) {
			val = loop;
			if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &val, sizeof(val)) < 0)
				pw_log_warn("setsockopt(IP_MULTICAST_LOOP) failed: %m");

			val = ttl;
			if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &val, sizeof(val)) < 0)
				pw_log_warn("setsockopt(IP_MULTICAST_TTL) failed: %m");
		} else {
			val = loop;
			if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &val, sizeof(val)) < 0)
				pw_log_warn("setsockopt(IPV6_MULTICAST_LOOP) failed: %m");

			val = ttl;
			if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &val, sizeof(val)) < 0)
				pw_log_warn("setsockopt(IPV6_MULTICAST_HOPS) failed: %m");
		}
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

static void stream_destroy(void *d)
{
	struct impl *impl = d;
	impl->stream = NULL;
}

static void teardown_rtp_target(struct rtp_target *target);

static int setup_rtp_target(struct rtp_target *target,
	const char *ifname, const char *destination_ip, uint16_t destination_port,
	const char *source_ip, uint32_t ttl, uint32_t dscp, bool mcast_loop)
{
	int res = 0;

	memset(target, 0, sizeof(struct rtp_target));
	target->socket_fd = -1;

	if (destination_port == 0)
		target->dest_port = (DEFAULT_PORT + ((uint32_t) (pw_rand32() % 512) << 1));
	else
		target->dest_port = destination_port;

	if ((res = pw_net_parse_address(destination_ip, target->dest_port,
		&target->dest_addr, &target->dest_addrlen)) < 0) {
		pw_log_error("invalid destination IP \"%s\": %s", destination_ip, spa_strerror(res));
		goto error;
	}

	if (source_ip == NULL)
		source_ip = (target->dest_addr.ss_family == AF_INET) ?
		DEFAULT_SOURCE_IP : DEFAULT_SOURCE_IP6;
	if ((res = pw_net_parse_address(source_ip, 0, &target->src_addr,
		&target->src_addrlen)) < 0) {
		pw_log_error("invalid source IP \"%s\": %s", source_ip, spa_strerror(res));
		goto error;
	}

	target->ttl = ttl;
	target->dscp = dscp;
	target->mcast_loop = mcast_loop;
	target->ifname = ifname ? strdup(ifname) : NULL;

	target->type = pw_net_is_multicast(&(target->dest_addr))
		? RTP_TARGET_TYPE_MULTICAST
		: RTP_TARGET_TYPE_UNICAST;

out:
	return res;

error:
	teardown_rtp_target(target);
	goto out;
}

static int setup_rtp_target_from_props(struct rtp_target *target, struct pw_properties *props, bool *target_was_setup)
{
	const char *destination_ip;
	uint16_t destination_port;
	const char *ifname;
	const char *source_ip;
	uint32_t ttl;
	uint32_t dscp;
	bool mcast_loop;
	int res;

	*target_was_setup = false;

	destination_ip = pw_properties_get(props, "destination.ip");
	if (destination_ip == NULL)
		return 0;

	destination_port = pw_properties_get_uint32(props, "destination.port", 0);
	ifname = pw_properties_get(props, "local.ifname");
	source_ip = pw_properties_get(props, "source.ip");
	ttl = pw_properties_get_uint32(props, "net.ttl", DEFAULT_TTL);
	dscp = pw_properties_get_uint32(props, "net.dscp", DEFAULT_DSCP);
	mcast_loop = pw_properties_get_bool(props, "net.loop", DEFAULT_LOOP);

	res = setup_rtp_target(target, ifname, destination_ip, destination_port, source_ip, ttl, dscp, mcast_loop);

	if (res == 0)
		*target_was_setup = true;

	return res;
}

static void teardown_rtp_target(struct rtp_target *target)
{
	if (target->socket_fd >= 0) {
		close(target->socket_fd);
		target->socket_fd = -1;
	}

	free(target->ifname);
	target->ifname = NULL;
}

#define LOG_RTP_TARGET(LOGLEVEL, PREFIX, TARGET) \
	do { \
		if (SPA_UNLIKELY(pw_log_topic_enabled((LOGLEVEL), PW_LOG_TOPIC_DEFAULT))) { \
			char src_addr_str_buf[256]; \
			char dest_addr_str_buf[256]; \
			const char *type_str; \
			const char *src_addr_str; \
			const char *dest_addr_str; \
			const char *iface_name; \
			uint16_t dest_port = 0; \
 \
			type_str = ((TARGET)->type == RTP_TARGET_TYPE_UNICAST) ? "unicast" : "multicast"; \
			src_addr_str = dest_addr_str = "<could not get address>"; \
			iface_name = ((TARGET)->ifname != NULL) ? (TARGET)->ifname : "<none>"; \
 \
			if (pw_net_get_ip(&((TARGET)->src_addr), src_addr_str_buf, sizeof(src_addr_str_buf), NULL, NULL) == 0) \
				src_addr_str = src_addr_str_buf; \
			if (pw_net_get_ip(&((TARGET)->dest_addr), dest_addr_str_buf, sizeof(dest_addr_str_buf), NULL, &dest_port) == 0) \
				dest_addr_str = dest_addr_str_buf; \
 \
			if ((TARGET)->socket_fd < 0) { \
				pw_log_logt((LOGLEVEL), PW_LOG_TOPIC_DEFAULT, __FILE__, __LINE__, __func__, \
					    "%s:  type: %s; dest addr: %s; dest port: %" PRIu16 "; " \
					    "src addr: %s; TTL: %" PRIu32 "; DSCP: %" PRIu32 "; " \
					    "iface name: %s; mcast loop: %d; (socket not opened yet)", \
					    (PREFIX), type_str, dest_addr_str, dest_port, src_addr_str, \
					    (TARGET)->ttl, (TARGET)->dscp, iface_name, (TARGET)->mcast_loop); \
			} else { \
				pw_log_logt((LOGLEVEL), PW_LOG_TOPIC_DEFAULT, __FILE__, __LINE__, __func__, \
					    "%s  type: %s; dest addr: %s; dest port: %" PRIu16 "; " \
					    "src addr: %s; TTL: %" PRIu32 "; DSCP: %" PRIu32 "; " \
					    "iface name: %s; mcast loop: %d; socket FD: %d", (PREFIX), \
					    type_str, dest_addr_str, dest_port, src_addr_str, (TARGET)->ttl, \
					    (TARGET)->dscp, iface_name, (TARGET)->mcast_loop, (TARGET)->socket_fd); \
			} \
		} \
	} while (0)

static struct rtp_target *find_rtp_target_by_sockaddr(struct impl *impl, struct sockaddr_storage *address, bool compare_ports)
{
	struct rtp_target *rtp_target;

	spa_list_for_each(rtp_target, &(impl->rtp_targets), link) {
		if (pw_net_are_addresses_equal(&(rtp_target->dest_addr), address, compare_ports)) {
			return rtp_target;
		}
	}

	return NULL;
}

static struct rtp_target *find_rtp_target(struct impl *impl, const char *destination_ip, uint16_t destination_port)
{
	int res;
	struct sockaddr_storage dest_addr;
	socklen_t dest_len;

	if ((res = pw_net_parse_address(destination_ip, destination_port,
		&dest_addr, &dest_len)) < 0) {
		pw_log_error("invalid destination IP \"%s\": %s", destination_ip, spa_strerror(res));
		return NULL;
	}

	return find_rtp_target_by_sockaddr(impl, &dest_addr, destination_port != 0);
}

static int append_to_rtp_targets(struct spa_loop *loop, bool async, uint32_t seq,
				 const void *data, size_t size, void *user_data)
{
	/* IMPORTANT: This must be run from within the data loop, since the rtp_targets
	 * list is modified here, and the stream_send_packet() function (which runs in
	 * the data loop thread), iterates over this same list. */

	struct impl *impl = user_data;
	struct rtp_target *rtp_target_to_add = (struct rtp_target *)data;

	spa_list_append(&(impl->rtp_targets), &(rtp_target_to_add->link));
	impl->num_rtp_targets++;

	return 0;
}

static int add_rtp_target(struct impl *impl, struct rtp_target *rtp_target, bool append_in_data_loop)
{
	struct rtp_target *rtp_target_copy;

	LOG_RTP_TARGET(SPA_LOG_LEVEL_INFO, "Adding RTP target", rtp_target);

	/* Allocate and fill the target here, outside of the data loop
	 * thread, to not unnecessarily block it. */
	rtp_target_copy = malloc(sizeof(struct rtp_target));
	if (SPA_UNLIKELY(rtp_target_copy == NULL)) {
		pw_log_error("could not allocate memory for new target");
		return -ENOMEM;
	}

	memcpy(rtp_target_copy, rtp_target, sizeof(struct rtp_target));
	/* Ensure that nothing is stored in the spa_list link; it will
	 * be filled by spa_list_append(). */
	spa_zero(rtp_target_copy->link);

	if (append_in_data_loop)
		rtp_stream_run_in_data_loop(impl->stream, append_to_rtp_targets, 1, rtp_target_copy, 0, impl);
	else
		append_to_rtp_targets(NULL, false, 0, rtp_target_copy, 0, impl);

	return 0;
}

static int remove_from_rtp_targets(struct spa_loop *loop, bool async, uint32_t seq,
				   const void *data, size_t size, void *user_data)
{
	/* IMPORTANT: This must be run from within the data loop, since the rtp_targets
	 * list is modified here, and the stream_send_packet() function (which runs in
	 * the data loop thread), iterates over this same list. */

	struct impl *impl = user_data;
	struct rtp_target *rtp_target_to_remove = (struct rtp_target *)data;

	spa_list_remove(&(rtp_target_to_remove->link));
	impl->num_rtp_targets--;

	return 0;
}

static void remove_rtp_target(struct impl *impl, struct rtp_target *rtp_target_to_remove,
			      bool remove_in_data_loop, const char *custom_prefix)
{
	if (custom_prefix == NULL)
		custom_prefix = "Removing RTP target";

	LOG_RTP_TARGET(SPA_LOG_LEVEL_INFO, custom_prefix, rtp_target_to_remove);

	if (remove_in_data_loop)
		rtp_stream_run_in_data_loop(impl->stream, remove_from_rtp_targets, 1, rtp_target_to_remove, 0, impl);
	else
		remove_from_rtp_targets(NULL, false, 0, rtp_target_to_remove, 0, impl);

	teardown_rtp_target(rtp_target_to_remove);

	free(rtp_target_to_remove);
}

static void remove_rtp_target_by_ip_and_port(struct impl *impl, const char *destination_ip,
					     uint16_t destination_port, bool remove_in_data_loop)
{
	struct rtp_target *rtp_target_to_remove;

	if (impl->num_rtp_targets == 0)
		return;

	rtp_target_to_remove = find_rtp_target(impl, destination_ip, destination_port);
	if (rtp_target_to_remove == NULL)
		return;

	remove_rtp_target(impl, rtp_target_to_remove, remove_in_data_loop, NULL);
}

static inline uint64_t get_time_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return SPA_TIMESPEC_TO_NSEC(&ts);
}

static void stream_send_packet(void *data, struct iovec *iov, size_t iovlen)
{
	struct impl *impl = data;
	struct msghdr msg;
	ssize_t n;
	struct rtp_target *rtp_target;

	spa_zero(msg);
	msg.msg_iov = iov;
	msg.msg_iovlen = iovlen;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = 0;

	spa_list_for_each(rtp_target, &(impl->rtp_targets), link) {
		/* All targets are required to have open sockets
		 * by the time sending takes place. */
		spa_assert(rtp_target->socket_fd >= 0);
		n = sendmsg(rtp_target->socket_fd, &msg, MSG_NOSIGNAL);
		if (n < 0) {
			int suppressed;
			if ((suppressed = spa_ratelimit_test(&impl->rate_limit, get_time_ns())) >= 0) {
				pw_log_warn("(%d suppressed) sendmsg() failed: %m", suppressed);
				LOG_RTP_TARGET(SPA_LOG_LEVEL_WARN, "RTP target", rtp_target);
			}
		}
	}
}

static void stream_report_error(void *data, const char *error)
{
	struct impl *impl = data;
	if (error) {
		pw_log_error("stream error: %s", error);
		pw_impl_module_schedule_destroy(impl->module);
	}
}

static void stream_close_connection(void *data, int *result);

static void stream_open_connection(void *data, int *result)
{
	int res;
	struct impl *impl = data;
	struct rtp_target *rtp_target;

	spa_list_for_each(rtp_target, &(impl->rtp_targets), link) {
		if ((res = make_socket(&rtp_target->src_addr, rtp_target->src_addrlen,
				       &rtp_target->dest_addr, rtp_target->dest_addrlen,
				       rtp_target->mcast_loop, rtp_target->ttl,
				       rtp_target->dscp, rtp_target->ifname)) < 0) {
			pw_log_error("can't make socket: %s", spa_strerror(res));
			LOG_RTP_TARGET(SPA_LOG_LEVEL_WARN, "RTP target", rtp_target);
			rtp_stream_set_error(impl->stream, res, "Can't make socket");
			stream_close_connection(data, NULL);
			if (result)
				*result = res;
			return;
		}
		rtp_target->socket_fd = res;
	}

	if (result)
		*result = 1;

	/* Now, after all sockets are opened, mark the stream as connected. */
	impl->stream_connected = true;
}

static void stream_close_connection(void *data, int *result)
{
	struct impl *impl = data;
	struct rtp_target *rtp_target;

	/* Mark the stream as disconnected to let future on_add_receiver()
	 * calls know that they must not connect the socket on their own. */
	impl->stream_connected = false;

	if (result)
		*result = 0;

	spa_list_for_each(rtp_target, &(impl->rtp_targets), link) {
		if (rtp_target->socket_fd >= 0) {
			if (result)
				*result = 1;
			close(rtp_target->socket_fd);
			rtp_target->socket_fd = -1;
		}
	}
}

static void stream_props_changed(struct impl *impl, uint32_t id, const struct spa_pod *param)
{
	struct spa_pod_object *obj = (struct spa_pod_object *)param;
	struct spa_pod_prop *prop;

	if (param == NULL)
		return;

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		if (prop->key == SPA_PROP_params) {
			struct spa_pod *params = NULL;
			struct spa_pod_parser prs;
			struct spa_pod_frame f;
			const char *key;
			struct spa_pod *pod;
			struct spa_dict_item items[4];
			unsigned int n_items = 0;

			if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_Props, NULL, SPA_PROP_params,
					SPA_POD_OPT_Pod(&params)) < 0)
				return;
			spa_pod_parser_pod(&prs, params);
			if (spa_pod_parser_push_struct(&prs, &f) < 0)
				return;

			while (n_items < SPA_N_ELEMENTS(items)) {
				const char *value_str = NULL;
				int value_int = -1;

				if (spa_pod_parser_get_string(&prs, &key) < 0)
					break;
				if (spa_pod_parser_get_pod(&prs, &pod) < 0)
					break;
				if (spa_pod_get_string(pod, &value_str) < 0 &&
						spa_pod_get_int(pod, &value_int) < 0)
					continue;
				pw_log_info("key '%s', value '%s'/%u", key, value_str, value_int);
				if (spa_streq(key, "sess.name")) {
					if (!value_str) {
						pw_log_error("invalid sess.name");
						break;
					}
					pw_properties_set(impl->stream_props, "sess.name", value_str);
					items[n_items++] = SPA_DICT_ITEM_INIT("sess.name", value_str);
				} else if (spa_streq(key, "sess.id") || spa_streq(key, "sess.version")) {
					if (value_int < 0 || (unsigned int)value_int > UINT32_MAX) {
						pw_log_error("invalid %s: '%d'", key, value_int);
						break;
					}
					pw_properties_setf(impl->stream_props, key, "%d", value_int);
					items[n_items++] = SPA_DICT_ITEM_INIT(key, pw_properties_get(impl->stream_props, key));
				} else if (spa_streq(key, "sess.sap.announce")) {
					if (!value_str) {
						pw_log_error("invalid sess.sap.announce");
						break;
					}
					pw_properties_setf(impl->stream_props, key, "%s", value_str);
					items[n_items++] = SPA_DICT_ITEM_INIT(key, pw_properties_get(impl->stream_props, key));
				}
			}

			rtp_stream_update_properties(impl->stream, &SPA_DICT_INIT(items, n_items));
		}
	}
}

static void stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct impl *impl = data;

	switch (id) {
	case SPA_PARAM_Props:
		if (param != NULL)
			stream_props_changed(impl, id, param);
		break;
	}
}

static void on_add_receiver(struct impl *impl, struct spa_json *command_json_iter)
{
	int res;
	char key[256];
	const char *value;
	struct rtp_target rtp_target;
	int len;
	char ifname[64];
	char destination_ip[64];
	int destination_port = 0;
	char source_ip[64];
	int ttl = DEFAULT_TTL;
	int dscp = DEFAULT_DSCP;
	bool mcast_loop = DEFAULT_LOOP;

	ifname[0] = '\0';
	destination_ip[0] = '\0';
	source_ip[0] = '\0';

	while ((len = spa_json_object_next(command_json_iter, key, sizeof(key), &value)) > 0) {
		if (spa_streq(key, "local.ifname")) {
			spa_json_parse_stringn(value, len, ifname, sizeof(ifname));
		} else if (spa_streq(key, "destination.ip")) {
			spa_json_parse_stringn(value, len, destination_ip, sizeof(destination_ip));
		} else if (spa_streq(key, "destination.port")) {
			spa_json_parse_int(value, len, &destination_port);
		} else if (spa_streq(key, "source.ip")) {
			spa_json_parse_stringn(value, len, source_ip, sizeof(source_ip));
		} else if (spa_streq(key, "net.ttl")) {
			spa_json_parse_int(value, len, &ttl);
		} else if (spa_streq(key, "net.dscp")) {
			spa_json_parse_int(value, len, &dscp);
		} else if (spa_streq(key, "net.loop")) {
			spa_json_parse_bool(value, len, &mcast_loop);
		}
	}

	if (destination_ip[0] == '\0') {
		pw_log_error("Cannot add receiver without a destination.ip value");
		return;
	}

	if ((res = setup_rtp_target(&rtp_target, (ifname[0] != '\0') ? ifname : NULL,
		destination_ip, destination_port, (source_ip[0] != '\0') ? source_ip : NULL,
		ttl, dscp, mcast_loop)) != 0)
		return;

	if (find_rtp_target_by_sockaddr(impl, &(rtp_target.dest_addr), true) != NULL) {
		LOG_RTP_TARGET(SPA_LOG_LEVEL_WARN, "Not adding RTP target because it is already added", &rtp_target);
		goto duplicate_add;
	}

	/* Only create the socket when the stream is connected. Sockets are
	 * supposed to be disconnected otherwise. If it is disconnected, adding
	 * the RTP target to the list is enough - stream_open_connection() will
	 * connect the target's socket then. */
	if (impl->stream_connected) {
		if ((res = make_socket(&rtp_target.src_addr, rtp_target.src_addrlen,
					&rtp_target.dest_addr, rtp_target.dest_addrlen,
					rtp_target.mcast_loop, rtp_target.ttl,
					rtp_target.dscp, rtp_target.ifname)) < 0) {
			pw_log_error("Couldn't make socket for new receiver: %s", spa_strerror(res));
			goto error;
		}
		pw_log_debug("Created socket for new receiver, socket FD: %d", res);

		rtp_target.socket_fd = res;
	}

	if (add_rtp_target(impl, &rtp_target, true) != 0)
		goto error;

finish:
	return;

duplicate_add:
	teardown_rtp_target(&rtp_target);
	goto finish;

error:
	teardown_rtp_target(&rtp_target);
	goto finish;
}

static void on_remove_receiver(struct impl *impl, struct spa_json *command_json_iter)
{
	char key[256];
	const char *value;
	int len;
	char destination_ip[64];
	int destination_port = 0;

	destination_ip[0] = '\0';

	while ((len = spa_json_object_next(command_json_iter, key, sizeof(key), &value)) > 0) {
		if (spa_streq(key, "destination.ip")) {
			spa_json_parse_stringn(value, len, destination_ip, sizeof(destination_ip));
		} else if (spa_streq(key, "destination.port")) {
			spa_json_parse_int(value, len, &destination_port);
		}
	}

	if (destination_ip[0] == '\0') {
		pw_log_error("Cannot remove receiver without a destination.ip value");
		return;
	}

	remove_rtp_target_by_ip_and_port(impl, destination_ip, destination_port, true);
}

static void on_clear_receivers(struct impl *impl)
{
	struct rtp_target *rtp_target;

	pw_log_info("Clearing all receivers");

	spa_list_consume(rtp_target, &(impl->rtp_targets), link)
		remove_rtp_target(impl, rtp_target, true, NULL);
}

static void parse_rtp_command(struct impl *impl, const char *command_json_str)
{
	int res;
	struct spa_json iter;
	char rtp_command_id[64];

	if ((res = spa_json_str_object_find(command_json_str, strlen(command_json_str),
				     "command.id", rtp_command_id, sizeof(rtp_command_id))) <= 0) {
		if (res == -ENOENT) {
			pw_log_error("Command JSON string \"%s\" has no command.id field",
				     command_json_str);
		} else {
			pw_log_error("Error while parsing JSON string \"%s\": %s", command_json_str,
				     spa_strerror(res));
		}

		return;
	}

	if ((res = spa_json_begin_object(&iter, command_json_str, strlen(command_json_str))) <= 0) {
		pw_log_error("Error while parsing JSON string \"%s\": %s", command_json_str,
			     spa_strerror(res));
		return;
	}

	if (spa_streq(rtp_command_id, "add-receiver")) {
		on_add_receiver(impl, &iter);
	} else if (spa_streq(rtp_command_id, "remove-receiver")) {
		on_remove_receiver(impl, &iter);
	} else if (spa_streq(rtp_command_id, "clear-receivers")) {
		on_clear_receivers(impl);
	} else {
		pw_log_error("Command JSON string \"%s\" has unrecognized command ID \"%s\"",
			     command_json_str, rtp_command_id);
	}
}

static void stream_command(void *data, const struct spa_command *command)
{
	struct impl *impl = data;

	if (SPA_UNLIKELY(SPA_COMMAND_TYPE(command) != SPA_TYPE_COMMAND_Node))
		return;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_User: {
		const struct spa_pod_object *pod_object = (const struct spa_pod_object *)command;
		struct spa_pod_prop *prop;

		SPA_POD_OBJECT_FOREACH(pod_object, prop) {
			if (prop->key != SPA_COMMAND_NODE_extra)
				continue;

			if (prop->value.type != SPA_TYPE_String)
				continue;

			const char *json_str = SPA_POD_CONTENTS(struct spa_pod, &prop->value);
			parse_rtp_command(impl, json_str);
		}

		break;
	}
	default:
		break;
	}
}

static const struct rtp_stream_events stream_events = {
	RTP_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.report_error = stream_report_error,
	.open_connection = stream_open_connection,
	.close_connection = stream_close_connection,
	.param_changed = stream_param_changed,
	.send_packet = stream_send_packet,
	.command = stream_command,
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
	struct rtp_target *rtp_target;

	if (impl->stream)
		rtp_stream_destroy(impl->stream);

	if (impl->core && impl->do_disconnect_core)
		pw_core_disconnect(impl->core);

	spa_list_consume(rtp_target, &(impl->rtp_targets), link)
		remove_rtp_target(impl, rtp_target, false, "Removing RTP target as part of shutdown");

	pw_properties_free(impl->stream_props);
	pw_properties_free(impl->props);

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
	int64_t ts_offset;
	int res = 0;
	uint32_t header_size;
	struct rtp_target initial_target;
	bool initial_target_valid = false;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

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

	spa_list_init(&impl->rtp_targets);

	impl->rate_limit.interval = 2 * SPA_NSEC_PER_SEC;
	impl->rate_limit.burst = 1;

	impl->module = module;
	impl->context = context;
	impl->loop = pw_context_get_main_loop(context);

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
	copy_props(impl, props, SPA_KEY_AUDIO_LAYOUT);
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
	copy_props(impl, props, "sess.id");
	copy_props(impl, props, "sess.version");
	copy_props(impl, props, "sess.min-ptime");
	copy_props(impl, props, "sess.max-ptime");
	copy_props(impl, props, "sess.latency.msec");
	copy_props(impl, props, "sess.ts-direct");
	copy_props(impl, props, "sess.ts-refclk");
	copy_props(impl, props, "aes67.driver-group");

	if ((res = setup_rtp_target_from_props(&initial_target, props, &initial_target_valid)) < 0) {
		pw_log_error("could not setup initial destination: %s", spa_strerror(res));
		goto out;
	}

	ts_offset = pw_properties_get_int64(props, "sess.ts-offset", DEFAULT_TS_OFFSET);
	if (ts_offset == -1)
		ts_offset = pw_rand32();
	pw_properties_setf(stream_props, "rtp.sender-ts-offset", "%u", (uint32_t)ts_offset);

	/* Assume IPv6 header size. This is necessary, since it
	 * is possible to add a mixture of IPv4 and IPv6 targets
	 * to the RTP sink. Having different headers sizes for
	 * both IPv4 and IPv6 would require producing packets
	 * separately for both protocols, which adds significant
	 * complexity and runtime overhead. Instead, accept the
	 * small waste (20 bytes) by always assuming an IPv6
	 * header size, even when IPv4 is used. */
	header_size = IP6_HEADER_SIZE;
	header_size += UDP_HEADER_SIZE;
	pw_properties_setf(stream_props, "net.header", "%u", header_size);

	if (initial_target_valid) {
		res = add_rtp_target(impl, &initial_target, false);
		if (res != 0) {
			teardown_rtp_target(&initial_target);
			goto out;
		}

		pw_net_get_ip(&(initial_target.src_addr), addr, sizeof(addr), NULL, NULL);
		pw_properties_set(stream_props, "rtp.source.ip", addr);
		pw_net_get_ip(&(initial_target.dest_addr), addr, sizeof(addr), NULL, NULL);
		pw_properties_set(stream_props, "rtp.destination.ip", addr);
		pw_properties_setf(stream_props, "rtp.destination.port", "%u", initial_target.dest_port);
		pw_properties_setf(stream_props, "rtp.ttl", "%u", initial_target.ttl);
		pw_properties_setf(stream_props, "rtp.dscp", "%u", initial_target.dscp);
	}

	impl->core = pw_context_get_object(impl->context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		impl->core = pw_context_connect(impl->context,
				pw_properties_new(
					PW_KEY_REMOTE_NAME, str,
					NULL),
				0);
		impl->do_disconnect_core = true;
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

	impl->stream = rtp_stream_new(impl->core,
			PW_DIRECTION_INPUT, pw_properties_copy(stream_props),
			&stream_events, impl);
	if (impl->stream == NULL) {
		res = -errno;
		pw_log_error("can't create stream: %m");
		goto out;
	}

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_info));

	pw_log_info("Successfully loaded module-rtp-sink");

	return 0;
out:
	impl_destroy(impl);
	return res;
}
