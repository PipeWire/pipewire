/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_MAAP_H
#define AVB_MAAP_H

#include "packets.h"
#include "internal.h"

#define AVB_TSN_ETH 0x22f0
#define AVB_MAAP_MAC { 0x91, 0xe0, 0xf0, 0x00, 0xff, 0x00 };

#define AVB_MAAP_MESSAGE_TYPE_PROBE		1
#define AVB_MAAP_MESSAGE_TYPE_DEFEND		2
#define AVB_MAAP_MESSAGE_TYPE_ANNOUNCE		3

struct avb_packet_maap {
	struct avb_packet_header hdr;
	uint64_t stream_id;
	uint8_t request_start[6];
	uint16_t request_count;
	uint8_t conflict_start[6];
	uint16_t conflict_count;
} __attribute__ ((__packed__));

#define AVB_PACKET_MAAP_SET_MESSAGE_TYPE(p,v)		AVB_PACKET_SET_SUB1(&(p)->hdr, v)
#define AVB_PACKET_MAAP_SET_MAAP_VERSION(p,v)		AVB_PACKET_SET_SUB2(&(p)->hdr, v)
#define AVB_PACKET_MAAP_SET_STREAM_ID(p,v)		((p)->stream_id = htobe64(v))
#define AVB_PACKET_MAAP_SET_REQUEST_START(p,v)		memcpy((p)->request_start, (v), 6)
#define AVB_PACKET_MAAP_SET_REQUEST_COUNT(p,v)		((p)->request_count = htons(v))
#define AVB_PACKET_MAAP_SET_CONFLICT_START(p,v)		memcpy((p)->conflict_start, (v), 6)
#define AVB_PACKET_MAAP_SET_CONFLICT_COUNT(p,v)		((p)->conflict_count = htons(v))

#define AVB_PACKET_MAAP_GET_MESSAGE_TYPE(p)		AVB_PACKET_GET_SUB1(&(p)->hdr)
#define AVB_PACKET_MAAP_GET_MAAP_VERSION(p)		AVB_PACKET_GET_SUB2(&(p)->hdr)
#define AVB_PACKET_MAAP_GET_STREAM_ID(p)		be64toh((p)->stream_id)
#define AVB_PACKET_MAAP_GET_REQUEST_START(p)		((p)->request_start)
#define AVB_PACKET_MAAP_GET_REQUEST_COUNT(p)		ntohs((p)->request_count)
#define AVB_PACKET_MAAP_GET_CONFLICT_START(p)		((p)->conflict_start)
#define AVB_PACKET_MAAP_GET_CONFLICT_COUNT(p)		ntohs((p)->conflict_count)

struct avb_maap;

struct avb_maap *avb_maap_register(struct server *server);

int avb_maap_reserve(struct avb_maap *maap, uint32_t count);
int avb_maap_get_address(struct avb_maap *maap, uint8_t addr[6], uint32_t index);

#endif /* AVB_MAAP_H */
