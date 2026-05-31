/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/filter.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/net_tstamp.h>
#include <limits.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <spa/support/cpu.h>
#include <spa/debug/mem.h>
#include <spa/utils/cleanup.h>
#include <spa/utils/result.h>

#include <pipewire/pipewire.h>

#include "avb.h"
#include "packets.h"
#include "internal.h"
#include "gptp.h"
#include "stream.h"
#include "acmp.h"
#include "adp.h"
#include "aecp.h"
#include "maap.h"
#include "mmrp.h"
#include "msrp.h"
#include "mvrp.h"
#include "descriptors.h"
#include "utils.h"
#include "acmp-cmds-resps/acmp-milan-v12.h"

/* IEEE 802.1Q-2014 Section 10.7.11: MRP join timer is ~100 ms. Run the periodic
 * dispatch at the same granularity so join/leave timers fire on time. */
#define DEFAULT_INTERVAL_MS	100

#define server_emit(s,m,v,...) spa_hook_list_call(&s->listener_list, struct server_events, m, v, ##__VA_ARGS__)
#define server_emit_destroy(s)		server_emit(s, destroy, 0)
#define server_emit_message(s,n,m,l)	server_emit(s, message, 0, n, m, l)
#define server_emit_periodic(s,n)	server_emit(s, periodic, 0, n)
#define server_emit_command(s,n,c,a,f)	server_emit(s, command, 0, n, c, a, f)


static const char *avb_mode_str[] = {
	[AVB_MODE_LEGACY] = "AVB Legacy",
	[AVB_MODE_MILAN_V12] = "Milan V1.2",
};

static void on_timer_event(void *data)
{
	struct server *server = data;
	struct impl *impl = server->impl;
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);
	server_emit_periodic(server, SPA_TIMESPEC_TO_NSEC(&now));

	pw_timer_queue_add(impl->timer_queue, &server->timer,
		&server->timer.timeout, DEFAULT_INTERVAL_MS * SPA_NSEC_PER_MSEC,
		on_timer_event, server);
}

static void on_socket_data(void *data, int fd, uint32_t mask)
{
	struct server *server = data;
	struct timespec now;

	if (mask & SPA_IO_IN) {
		int len;
		uint8_t buffer[2048];

		len = recv(fd, buffer, sizeof(buffer), 0);

		if (len < 0) {
			pw_log_warn("got recv error: %m");
		}
		else if (len < (int)sizeof(struct avb_packet_header)) {
			pw_log_warn("short packet received (%d < %d)", len,
					(int)sizeof(struct avb_packet_header));
		} else {
			clock_gettime(CLOCK_REALTIME, &now);
			server_emit_message(server, SPA_TIMESPEC_TO_NSEC(&now), buffer, len);
		}
	}
}

static int raw_send_packet(struct server *server, const uint8_t dest[6],
		uint16_t type, void *data, size_t size)
{
	struct avb_ethernet_header *hdr = (struct avb_ethernet_header*)data;
	int res = 0;

	memcpy(hdr->dest, dest, ETH_ALEN);
	memcpy(hdr->src, server->mac_addr, ETH_ALEN);
	hdr->type = htons(type);

	if (send(server->source->fd, data, size, 0) < 0) {
		res = -errno;
		pw_log_warn("got send error (size=%zu type=0x%04x): %m", size, type);
	}
	return res;
}

int avb_server_send_packet(struct server *server, const uint8_t dest[6],
		uint16_t type, void *data, size_t size)
{
	return server->transport->send_packet(server, dest, type, data, size);
}

