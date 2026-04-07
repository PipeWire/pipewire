/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2026 PipeWire contributors */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_TRANSPORT_LOOPBACK_H
#define AVB_TRANSPORT_LOOPBACK_H

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

#include "internal.h"
#include "packets.h"

#define AVB_LOOPBACK_MAX_PACKETS	64
#define AVB_LOOPBACK_MAX_PACKET_SIZE	2048

struct avb_loopback_packet {
	uint8_t dest[6];
	uint16_t type;
	size_t size;
	uint8_t data[AVB_LOOPBACK_MAX_PACKET_SIZE];
};

struct avb_loopback_transport {
	struct avb_loopback_packet packets[AVB_LOOPBACK_MAX_PACKETS];
	int packet_count;
	int packet_read;
};

static inline int avb_loopback_setup(struct server *server)
{
	struct avb_loopback_transport *t;
	static const uint8_t test_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

	t = calloc(1, sizeof(*t));
	if (t == NULL)
		return -errno;

	server->transport_data = t;

	memcpy(server->mac_addr, test_mac, 6);
	server->ifindex = 1;
	server->entity_id = (uint64_t)server->mac_addr[0] << 56 |
			(uint64_t)server->mac_addr[1] << 48 |
			(uint64_t)server->mac_addr[2] << 40 |
			(uint64_t)0xff << 32 |
			(uint64_t)0xfe << 24 |
			(uint64_t)server->mac_addr[3] << 16 |
			(uint64_t)server->mac_addr[4] << 8 |
			(uint64_t)server->mac_addr[5];

	return 0;
}

static inline int avb_loopback_send_packet(struct server *server,
		const uint8_t dest[6], uint16_t type, void *data, size_t size)
{
	struct avb_loopback_transport *t = server->transport_data;
	struct avb_loopback_packet *pkt;
	struct avb_ethernet_header *hdr = (struct avb_ethernet_header*)data;

	if (t->packet_count >= AVB_LOOPBACK_MAX_PACKETS)
		return -ENOSPC;
	if (size > AVB_LOOPBACK_MAX_PACKET_SIZE)
		return -EMSGSIZE;

	/* Fill in the ethernet header like the raw transport does */
	memcpy(hdr->dest, dest, 6);
	memcpy(hdr->src, server->mac_addr, 6);
	hdr->type = htons(type);

	pkt = &t->packets[t->packet_count % AVB_LOOPBACK_MAX_PACKETS];
	memcpy(pkt->dest, dest, 6);
	pkt->type = type;
	pkt->size = size;
	memcpy(pkt->data, data, size);
	t->packet_count++;

	return 0;
}

/**
 * Return a dummy fd for protocol handlers that create their own sockets.
 * Uses eventfd so pw_loop_add_io() has a valid fd to work with.
 */
static inline int avb_loopback_make_socket(struct server *server,
		uint16_t type, const uint8_t mac[6])
{
	int fd;

	fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (fd < 0)
		return -errno;

	return fd;
}

static inline void avb_loopback_destroy(struct server *server)
{
	free(server->transport_data);
	server->transport_data = NULL;
}

/**
 * Create a dummy stream socket using eventfd.
 * No AF_PACKET, no ioctls, no privileges needed.
 */
static inline int avb_loopback_stream_setup_socket(struct server *server,
		struct stream *stream)
{
	int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (fd < 0)
		return -errno;

	spa_zero(stream->sock_addr);
	stream->sock_addr.sll_family = AF_PACKET;
	stream->sock_addr.sll_halen = ETH_ALEN;

	return fd;
}

/**
 * No-op stream send — pretend the send succeeded.
 * Audio data is consumed from the ringbuffer but goes nowhere.
 */
static inline ssize_t avb_loopback_stream_send(struct server *server,
		struct stream *stream, struct msghdr *msg, int flags)
{
	ssize_t total = 0;
	for (size_t i = 0; i < msg->msg_iovlen; i++)
		total += msg->msg_iov[i].iov_len;
	return total;
}

static const struct avb_transport_ops avb_transport_loopback = {
	.setup = avb_loopback_setup,
	.send_packet = avb_loopback_send_packet,
	.make_socket = avb_loopback_make_socket,
	.destroy = avb_loopback_destroy,
	.stream_setup_socket = avb_loopback_stream_setup_socket,
	.stream_send = avb_loopback_stream_send,
};

/** Get the number of captured sent packets */
static inline int avb_loopback_get_packet_count(struct server *server)
{
	struct avb_loopback_transport *t = server->transport_data;
	return t->packet_count - t->packet_read;
}

/** Read the next captured sent packet, returns packet size or -1 */
static inline int avb_loopback_get_packet(struct server *server,
		void *buf, size_t bufsize)
{
	struct avb_loopback_transport *t = server->transport_data;
	struct avb_loopback_packet *pkt;

	if (t->packet_read >= t->packet_count)
		return -1;

	pkt = &t->packets[t->packet_read % AVB_LOOPBACK_MAX_PACKETS];
	t->packet_read++;

	if (pkt->size > bufsize)
		return -1;

	memcpy(buf, pkt->data, pkt->size);
	return pkt->size;
}

/** Clear all captured packets */
static inline void avb_loopback_clear_packets(struct server *server)
{
	struct avb_loopback_transport *t = server->transport_data;
	t->packet_count = 0;
	t->packet_read = 0;
}

#endif /* AVB_TRANSPORT_LOOPBACK_H */
