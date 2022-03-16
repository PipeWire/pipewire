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

#define AVBTP_AECP_DATA_LENGTH	56

struct avbtp_packet_aecp {
	uint8_t subtype;
#if __BYTE_ORDER == __BIG_ENDIAN
	unsigned sv:1;
	unsigned version:3;
	unsigned message_type:4;

	unsigned valid_time:5;
	unsigned len1:3;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned message_type:4;
	unsigned version:3;
	unsigned sv:1;

	unsigned len1:3;
	unsigned valid_time:5;
#endif
	uint8_t len2:8;
} __attribute__ ((__packed__));

#define AVBTP_PACKET_AECP_SET_SUBTYPE(p,v)		((p)->subtype = (v))
#define AVBTP_PACKET_AECP_SET_SV(p,v)			((p)->sv = (v))
#define AVBTP_PACKET_AECP_SET_VERSION(p,v)		((p)->version = (v))
#define AVBTP_PACKET_AECP_SET_MESSAGE_TYPE(p,v)		((p)->message_type = (v))
#define AVBTP_PACKET_AECP_SET_VALID_TIME(p,v)		((p)->valid_time = (v))
#define AVBTP_PACKET_AECP_SET_LENGTH(p,v)		((p)->len1 = ((v) >> 8),(p)->len2 = (v))

#define AVBTP_PACKET_AECP_GET_SUBTYPE(p)			((p)->subtype)
#define AVBTP_PACKET_AECP_GET_SV(p)			((p)->sv)
#define AVBTP_PACKET_AECP_GET_VERSION(p)			((p)->version)
#define AVBTP_PACKET_AECP_GET_MESSAGE_TYPE(p)		((p)->message_type)
#define AVBTP_PACKET_AECP_GET_VALID_TIME(p)		((p)->valid_time)
#define AVBTP_PACKET_AECP_GET_LENGTH(p)			(((p)->len1 << 8) | (p)->len2)

struct avbtp_aecp *avbtp_aecp_register(struct server *server);

#endif /* AVBTP_AECP_H */