static int load_filter(int fd, uint16_t eth, const uint8_t dest[6], const uint8_t mac[6])
{
	struct sock_fprog filter;
	struct sock_filter bpf_code[] = {
		BPF_STMT(BPF_LD|BPF_H|BPF_ABS,  12),
		BPF_JUMP(BPF_JMP|BPF_JEQ,       eth,        0, 8),
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS,  2),
		BPF_JUMP(BPF_JMP|BPF_JEQ,       (dest[2] << 24) |
						(dest[3] << 16) |
						(dest[4] <<  8) |
						(dest[5]),  0, 2),
		BPF_STMT(BPF_LD|BPF_H|BPF_ABS,  0),
		BPF_JUMP(BPF_JMP|BPF_JEQ,       (dest[0] << 8) |
						(dest[1]),  3, 4),
		BPF_JUMP(BPF_JMP|BPF_JEQ,       (mac[2] << 24) |
						(mac[3] << 16) |
						(mac[4] <<  8) |
						(mac[5]),   0, 3),
		BPF_STMT(BPF_LD|BPF_H|BPF_ABS,  0),
		BPF_JUMP(BPF_JMP|BPF_JEQ,       (mac[0] <<  8) |
						(mac[1]), 0, 1),
		BPF_STMT(BPF_RET,               0x00040000),
		BPF_STMT(BPF_RET,               0x00000000),
	};
	filter.len = sizeof(bpf_code) / 8;
	filter.filter = bpf_code;

	if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER,
				&filter, sizeof(filter)) < 0) {
		pw_log_error("setsockopt(ATTACH_FILTER) failed: %m");
		return -errno;
	}
	return 0;
}

static int raw_make_socket(struct server *server, uint16_t type, const uint8_t mac[6])
{
	int res;
	struct ifreq req;
	struct packet_mreq mreq;
	struct sockaddr_ll sll;

	spa_autoclose int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(ETH_P_ALL));
	if (fd < 0) {
		pw_log_error("socket() failed: %m");
		return -errno;
	}

	spa_zero(req);
	snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", server->ifname);
	if (ioctl(fd, SIOCGIFINDEX, &req) < 0) {
		res = -errno;
		pw_log_error("SIOCGIFINDEX %s failed: %m", server->ifname);
		return res;
	}
	server->ifindex = req.ifr_ifindex;

	spa_zero(req);
	snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", server->ifname);
	if (ioctl(fd, SIOCGIFHWADDR, &req) < 0) {
		res = -errno;
		pw_log_error("SIOCGIFHWADDR %s failed: %m", server->ifname);
		return res;
	}
	memcpy(server->mac_addr, req.ifr_hwaddr.sa_data, sizeof(server->mac_addr));

	server->entity_id = (uint64_t)server->mac_addr[0] << 56 |
			(uint64_t)server->mac_addr[1] << 48 |
			(uint64_t)server->mac_addr[2] << 40 |
			(uint64_t)0xff << 32 |
			(uint64_t)0xfe << 24 |
			(uint64_t)server->mac_addr[3] << 16 |
			(uint64_t)server->mac_addr[4] << 8 |
			(uint64_t)server->mac_addr[5];

	spa_zero(sll);
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ALL);
	sll.sll_ifindex = server->ifindex;
	if (bind(fd, (struct sockaddr *) &sll, sizeof(sll)) < 0) {
		res = -errno;
		pw_log_error("bind() failed: %m");
		return res;
	}

	spa_zero(mreq);
	mreq.mr_ifindex = server->ifindex;
	mreq.mr_type = PACKET_MR_MULTICAST;
	mreq.mr_alen = ETH_ALEN;
	memcpy(mreq.mr_address, mac, ETH_ALEN);

	if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP,
				&mreq, sizeof(mreq)) < 0) {
		res = -errno;
		pw_log_error("setsockopt(ADD_MEMBERSHIP) failed: %m");
		return res;
	}

	if ((res = load_filter(fd, type, mac, server->mac_addr)) < 0)
		return res;

	return spa_steal_fd(fd);
}

int avb_server_make_socket(struct server *server, uint16_t type, const uint8_t mac[6])
{
	if (server->transport && server->transport->make_socket)
		return server->transport->make_socket(server, type, mac);
	return raw_make_socket(server, type, mac);
}

