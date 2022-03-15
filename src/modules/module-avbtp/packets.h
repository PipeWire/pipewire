/* Spa AVB support
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

#ifndef AVBTP_PACKETS_H
#define AVBTP_PACKETS_H

#include <arpa/inet.h>

#define AVBTP_SUBTYPE_61883_IIDC	0x00
#define AVBTP_SUBTYPE_MMA_STREAM	0x01
#define AVBTP_SUBTYPE_AAF		0x02
#define AVBTP_SUBTYPE_CVF		0x03
#define AVBTP_SUBTYPE_CRF		0x04
#define AVBTP_SUBTYPE_TSCF		0x05
#define AVBTP_SUBTYPE_SVF		0x06
#define AVBTP_SUBTYPE_RVF		0x07
#define AVBTP_SUBTYPE_AEF_CONTINUOUS	0x6E
#define AVBTP_SUBTYPE_VSF_STREAM	0x6F
#define AVBTP_SUBTYPE_EF_STREAM		0x7F
#define AVBTP_SUBTYPE_NTSCF		0x82
#define AVBTP_SUBTYPE_ESCF		0xEC
#define AVBTP_SUBTYPE_EECF		0xED
#define AVBTP_SUBTYPE_AEF_DISCRETE	0xEE
#define AVBTP_SUBTYPE_ADP		0xFA
#define AVBTP_SUBTYPE_AECP		0xFB
#define AVBTP_SUBTYPE_ACMP		0xFC
#define AVBTP_SUBTYPE_MAAP		0xFE
#define AVBTP_SUBTYPE_EF_CONTROL	0xFF

struct avbtp_packet_common {
	uint8_t subtype;
#if __BYTE_ORDER == __BIG_ENDIAN
	unsigned sv:1;			/* stream_id valid */
	unsigned version:3;
	unsigned subtype_data1:4;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned subtype_data1:4;
	unsigned version:3;
	unsigned sv:1;
#elif
#error "Unknown byte order"
#endif
	uint16_t subtype_data2;
	uint64_t stream_id;
	uint8_t payload[0];
} __attribute__ ((__packed__));

#define AVBTP_PACKET_SET_SUBTYPE(p,v)	((p)->subtype = (v))
#define AVBTP_PACKET_SET_SV(p,v)	((p)->sv = (v))
#define AVBTP_PACKET_SET_VERSION(p,v)	((p)->version = (v))
#define AVBTP_PACKET_SET_STREAM_ID(p,v)	((p)->stream_id = htobe64(v))

#define AVBTP_PACKET_GET_SUBTYPE(p)	((p)->subtype)
#define AVBTP_PACKET_GET_SV(p)		((p)->sv)
#define AVBTP_PACKET_GET_VERSION(p)	((p)->version)
#define AVBTP_PACKET_GET_STREAM_ID(p)	be64toh((p)->stream_id)

struct avbtp_packet_cc {
	uint8_t subtype;
#if __BYTE_ORDER == __BIG_ENDIAN
	unsigned sv:1;
	unsigned version:3;
	unsigned control_data1:4;

	unsigned status:5;
	unsigned len1:3;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned control_data1:4;
	unsigned version:3;
	unsigned sv:1;

	unsigned len1:3;
	unsigned status:5;
#endif
	uint8_t len2:8;
	uint64_t stream_id;
	uint8_t payload[0];
} __attribute__ ((__packed__));

#define AVBTP_PACKET_CC_SET_SUBTYPE(p,v)	((p)->subtype = (v))
#define AVBTP_PACKET_CC_SET_SV(p,v)		((p)->sv = (v))
#define AVBTP_PACKET_CC_SET_VERSION(p,v)	((p)->version = (v))
#define AVBTP_PACKET_CC_SET_STREAM_ID(p,v)	((p)->stream_id = htobe64(v))
#define AVBTP_PACKET_CC_SET_STATUS(p,v)		((p)->status = (v))
#define AVBTP_PACKET_CC_SET_LENGTH(p,v)		((p)->len1 = ((v) >> 8),(p)->len2 = (v))

#define AVBTP_PACKET_CC_GET_SUBTYPE(p)		((p)->subtype)
#define AVBTP_PACKET_CC_GET_SV(p)		((p)->sv)
#define AVBTP_PACKET_CC_GET_VERSION(p)		((p)->version)
#define AVBTP_PACKET_CC_GET_STREAM_ID(p)	be64toh((p)->stream_id)
#define AVBTP_PACKET_CC_GET_STATUS(p)		((p)->status)
#define AVBTP_PACKET_CC_GET_LENGTH(p)		((p)->len1 << 8 || (p)->len2)

#endif /* AVBTP_PACKETS_H */
