/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/filter.h>
#include <linux/net_tstamp.h>
#include <limits.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <spa/support/cpu.h>
#include <spa/debug/mem.h>
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
	int fd, res;
	struct ifreq req;
	struct packet_mreq mreq;
	struct sockaddr_ll sll;

	fd = socket(AF_PACKET, SOCK_RAW|SOCK_NONBLOCK, htons(ETH_P_ALL));
	if (fd < 0) {
		pw_log_error("socket() failed: %m");
		return -errno;
	}

	spa_zero(req);
	snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", server->ifname);
	if (ioctl(fd, SIOCGIFINDEX, &req) < 0) {
		res = -errno;
		pw_log_error("SIOCGIFINDEX %s failed: %m", server->ifname);
		goto error_close;
	}
	server->ifindex = req.ifr_ifindex;

	spa_zero(req);
	snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", server->ifname);
	if (ioctl(fd, SIOCGIFHWADDR, &req) < 0) {
		res = -errno;
		pw_log_error("SIOCGIFHWADDR %s failed: %m", server->ifname);
		goto error_close;
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
		goto error_close;
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
		goto error_close;
	}

	if ((res = load_filter(fd, type, mac, server->mac_addr)) < 0)
		goto error_close;

	return fd;

error_close:
	close(fd);
	return res;
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
	int fd, res;
	static const uint8_t bmac[6] = AVB_BROADCAST_MAC;

	fd = raw_make_socket(server, AVB_TSN_ETH, bmac);
	if (fd < 0)
		return fd;

	pw_log_info("0x%"PRIx64" %d", server->entity_id, server->ifindex);

	server->source = pw_loop_add_io(impl->loop, fd, SPA_IO_IN, true, on_socket_data, server);
	if (server->source == NULL) {
		res = -errno;
		pw_log_error("server %p: can't create server source: %m", impl);
		goto error_no_source;
	}

	if ((res = pw_timer_queue_add(impl->timer_queue, &server->timer,
			NULL, DEFAULT_INTERVAL_MS * SPA_NSEC_PER_MSEC,
			on_timer_event, server)) < 0) {
		pw_log_error("server %p: can't create timer: %s", impl, spa_strerror(res));
		goto error_no_timer;
	}
	return 0;

error_no_timer:
	pw_loop_destroy_source(impl->loop, server->source);
	server->source = NULL;
error_no_source:
	close(fd);
	return res;
}

static int raw_stream_setup_socket(struct server *server, struct stream *stream)
{
	int fd, res;
	char buf[128];
	struct ifreq req;

	fd = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK, htons(ETH_P_ALL));
	if (fd < 0) {
		pw_log_error("socket() failed: %m");
		return -errno;
	}

	spa_zero(req);
	snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", server->ifname);
	res = ioctl(fd, SIOCGIFINDEX, &req);
	if (res < 0) {
		pw_log_error("SIOCGIFINDEX %s failed: %m", server->ifname);
		res = -errno;
		goto error_close;
	}

	spa_zero(stream->sock_addr);
	stream->sock_addr.sll_family = AF_PACKET;
	stream->sock_addr.sll_protocol = htons(ETH_P_TSN);
	stream->sock_addr.sll_ifindex = req.ifr_ifindex;

	if (stream->direction == SPA_DIRECTION_OUTPUT) {
		struct sock_txtime txtime_cfg;

		res = setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &stream->prio,
				sizeof(stream->prio));
		if (res < 0) {
			pw_log_error("setsockopt(SO_PRIORITY %d) failed: %m", stream->prio);
			res = -errno;
			goto error_close;
		}

		txtime_cfg.clockid = CLOCK_TAI;
		txtime_cfg.flags = 0;
		res = setsockopt(fd, SOL_SOCKET, SO_TXTIME, &txtime_cfg,
				sizeof(txtime_cfg));
		if (res < 0) {
			pw_log_error("setsockopt(SO_TXTIME) failed: %m");
			res = -errno;
			goto error_close;
		}
	} else {
		struct packet_mreq mreq;

		res = bind(fd, (struct sockaddr *) &stream->sock_addr, sizeof(stream->sock_addr));
		if (res < 0) {
			pw_log_error("bind() failed: %m");
			res = -errno;
			goto error_close;
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
			res = -errno;
			goto error_close;
		}
	}
	return fd;

error_close:
	close(fd);
	return res;
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