static int raw_transport_setup(struct server *server)
{
	struct impl *impl = server->impl;
	int res;
	static const uint8_t bmac[6] = AVB_BROADCAST_MAC;

	spa_autoclose int fd = raw_make_socket(server, AVB_TSN_ETH, bmac);
	if (fd < 0)
		return fd;

	pw_log_info("0x%"PRIx64" %d", server->entity_id, server->ifindex);

	server->source = pw_loop_add_io(impl->loop, spa_steal_fd(fd), SPA_IO_IN, true, on_socket_data, server);
	if (server->source == NULL) {
		res = -errno;
		pw_log_error("server %p: can't create server source: %m", impl);
		return res;
	}

	if ((res = pw_timer_queue_add(impl->timer_queue, &server->timer,
			NULL, DEFAULT_INTERVAL_MS * SPA_NSEC_PER_MSEC,
			on_timer_event, server)) < 0) {
		pw_log_error("server %p: can't create timer: %s", impl, spa_strerror(res));
		pw_loop_destroy_source(impl->loop, server->source);
		server->source = NULL;
		return res;
	}
	return 0;
}

/* milan-avb: hand-rolled netlink VLAN sub-iface creator.
 *
 * On I210-class NICs with rx-vlan-filter[fixed]=on the silicon drops
 * VLAN-tagged frames whose VID is not registered. Registering happens
 * implicitly when a VLAN sub-iface is added via RTM_NEWLINK. We create
 * <parent>.<vid> on first use, never delete it (bounded leak: one
 * sub-iface per SR class), and bind the listener stream socket to it.
 *
 * Fall back to PACKET_MR_PROMISC if any step fails (no CAP_NET_ADMIN,
 * no 8021q module loaded, etc.).  */

#define MILAN_NLALIGN(n) (((n) + 3U) & ~3U)

static int milan_nl_send(int fd, uint16_t mt, uint16_t flags,
		uint32_t seq, const void *payload, size_t plen)
{
	struct {
		struct nlmsghdr nlh;
		char body[2048];
	} msg = { 0 };
	size_t need = NLMSG_HDRLEN + plen;
	if (need > sizeof(msg))
		return -EMSGSIZE;
	msg.nlh.nlmsg_len = need;
	msg.nlh.nlmsg_type = mt;
	msg.nlh.nlmsg_flags = flags;
	msg.nlh.nlmsg_seq = seq;
	msg.nlh.nlmsg_pid = 0;
	if (plen)
		memcpy(msg.body, payload, plen);
	if (send(fd, &msg, need, 0) < 0)
		return -errno;
	return 0;
}

static int milan_nl_recv_ack(int fd, uint32_t seq)
{
	char buf[4096];
	for (;;) {
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
		size_t off = 0;
		if (n < 0)
			return -errno;
		while (off + NLMSG_HDRLEN <= (size_t)n) {
			struct nlmsghdr *h = (struct nlmsghdr *)(buf + off);
			if (h->nlmsg_len < NLMSG_HDRLEN ||
			    off + h->nlmsg_len > (size_t)n)
				break;
			if (h->nlmsg_type == NLMSG_ERROR && h->nlmsg_seq == seq) {
				struct nlmsgerr *e = NLMSG_DATA(h);
				return e->error;
			}
			off += NLMSG_ALIGN(h->nlmsg_len);
		}
	}
}

static size_t milan_nl_attr(char *p, uint16_t type, const void *val, uint16_t vlen)
{
	struct rtattr *r = (struct rtattr *)p;
	size_t total;
	r->rta_len = RTA_LENGTH(vlen);
	r->rta_type = type;
	memcpy(RTA_DATA(r), val, vlen);
	total = RTA_ALIGN(r->rta_len);
	if (total > (size_t)r->rta_len)
		memset(p + r->rta_len, 0, total - r->rta_len);
	return total;
}

