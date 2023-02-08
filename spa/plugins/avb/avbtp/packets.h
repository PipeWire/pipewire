/* Spa AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_AVB_PACKETS_H
#define SPA_AVB_PACKETS_H

#define SPA_AVBTP_SUBTYPE_61883_IIDC		0x00
#define SPA_AVBTP_SUBTYPE_MMA_STREAM		0x01
#define SPA_AVBTP_SUBTYPE_AAF			0x02
#define SPA_AVBTP_SUBTYPE_CVF			0x03
#define SPA_AVBTP_SUBTYPE_CRF			0x04
#define SPA_AVBTP_SUBTYPE_TSCF			0x05
#define SPA_AVBTP_SUBTYPE_SVF			0x06
#define SPA_AVBTP_SUBTYPE_RVF			0x07
#define SPA_AVBTP_SUBTYPE_AEF_CONTINUOUS	0x6E
#define SPA_AVBTP_SUBTYPE_VSF_STREAM		0x6F
#define SPA_AVBTP_SUBTYPE_EF_STREAM		0x7F
#define SPA_AVBTP_SUBTYPE_NTSCF			0x82
#define SPA_AVBTP_SUBTYPE_ESCF			0xEC
#define SPA_AVBTP_SUBTYPE_EECF			0xED
#define SPA_AVBTP_SUBTYPE_AEF_DISCRETE		0xEE
#define SPA_AVBTP_SUBTYPE_ADP			0xFA
#define SPA_AVBTP_SUBTYPE_AECP			0xFB
#define SPA_AVBTP_SUBTYPE_ACMP			0xFC
#define SPA_AVBTP_SUBTYPE_MAAP			0xFE
#define SPA_AVBTP_SUBTYPE_EF_CONTROL		0xFF

struct spa_avbtp_packet_common {
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

#define SPA_AVBTP_PACKET_SET_SUBTYPE(p,v)	((p)->subtype = (v))
#define SPA_AVBTP_PACKET_SET_SV(p,v)		((p)->sv = (v))
#define SPA_AVBTP_PACKET_SET_VERSION(p,v)	((p)->version = (v))
#define SPA_AVBTP_PACKET_SET_STREAM_ID(p,v)	((p)->stream_id = htobe64(v))

#define SPA_AVBTP_PACKET_GET_SUBTYPE(p)		((p)->subtype)
#define SPA_AVBTP_PACKET_GET_SV(p)		((p)->sv)
#define SPA_AVBTP_PACKET_GET_VERSION(p)		((p)->version)
#define SPA_AVBTP_PACKET_GET_STREAM_ID(p)	be64toh((p)->stream_id)

struct spa_avbtp_packet_cc {
	uint8_t subtype;
#if __BYTE_ORDER == __BIG_ENDIAN
	unsigned sv:1;
	unsigned version:3;
	unsigned control_data1:4;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	unsigned control_data1:4;
	unsigned version:3;
	unsigned sv:1;
#endif
	uint8_t status;
	uint16_t control_frame_length;
	uint64_t stream_id;
	uint8_t payload[0];
} __attribute__ ((__packed__));

#define SPA_AVBTP_PACKET_CC_SET_SUBTYPE(p,v)		((p)->subtype = (v))
#define SPA_AVBTP_PACKET_CC_SET_SV(p,v)			((p)->sv = (v))
#define SPA_AVBTP_PACKET_CC_SET_VERSION(p,v)		((p)->version = (v))
#define SPA_AVBTP_PACKET_CC_SET_STREAM_ID(p,v)		((p)->stream_id = htobe64(v))
#define SPA_AVBTP_PACKET_CC_SET_STATUS(p,v)		((p)->status = (v))
#define SPA_AVBTP_PACKET_CC_SET_LENGTH(p,v)		((p)->control_frame_length = htons(v))

#define SPA_AVBTP_PACKET_CC_GET_SUBTYPE(p)		((p)->subtype)
#define SPA_AVBTP_PACKET_CC_GET_SV(p)			((p)->sv)
#define SPA_AVBTP_PACKET_CC_GET_VERSION(p)		((p)->version)
#define SPA_AVBTP_PACKET_CC_GET_STREAM_ID(p)		be64toh((p)->stream_id)
#define SPA_AVBTP_PACKET_CC_GET_STATUS(p)		((p)->status)
#define SPA_AVBTP_PACKET_CC_GET_LENGTH(p)		ntohs((p)->control_frame_length)

/* AAF */
struct spa_avbtp_packet_aaf {
	uint8_t subtype;
#if __BYTE_ORDER == __BIG_ENDIAN
	unsigned sv:1;
	unsigned version:3;
	unsigned mr:1;
	unsigned _r1:1;
	unsigned gv:1;
	unsigned tv:1;

