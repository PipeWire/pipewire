/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_AAF_H
#define AVB_AAF_H

struct avb_packet_aaf {
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
#define AVB_AAF_FORMAT_USER		0x00
#define AVB_AAF_FORMAT_FLOAT_32BIT	0x01
#define AVB_AAF_FORMAT_INT_32BIT	0x02
#define AVB_AAF_FORMAT_INT_24BIT	0x03
#define AVB_AAF_FORMAT_INT_16BIT	0x04
#define AVB_AAF_FORMAT_AES3_32BIT	0x05
	uint8_t format;

#define AVB_AAF_PCM_NSR_USER		0x00
#define AVB_AAF_PCM_NSR_8KHZ		0x01
#define AVB_AAF_PCM_NSR_16KHZ		0x02
#define AVB_AAF_PCM_NSR_32KHZ		0x03
#define AVB_AAF_PCM_NSR_44_1KHZ	0x04
#define AVB_AAF_PCM_NSR_48KHZ		0x05
#define AVB_AAF_PCM_NSR_88_2KHZ	0x06
#define AVB_AAF_PCM_NSR_96KHZ		0x07
#define AVB_AAF_PCM_NSR_176_4KHZ	0x08
#define AVB_AAF_PCM_NSR_192KHZ	0x09
#define AVB_AAF_PCM_NSR_24KHZ		0x0A
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

#define AVB_AAF_PCM_SP_NORMAL		0x00
#define AVB_AAF_PCM_SP_SPARSE		0x01
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

#endif /* AVB_AAF_H */