/* Returns ifindex (>0) or -errno. */
static int milan_get_ifindex(const char *name)
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct ifreq req;
	int res, saved;
	if (fd < 0)
		return -errno;
	spa_zero(req);
	snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", name);
	res = ioctl(fd, SIOCGIFINDEX, &req);
	saved = -errno;
	close(fd);
	return res < 0 ? saved : req.ifr_ifindex;
}

/* Ensure <parent>.<vid> exists, is UP, and is a VLAN sub-iface of <parent>.
 * Writes the sub-iface name to out and returns 0 on success. */
static int milan_ensure_vlan_iface(const char *parent_ifname, uint16_t vid,
		char *out, size_t out_size)
{
	int rc, existing, parent_idx, nlfd, err, new_idx;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	char payload[256];
	char *p = payload;
	struct ifinfomsg ifi = { 0 };
	uint32_t pidx;
	char nested[64];
	char *np = nested;
	const char kind[] = "vlan";
	char data[16];
	char *dp = data;
	uint16_t vidv = vid;
	uint32_t seq = 1;
	struct ifinfomsg up_ifi = { 0 };

	if (vid == 0 || vid >= 4095)
		return -EINVAL;
	rc = snprintf(out, out_size, "%s.%u", parent_ifname, (unsigned)vid);
	if (rc < 0 || (size_t)rc >= out_size)
		return -ENAMETOOLONG;

	/* If the sub-iface already exists, assume it is what we want and reuse. */
	existing = milan_get_ifindex(out);
	if (existing > 0)
		return 0;

	parent_idx = milan_get_ifindex(parent_ifname);
	if (parent_idx <= 0)
		return parent_idx ? parent_idx : -ENODEV;

	nlfd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (nlfd < 0)
		return -errno;
	if (bind(nlfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int e = -errno; close(nlfd); return e;
	}

	/* RTM_NEWLINK: ifinfomsg + IFLA_IFNAME + IFLA_LINK + IFLA_LINKINFO{KIND=vlan, DATA{VLAN_ID}} */
	ifi.ifi_family = AF_UNSPEC;
	ifi.ifi_change = 0xFFFFFFFFu;
	memcpy(p, &ifi, sizeof(ifi)); p += sizeof(ifi);
	p += milan_nl_attr(p, IFLA_IFNAME, out, (uint16_t)(strlen(out) + 1));
	pidx = (uint32_t)parent_idx;
	p += milan_nl_attr(p, IFLA_LINK, &pidx, sizeof(pidx));

	/* LINKINFO is nested: KIND=vlan, DATA={VLAN_ID=vid} */
	np += milan_nl_attr(np, IFLA_INFO_KIND, kind, sizeof(kind));
	dp += milan_nl_attr(dp, IFLA_VLAN_ID, &vidv, sizeof(vidv));
	np += milan_nl_attr(np, IFLA_INFO_DATA, data, (uint16_t)(dp - data));
	p += milan_nl_attr(p, IFLA_LINKINFO, nested, (uint16_t)(np - nested));

	err = milan_nl_send(nlfd, RTM_NEWLINK,
			NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK,
			seq, payload, (size_t)(p - payload));
	if (err == 0)
		err = milan_nl_recv_ack(nlfd, seq);
	if (err != 0 && err != -EEXIST) {
		close(nlfd);
		return err;
	}

	/* Bring it UP. */
	new_idx = milan_get_ifindex(out);
	if (new_idx <= 0) {
		close(nlfd);
		return new_idx ? new_idx : -ENODEV;
	}
	up_ifi.ifi_family = AF_UNSPEC;
	up_ifi.ifi_index = new_idx;
	up_ifi.ifi_flags = IFF_UP;
	up_ifi.ifi_change = IFF_UP;
	err = milan_nl_send(nlfd, RTM_NEWLINK, NLM_F_REQUEST | NLM_F_ACK,
			++seq, &up_ifi, sizeof(up_ifi));
	if (err == 0)
		err = milan_nl_recv_ack(nlfd, seq);
	close(nlfd);
	if (err != 0)
		return err;

	return 0;
}

static int raw_stream_setup_socket(struct server *server, struct stream *stream)
{
	int res;
	char buf[128];
	struct ifreq req;
	const char *bind_ifname = server->ifname;
	char vlan_ifname[IFNAMSIZ];
	bool used_vlan_subiface = false;

	spa_autoclose int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(ETH_P_ALL));
	if (fd < 0) {
		pw_log_error("socket() failed: %m");
		return -errno;
	}

	/* For listener RX: route stream via a VLAN sub-iface so the NIC's
	 * hardware filter accepts VID-tagged AAF without promisc on parent. */
	/* Listener-only: route stream RX via a VLAN sub-iface so the NIC accepts
	 * VID-tagged AAF without promisc on parent. OUTPUT direction stays on
	 * the parent because setup_pdu_milan_v12() already inserts a manual
	 * 802.1Q tag in the PDU header — the kernel would add a second tag
	 * (QinQ) if we bound the talker socket to enp6s0.<vid>. */
	if (stream->direction == SPA_DIRECTION_INPUT && stream->vlan_id > 0) {
		int e = milan_ensure_vlan_iface(server->ifname,
				(uint16_t)stream->vlan_id,
				vlan_ifname, sizeof(vlan_ifname));
		if (e == 0) {
			bind_ifname = vlan_ifname;
			used_vlan_subiface = true;
			pw_log_info("milan-avb: listener RX via VLAN sub-iface %s (vid %d)",
					vlan_ifname, stream->vlan_id);
		} else {
			pw_log_warn("milan-avb: VLAN sub-iface setup failed (%d), "
					"falling back to PACKET_MR_PROMISC on parent", -e);
		}
	}

	spa_zero(req);
	snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", bind_ifname);
	res = ioctl(fd, SIOCGIFINDEX, &req);
	if (res < 0) {
		pw_log_error("SIOCGIFINDEX %s failed: %m", server->ifname);
		return -errno;
	}

	spa_zero(stream->sock_addr);
	stream->sock_addr.sll_family = AF_PACKET;
	stream->sock_addr.sll_protocol = htons(ETH_P_TSN);
	stream->sock_addr.sll_ifindex = req.ifr_ifindex;

	if (stream->direction == SPA_DIRECTION_OUTPUT) {
		/* CBS/Qav-exclusive: set only the traffic-class priority so the egress
		 * CBS qdisc shapes this stream. SO_TXTIME (launch-time/ETF) is NOT set --
		 * CBS and SO_TXTIME cannot coexist on the same queue. */
		res = setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &stream->prio,
				sizeof(stream->prio));
		if (res < 0) {
			pw_log_error("setsockopt(SO_PRIORITY %d) failed: %m", stream->prio);
			return -errno;
		}
	} else {
		struct packet_mreq mreq;

		res = bind(fd, (struct sockaddr *) &stream->sock_addr, sizeof(stream->sock_addr));
		if (res < 0) {
			pw_log_error("bind() failed: %m");
			return -errno;
		}

		spa_zero(mreq);
		mreq.mr_ifindex = req.ifr_ifindex;
		mreq.mr_type = PACKET_MR_MULTICAST;
		mreq.mr_alen = ETH_ALEN;
		memcpy(&mreq.mr_address, stream->addr, ETH_ALEN);
		res = setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP,
				&mreq, sizeof(struct packet_mreq));

		pw_log_info("join %s", avb_utils_format_addr(buf, 128, stream->addr));

		if (res < 0) {
			pw_log_error("setsockopt(ADD_MEMBERSHIP) failed: %m");
			return -errno;
		}

		/* Fallback: lift promisc only when the VLAN sub-iface path didn't
		 * take. With the sub-iface, the NIC accepts VID 2 natively. */
		if (!used_vlan_subiface) {
			spa_zero(mreq);
			mreq.mr_ifindex = req.ifr_ifindex;
			mreq.mr_type = PACKET_MR_PROMISC;
			res = setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP,
					&mreq, sizeof(struct packet_mreq));
			if (res < 0)
				pw_log_warn("setsockopt(PACKET_MR_PROMISC) fallback failed: %m");
		}
	}
	return spa_steal_fd(fd);
}

