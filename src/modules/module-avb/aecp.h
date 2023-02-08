/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_AECP_H
#define AVB_AECP_H

#include "packets.h"
#include "internal.h"

#define AVB_AECP_MESSAGE_TYPE_AEM_COMMAND		0
#define AVB_AECP_MESSAGE_TYPE_AEM_RESPONSE		1
#define AVB_AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND	2
#define AVB_AECP_MESSAGE_TYPE_ADDRESS_ACCESS_RESPONSE	3
#define AVB_AECP_MESSAGE_TYPE_AVC_COMMAND		4
#define AVB_AECP_MESSAGE_TYPE_AVC_RESPONSE		5
#define AVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND	6
#define AVB_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE	7
#define AVB_AECP_MESSAGE_TYPE_EXTENDED_COMMAND		14
#define AVB_AECP_MESSAGE_TYPE_EXTENDED_RESPONSE		15

#define AVB_AECP_STATUS_SUCCESS				0
#define AVB_AECP_STATUS_NOT_IMPLEMENTED			1

struct avb_packet_aecp_header {
	struct avb_packet_header hdr;
	uint64_t target_guid;
	uint64_t controller_guid;
	uint16_t sequence_id;
} __attribute__ ((__packed__));

#define AVB_PACKET_AECP_SET_MESSAGE_TYPE(p,v)		AVB_PACKET_SET_SUB1(&(p)->hdr, v)
#define AVB_PACKET_AECP_SET_STATUS(p,v)			AVB_PACKET_SET_SUB2(&(p)->hdr, v)

#define AVB_PACKET_AECP_GET_MESSAGE_TYPE(p)		AVB_PACKET_GET_SUB1(&(p)->hdr)
#define AVB_PACKET_AECP_GET_STATUS(p)			AVB_PACKET_GET_SUB2(&(p)->hdr)

struct avb_aecp *avb_aecp_register(struct server *server);

#endif /* AVB_AECP_H */
