/* Spa AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_PACKETS_H
#define AVB_PACKETS_H

#include <arpa/inet.h>

#define AVB_SUBTYPE_61883_IIDC		0x00
#define AVB_SUBTYPE_MMA_STREAM		0x01
#define AVB_SUBTYPE_AAF			0x02
#define AVB_SUBTYPE_CVF			0x03
#define AVB_SUBTYPE_CRF			0x04
#define AVB_SUBTYPE_TSCF		0x05
#define AVB_SUBTYPE_SVF			0x06
#define AVB_SUBTYPE_RVF			0x07
#define AVB_SUBTYPE_AEF_CONTINUOUS	0x6E
#define AVB_SUBTYPE_VSF_STREAM		0x6F
#define AVB_SUBTYPE_EF_STREAM		0x7F
#define AVB_SUBTYPE_NTSCF		0x82
#define AVB_SUBTYPE_ESCF		0xEC
#define AVB_SUBTYPE_EECF		0xED
#define AVB_SUBTYPE_AEF_DISCRETE	0xEE
#define AVB_SUBTYPE_ADP			0xFA
#define AVB_SUBTYPE_AECP		0xFB
#define AVB_SUBTYPE_ACMP		0xFC
#define AVB_SUBTYPE_MAAP		0xFE
#define AVB_SUBTYPE_EF_CONTROL		0xFF

struct avb_ethernet_header {
	uint8_t dest[6];
	uint8_t src[6];
	uint16_t type;
} __attribute__ ((__packed__));

struct avb_frame_header {
	uint8_t dest[6];
	uint8_t src[6];
	uint16_t type;		/* 802.1Q Virtual Lan 0x8100 */
	uint16_t prio_cfi_id;
	uint16_t etype;
} __attribute__ ((__packed__));

struct avb_packet_header {
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

#define AVB_PACKET_SET_SUBTYPE(p,v)	((p)->subtype = (v))
#define AVB_PACKET_SET_SV(p,v)		((p)->sv = (v))
#define AVB_PACKET_SET_VERSION(p,v)	((p)->version = (v))
#define AVB_PACKET_SET_SUB1(p,v)	((p)->subtype_data1 = (v))
#define AVB_PACKET_SET_SUB2(p,v)	((p)->subtype_data2 = (v))
#define AVB_PACKET_SET_LENGTH(p,v)	((p)->len1 = ((v) >> 8),(p)->len2 = (v))

#define AVB_PACKET_GET_SUBTYPE(p)	((p)->subtype)
#define AVB_PACKET_GET_SV(p)		((p)->sv)
#define AVB_PACKET_GET_VERSION(p)	((p)->version)
#define AVB_PACKET_GET_SUB1(p)		((p)->subtype_data1)
#define AVB_PACKET_GET_SUB2(p)		((p)->subtype_data2)
#define AVB_PACKET_GET_LENGTH(p)	((p)->len1 << 8 | (p)->len2)

#endif /* AVB_PACKETS_H */