static ssize_t raw_stream_send(struct server *server, struct stream *stream,
		struct msghdr *msg, int flags)
{
	return sendmsg(stream->source->fd, msg, flags);
}

int avb_server_stream_setup_socket(struct server *server, struct stream *stream)
{
	return server->transport->stream_setup_socket(server, stream);
}

ssize_t avb_server_stream_send(struct server *server, struct stream *stream,
		struct msghdr *msg, int flags)
{
	return server->transport->stream_send(server, stream, msg, flags);
}

static void raw_transport_destroy(struct server *server)
{
	struct impl *impl = server->impl;
	if (server->source)
		pw_loop_destroy_source(impl->loop, server->source);
	server->source = NULL;
}

const struct avb_transport_ops avb_transport_raw = {
	.setup = raw_transport_setup,
	.send_packet = raw_send_packet,
	.make_socket = raw_make_socket,
	.destroy = raw_transport_destroy,
	.stream_setup_socket = raw_stream_setup_socket,
	.stream_send = raw_stream_send,
};

struct server *avdecc_server_new(struct impl *impl, struct spa_dict *props)
{
	struct server *server;
	const char *str;
	int res = 0;

	server = calloc(1, sizeof(*server));
	if (server == NULL)
		return NULL;

	server->impl = impl;
	spa_list_append(&impl->servers, &server->link);
	str = spa_dict_lookup(props, "ifname");
	server->ifname = str ? strdup(str) : NULL;