	uint8_t seq_num;

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
#define SPA_AVBTP_AAF_FORMAT_USER		0x00
#define SPA_AVBTP_AAF_FORMAT_FLOAT_32BIT	0x01
#define SPA_AVBTP_AAF_FORMAT_INT_32BIT		0x02
#define SPA_AVBTP_AAF_FORMAT_INT_24BIT		0x03
#define SPA_AVBTP_AAF_FORMAT_INT_16BIT		0x04
#define SPA_AVBTP_AAF_FORMAT_AES3_32BIT		0x05
	uint8_t format;

#define SPA_AVBTP_AAF_PCM_NSR_USER		0x00
#define SPA_AVBTP_AAF_PCM_NSR_8KHZ		0x01
#define SPA_AVBTP_AAF_PCM_NSR_16KHZ		0x02
#define SPA_AVBTP_AAF_PCM_NSR_32KHZ		0x03
#define SPA_AVBTP_AAF_PCM_NSR_44_1KHZ		0x04
#define SPA_AVBTP_AAF_PCM_NSR_48KHZ		0x05
#define SPA_AVBTP_AAF_PCM_NSR_88_2KHZ		0x06
#define SPA_AVBTP_AAF_PCM_NSR_96KHZ		0x07
#define SPA_AVBTP_AAF_PCM_NSR_176_4KHZ		0x08
#define SPA_AVBTP_AAF_PCM_NSR_192KHZ		0x09
#define SPA_AVBTP_AAF_PCM_NSR_24KHZ		0x0A
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

#define SPA_AVBTP_AAF_PCM_SP_NORMAL		0x00
#define SPA_AVBTP_AAF_PCM_SP_SPARSE		0x01
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

#define SPA_AVBTP_PACKET_AAF_SET_SUBTYPE(p,v)		((p)->subtype = (v))
#define SPA_AVBTP_PACKET_AAF_SET_SV(p,v)		((p)->sv = (v))
#define SPA_AVBTP_PACKET_AAF_SET_VERSION(p,v)		((p)->version = (v))
#define SPA_AVBTP_PACKET_AAF_SET_MR(p,v)		((p)->mr = (v))
#define SPA_AVBTP_PACKET_AAF_SET_GV(p,v)		((p)->gv = (v))
#define SPA_AVBTP_PACKET_AAF_SET_TV(p,v)		((p)->tv = (v))
#define SPA_AVBTP_PACKET_AAF_SET_SEQ_NUM(p,v)		((p)->seq_num = (v))
#define SPA_AVBTP_PACKET_AAF_SET_TU(p,v)		((p)->tu = (v))
#define SPA_AVBTP_PACKET_AAF_SET_STREAM_ID(p,v)		((p)->stream_id = htobe64(v))
#define SPA_AVBTP_PACKET_AAF_SET_TIMESTAMP(p,v)		((p)->timestamp = htonl(v))
#define SPA_AVBTP_PACKET_AAF_SET_DATA_LEN(p,v)		((p)->data_len = htons(v))
#define SPA_AVBTP_PACKET_AAF_SET_FORMAT(p,v)		((p)->format = (v))
#define SPA_AVBTP_PACKET_AAF_SET_NSR(p,v)		((p)->nsr = (v))
#define SPA_AVBTP_PACKET_AAF_SET_CHAN_PER_FRAME(p,v)	((p)->chan_per_frame = (v))
#define SPA_AVBTP_PACKET_AAF_SET_BIT_DEPTH(p,v)		((p)->bit_depth = (v))
#define SPA_AVBTP_PACKET_AAF_SET_SP(p,v)		((p)->sp = (v))
#define SPA_AVBTP_PACKET_AAF_SET_EVENT(p,v)		((p)->event = (v))

#define SPA_AVBTP_PACKET_AAF_GET_SUBTYPE(p)		((p)->subtype)
#define SPA_AVBTP_PACKET_AAF_GET_SV(p)			((p)->sv)
#define SPA_AVBTP_PACKET_AAF_GET_VERSION(p)		((p)->version)
#define SPA_AVBTP_PACKET_AAF_GET_MR(p)			((p)->mr)
#define SPA_AVBTP_PACKET_AAF_GET_GV(p)			((p)->gv)
#define SPA_AVBTP_PACKET_AAF_GET_TV(p)			((p)->tv)
#define SPA_AVBTP_PACKET_AAF_GET_SEQ_NUM(p)		((p)->seq_num)
#define SPA_AVBTP_PACKET_AAF_GET_TU(p)			((p)->tu)
#define SPA_AVBTP_PACKET_AAF_GET_STREAM_ID(p)		be64toh((p)->stream_id)
#define SPA_AVBTP_PACKET_AAF_GET_TIMESTAMP(p)		ntohl((p)->timestamp)
#define SPA_AVBTP_PACKET_AAF_GET_DATA_LEN(p)		ntohs((p)->data_len)
#define SPA_AVBTP_PACKET_AAF_GET_FORMAT(p)		((p)->format)
#define SPA_AVBTP_PACKET_AAF_GET_NSR(p)			((p)->nsr)
#define SPA_AVBTP_PACKET_AAF_GET_CHAN_PER_FRAME(p)	((p)->chan_per_frame)
#define SPA_AVBTP_PACKET_AAF_GET_BIT_DEPTH(p)		((p)->bit_depth)
#define SPA_AVBTP_PACKET_AAF_GET_SP(p)			((p)->sp)
#define SPA_AVBTP_PACKET_AAF_GET_EVENT(p)		((p)->event)


#endif /* SPA_AVB_PACKETS_H */
