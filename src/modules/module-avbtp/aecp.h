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

#ifndef AVBTP_AECP_H
#define AVBTP_AECP_H

#include "packets.h"
#include "internal.h"

#define AVBTP_AECP_MESSAGE_TYPE_AEM_COMMAND		0
#define AVBTP_AECP_MESSAGE_TYPE_AEM_RESPONSE		1
#define AVBTP_AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND	2
#define AVBTP_AECP_MESSAGE_TYPE_ADDRESS_ACCESS_RESPONSE	3
#define AVBTP_AECP_MESSAGE_TYPE_AVC_COMMAND		4
#define AVBTP_AECP_MESSAGE_TYPE_AVC_RESPONSE		5
#define AVBTP_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND	6
#define AVBTP_AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE	7
#define AVBTP_AECP_MESSAGE_TYPE_EXTENDED_COMMAND	14
#define AVBTP_AECP_MESSAGE_TYPE_EXTENDED_RESPONSE	15

#define AVBTP_AECP_STATUS_SUCCESS			0
#define AVBTP_AECP_STATUS_NOT_IMPLEMENTED		1

struct avbtp_packet_aecp_header {
	struct avbtp_packet_header hdr;
	uint64_t target_guid;
	uint64_t controller_guid;
	uint16_t sequence_id;
} __attribute__ ((__packed__));

#define AVBTP_PACKET_AECP_SET_MESSAGE_TYPE(p,v)		AVBTP_PACKET_SET_SUB1(&(p)->hdr, v)
#define AVBTP_PACKET_AECP_SET_STATUS(p,v)		AVBTP_PACKET_SET_SUB2(&(p)->hdr, v)
#define AVBTP_PACKET_AECP_SET_TARGET_GUID(p,v)		((p)->target_guid = htobe64(v))
#define AVBTP_PACKET_AECP_SET_CONTROLLER_GUID(p,v)	((p)->controller_guid = htobe64(v))
#define AVBTP_PACKET_AECP_SET_SEQUENCE_ID(p,v)		((p)->sequence_id = htons(v))

#define AVBTP_PACKET_AECP_GET_MESSAGE_TYPE(p)		AVBTP_PACKET_GET_SUB1(&(p)->hdr)
#define AVBTP_PACKET_AECP_GET_STATUS(p)			AVBTP_PACKET_GET_SUB2(&(p)->hdr)
#define AVBTP_PACKET_AECP_GET_TARGET_GUID(p,v)		be64toh((p)->target_guid)
#define AVBTP_PACKET_AECP_GET_CONTROLLER_GUID(p,v)	be64toh((p)->controller_guid)
#define AVBTP_PACKET_AECP_GET_SEQUENCE_ID(p,v)		ntohs((p)->sequence_id)

struct avbtp_aecp *avbtp_aecp_register(struct server *server);

#endif /* AVBTP_AECP_H */