	if ((str = spa_dict_lookup(props, "milan")) != NULL && spa_atob(str))
		server->avb_mode = AVB_MODE_MILAN_V12;
	else
		server->avb_mode = AVB_MODE_LEGACY;

	spa_hook_list_init(&server->listener_list);
	spa_list_init(&server->descriptors);
	spa_list_init(&server->streams);

	server->debug_messages = false;

	if (server->transport == NULL)
		server->transport = &avb_transport_raw;

	if ((res = server->transport->setup(server)) < 0)
		goto error_free;

	server->gptp = avb_gptp_new(server);
	if (server->gptp == NULL)
		goto error_free;

	server->mrp = avb_mrp_new(server);
	if (server->mrp == NULL)
		goto error_free;

	avb_aecp_register(server);
	server->maap = avb_maap_register(server);
	server->mmrp = avb_mmrp_register(server);
	server->msrp = avb_msrp_register(server);
	server->mvrp = avb_mvrp_register(server);
	server->adp  = avb_adp_register(server);
	server->acmp = avb_acmp_register(server);

	avb_maap_reserve(server->maap, 1);

	init_descriptors(server);

	return server;

error_free:
	free(server);
	if (res < 0)
		errno = -res;
	return NULL;
}

void avdecc_server_add_listener(struct server *server, struct spa_hook *listener,
		const struct server_events *events, void *data)
{
	spa_hook_list_append(&server->listener_list, listener, events, data);
}

void avdecc_server_free(struct server *server)
{
	server_destroy_descriptors(server);
	spa_list_remove(&server->link);
	if (server->transport)
		server->transport->destroy(server);
	pw_timer_queue_cancel(&server->timer);
	spa_hook_list_clean(&server->listener_list);
	free(server->ifname);
	free(server);
}

const char *get_avb_mode_str(enum avb_mode mode)
{
	return avb_mode_str[mode];
}

void avb_log_state(struct server *server, const char *label)
{
	if (server == NULL)
		return;

	pw_log_debug("===== state @ %s =====", label);
	adp_log_state(server, label);
	avb_msrp_log_state(server, label);
	if (server->avb_mode == AVB_MODE_MILAN_V12)
		acmp_log_state_milan_v12(server, label);
	pw_log_debug("===== end state =====");
}
