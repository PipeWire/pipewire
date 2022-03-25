/* AVB support
 *
 * Copyright © 2022 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

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
