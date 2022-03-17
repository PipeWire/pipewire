/* AVBTP support
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

#ifndef AVBTP_AAF_H
#define AVBTP_AAF_H

struct avbtp_packet_aaf {
	struct avbtp_ethernet_header hdr;
	uint8_t subtype;
#if __BYTE_ORDER == __BIG_ENDIAN
	unsigned sv:1;
	unsigned version:3;
	unsigned mr:1;
	unsigned _r1:1;
	unsigned gv:1;
	unsigned tv:1;

	uint8_t seq_number;

	unsigned _r2:7;
	unsigned tu:1;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned tv:1;
	unsigned gv:1;
	unsigned _r1:1;
	unsigned mr:1;
	unsigned version:3;
	unsigned sv:1;

	uint8_t seq_num;

	unsigned tu:1;
	unsigned _r2:7;
#endif
	uint64_t stream_id;
	uint32_t timestamp;
#define AVBTP_AAF_FORMAT_USER		0x00
#define AVBTP_AAF_FORMAT_FLOAT_32BIT	0x01
#define AVBTP_AAF_FORMAT_INT_32BIT	0x02
#define AVBTP_AAF_FORMAT_INT_24BIT	0x03
#define AVBTP_AAF_FORMAT_INT_16BIT	0x04
#define AVBTP_AAF_FORMAT_AES3_32BIT	0x05
	uint8_t format;

#define AVBTP_AAF_PCM_NSR_USER		0x00
#define AVBTP_AAF_PCM_NSR_8KHZ		0x01
#define AVBTP_AAF_PCM_NSR_16KHZ		0x02
#define AVBTP_AAF_PCM_NSR_32KHZ		0x03
#define AVBTP_AAF_PCM_NSR_44_1KHZ	0x04
#define AVBTP_AAF_PCM_NSR_48KHZ		0x05
#define AVBTP_AAF_PCM_NSR_88_2KHZ	0x06
#define AVBTP_AAF_PCM_NSR_96KHZ		0x07
#define AVBTP_AAF_PCM_NSR_176_4KHZ	0x08
#define AVBTP_AAF_PCM_NSR_192KHZ	0x09
#define AVBTP_AAF_PCM_NSR_24KHZ		0x0A
#if __BYTE_ORDER == __BIG_ENDIAN
	unsigned nsr:4;
	unsigned _r3:4;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned _r3:4;
	unsigned nsr:4;
#endif
	uint8_t chan_per_frame;
	uint8_t bit_depth;
	uint16_t data_len;

#define AVBTP_AAF_PCM_SP_NORMAL		0x00
#define AVBTP_AAF_PCM_SP_SPARSE		0x01
#if __BYTE_ORDER == __BIG_ENDIAN
	unsigned _r4:3;
	unsigned sp:1;
	unsigned event:4;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned event:4;
	unsigned sp:1;
	unsigned _r4:3;
#endif
	uint8_t _r5;
	uint8_t payload[0];
} __attribute__ ((__packed__));

#define AVBTP_PACKET_AAF_SET_SUBTYPE(p,v)		((p)->subtype = (v))
#define AVBTP_PACKET_AAF_SET_SV(p,v)			((p)->sv = (v))
#define AVBTP_PACKET_AAF_SET_VERSION(p,v)		((p)->version = (v))
#define AVBTP_PACKET_AAF_SET_MR(p,v)			((p)->mr = (v))
#define AVBTP_PACKET_AAF_SET_GV(p,v)			((p)->gv = (v))
#define AVBTP_PACKET_AAF_SET_TV(p,v)			((p)->tv = (v))
#define AVBTP_PACKET_AAF_SET_SEQ_NUM(p,v)		((p)->seq_num = (v))
#define AVBTP_PACKET_AAF_SET_TU(p,v)			((p)->tu = (v))
#define AVBTP_PACKET_AAF_SET_STREAM_ID(p,v)		((p)->stream_id = htobe64(v))
#define AVBTP_PACKET_AAF_SET_TIMESTAMP(p,v)		((p)->timestamp = htonl(v))
#define AVBTP_PACKET_AAF_SET_DATA_LEN(p,v)		((p)->data_len = htons(v))
#define AVBTP_PACKET_AAF_SET_FORMAT(p,v)		((p)->format = (v))
#define AVBTP_PACKET_AAF_SET_NSR(p,v)			((p)->nsr = (v))
#define AVBTP_PACKET_AAF_SET_CHAN_PER_FRAME(p,v)	((p)->chan_per_frame = (v))
#define AVBTP_PACKET_AAF_SET_BIT_DEPTH(p,v)		((p)->bit_depth = (v))
#define AVBTP_PACKET_AAF_SET_SP(p,v)			((p)->sp = (v))
#define AVBTP_PACKET_AAF_SET_EVENT(p,v)			((p)->event = (v))

#define AVBTP_PACKET_AAF_GET_SUBTYPE(p)			((p)->subtype)
#define AVBTP_PACKET_AAF_GET_SV(p)			((p)->sv)
#define AVBTP_PACKET_AAF_GET_VERSION(p)			((p)->version)
#define AVBTP_PACKET_AAF_GET_MR(p)			((p)->mr)
#define AVBTP_PACKET_AAF_GET_GV(p)			((p)->gv)
#define AVBTP_PACKET_AAF_GET_TV(p)			((p)->tv)
#define AVBTP_PACKET_AAF_GET_SEQ_NUM(p)			((p)->seq_num)
#define AVBTP_PACKET_AAF_GET_TU(p)			((p)->tu)
#define AVBTP_PACKET_AAF_GET_STREAM_ID(p)		be64toh((p)->stream_id)
#define AVBTP_PACKET_AAF_GET_TIMESTAMP(p)		ntohl((p)->timestamp)
#define AVBTP_PACKET_AAF_GET_DATA_LEN(p)		ntohs((p)->data_len)
#define AVBTP_PACKET_AAF_GET_FORMAT(p)			((p)->format)
#define AVBTP_PACKET_AAF_GET_NSR(p)			((p)->nsr)
#define AVBTP_PACKET_AAF_GET_CHAN_PER_FRAME(p)		((p)->chan_per_frame)
#define AVBTP_PACKET_AAF_GET_BIT_DEPTH(p)		((p)->bit_depth)
#define AVBTP_PACKET_AAF_GET_SP(p)			((p)->sp)
#define AVBTP_PACKET_AAF_GET_EVENT(p)			((p)->event)


#endif /* AVBTP_AAF_H */
