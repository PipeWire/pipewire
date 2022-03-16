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

struct avbtp_ehternet_header {
	uint8_t dest[6];
	uint8_t src[6];
	uint16_t type;
} __attribute__ ((__packed__));

struct avbtp_packet_header {
	uint8_t subtype;
#if __BYTE_ORDER == __BIG_ENDIAN
	unsigned sv:1;			/* stream_id valid */
	unsigned version:3;
	unsigned subtype_data1:4;

	unsigned subtype_data2:5;
	unsigned len1:3;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned subtype_data1:4;
	unsigned version:3;
	unsigned sv:1;

	unsigned len1:3;
	unsigned subtype_data2:5;
#elif
#error "Unknown byte order"
#endif
	uint8_t len2:8;
} __attribute__ ((__packed__));

#define AVBTP_PACKET_SET_SUBTYPE(p,v)	((p)->subtype = (v))
#define AVBTP_PACKET_SET_SV(p,v)	((p)->sv = (v))
#define AVBTP_PACKET_SET_VERSION(p,v)	((p)->version = (v))
#define AVBTP_PACKET_SET_SUB1(p,v)	((p)->subtype_data1 = (v))
#define AVBTP_PACKET_SET_SUB2(p,v)	((p)->subtype_data2 = (v))
#define AVBTP_PACKET_SET_LENGTH(p,v)	((p)->len1 = ((v) >> 8),(p)->len2 = (v))

#define AVBTP_PACKET_GET_SUBTYPE(p)	((p)->subtype)
#define AVBTP_PACKET_GET_SV(p)		((p)->sv)
#define AVBTP_PACKET_GET_VERSION(p)	((p)->version)
#define AVBTP_PACKET_GET_SUB1(p)	((p)->subtype_data1)
#define AVBTP_PACKET_GET_SUB2(p)	((p)->subtype_data2)
#define AVBTP_PACKET_GET_LENGTH(p)	((p)->len1 << 8 | (p)->len2)

#endif /* AVBTP_PACKETS_H */
