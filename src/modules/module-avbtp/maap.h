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

#ifndef AVBTP_MAAP_H
#define AVBTP_MAAP_H

#include "packets.h"
#include "internal.h"

#define AVBTP_MAAP_MESSAGE_TYPE_PROBE			1
#define AVBTP_MAAP_MESSAGE_TYPE_DEFEND			2
#define AVBTP_MAAP_MESSAGE_TYPE_ANNOUNCE		3

struct avbtp_packet_maap {
	uint8_t subtype;
#if __BYTE_ORDER == __BIG_ENDIAN
	unsigned sv:1;
	unsigned version:3;
	unsigned message_type:4;

	unsigned maap_version:5;
	unsigned len1:3;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned message_type:4;
	unsigned version:3;
	unsigned sv:1;

	unsigned len1:3;
	unsigned maap_version:5;
#endif
	uint8_t len2:8;
	uint64_t stream_id;
	uint8_t request_start[6];
	uint16_t request_count;
	uint8_t conflict_start[6];
	uint16_t conflict_count;
} __attribute__ ((__packed__));

#define AVBTP_PACKET_MAAP_SET_SUBTYPE(p,v)		((p)->subtype = (v))
#define AVBTP_PACKET_MAAP_SET_SV(p,v)			((p)->sv = (v))
#define AVBTP_PACKET_MAAP_SET_VERSION(p,v)		((p)->version = (v))
#define AVBTP_PACKET_MAAP_SET_MESSAGE_TYPE(p,v)		((p)->message_type = (v))
#define AVBTP_PACKET_MAAP_SET_MAAP_VERSION(p,v)		((p)->maap_version = (v))
#define AVBTP_PACKET_MAAP_SET_LENGTH(p,v)		((p)->len1 = ((v) >> 8),(p)->len2 = (v))
#define AVBTP_PACKET_MAAP_SET_STREAM_ID(p,v)		((p)->stream_id = htobe64(v))
#define AVBTP_PACKET_MAAP_SET_REQUEST_START(p,v)	memcpy((p)->request_start, (v), 6)
#define AVBTP_PACKET_MAAP_SET_REQUEST_COUNT(p,v)	((p)->request_count = htons(v))
#define AVBTP_PACKET_MAAP_SET_CONFLICT_START(p,v)	memcpy((p)->conflict_start, (v), 6)
#define AVBTP_PACKET_MAAP_SET_CONFLICT_COUNT(p,v)	((p)->conflict_count = htons(v))

#define AVBTP_PACKET_MAAP_GET_SUBTYPE(p)		((p)->subtype)
#define AVBTP_PACKET_MAAP_GET_SV(p)			((p)->sv)
#define AVBTP_PACKET_MAAP_GET_VERSION(p)		((p)->version)
#define AVBTP_PACKET_MAAP_GET_MESSAGE_TYPE(p)		((p)->message_type)
#define AVBTP_PACKET_MAAP_GET_MAAP_VERSION(p)		((p)->maap_version)
#define AVBTP_PACKET_MAAP_GET_LENGTH(p)			(((p)->len1 << 8) | (p)->len2)
#define AVBTP_PACKET_MAAP_GET_STREAM_ID(p)		be64toh((p)->stream_id)
#define AVBTP_PACKET_MAAP_GET_REQUEST_START(p)		((p)->request_start)
#define AVBTP_PACKET_MAAP_GET_REQUEST_COUNT(p)		ntohs((p)->request_count)
#define AVBTP_PACKET_MAAP_GET_CONFLICT_START(p)		((p)->conflict_start)
#define AVBTP_PACKET_MAAP_GET_CONFLICT_COUNT(p)		ntohs((p)->conflict_count)

int avbtp_maap_register(struct server *server);

#endif /* AVBTP_MAAP_H */
