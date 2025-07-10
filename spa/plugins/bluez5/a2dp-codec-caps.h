/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2010  Nokia Corporation
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *  Copyright (C) 2018       Pali Rohár <pali.rohar@gmail.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifndef SPA_BLUEZ5_A2DP_CODEC_CAPS_H_
#define SPA_BLUEZ5_A2DP_CODEC_CAPS_H_

#include <stdint.h>
#include <stddef.h>

#define A2DP_CODEC_SBC			0x00
#define A2DP_CODEC_MPEG12		0x01
#define A2DP_CODEC_MPEG24		0x02
#define A2DP_CODEC_ATRAC		0x03
#define A2DP_CODEC_VENDOR		0xFF

#define A2DP_MAX_CAPS_SIZE		254

/* customized 16-bit vendor extension */
#define A2DP_CODEC_VENDOR_APTX		0x4FFF
#define A2DP_CODEC_VENDOR_LDAC		0x2DFF

#define SBC_SAMPLING_FREQ_48000		(1 << 0)
#define SBC_SAMPLING_FREQ_44100		(1 << 1)
#define SBC_SAMPLING_FREQ_32000		(1 << 2)
#define SBC_SAMPLING_FREQ_16000		(1 << 3)

#define SBC_CHANNEL_MODE_JOINT_STEREO	(1 << 0)
#define SBC_CHANNEL_MODE_STEREO		(1 << 1)
#define SBC_CHANNEL_MODE_DUAL_CHANNEL	(1 << 2)
#define SBC_CHANNEL_MODE_MONO		(1 << 3)

#define SBC_BLOCK_LENGTH_16		(1 << 0)
#define SBC_BLOCK_LENGTH_12		(1 << 1)
#define SBC_BLOCK_LENGTH_8		(1 << 2)
#define SBC_BLOCK_LENGTH_4		(1 << 3)

#define SBC_SUBBANDS_8			(1 << 0)
#define SBC_SUBBANDS_4			(1 << 1)

#define SBC_ALLOCATION_LOUDNESS		(1 << 0)
#define SBC_ALLOCATION_SNR		(1 << 1)

#define SBC_MIN_BITPOOL 2
#define SBC_MAX_BITPOOL 64

#define MPEG_CHANNEL_MODE_JOINT_STEREO	(1 << 0)
#define MPEG_CHANNEL_MODE_STEREO	(1 << 1)
#define MPEG_CHANNEL_MODE_DUAL_CHANNEL	(1 << 2)
#define MPEG_CHANNEL_MODE_MONO		(1 << 3)

#define MPEG_LAYER_MP3			(1 << 0)
#define MPEG_LAYER_MP2			(1 << 1)
#define MPEG_LAYER_MP1			(1 << 2)

#define MPEG_SAMPLING_FREQ_48000	(1 << 0)
#define MPEG_SAMPLING_FREQ_44100	(1 << 1)
#define MPEG_SAMPLING_FREQ_32000	(1 << 2)
#define MPEG_SAMPLING_FREQ_24000	(1 << 3)
#define MPEG_SAMPLING_FREQ_22050	(1 << 4)
#define MPEG_SAMPLING_FREQ_16000	(1 << 5)

#define MPEG_BIT_RATE_VBR		0x8000
#define MPEG_BIT_RATE_320000		0x4000
#define MPEG_BIT_RATE_256000		0x2000
#define MPEG_BIT_RATE_224000		0x1000
#define MPEG_BIT_RATE_192000		0x0800
#define MPEG_BIT_RATE_160000		0x0400
#define MPEG_BIT_RATE_128000		0x0200
#define MPEG_BIT_RATE_112000		0x0100
#define MPEG_BIT_RATE_96000		0x0080
#define MPEG_BIT_RATE_80000		0x0040
#define MPEG_BIT_RATE_64000		0x0020
#define MPEG_BIT_RATE_56000		0x0010
#define MPEG_BIT_RATE_48000		0x0008
#define MPEG_BIT_RATE_40000		0x0004
#define MPEG_BIT_RATE_32000		0x0002
#define MPEG_BIT_RATE_FREE		0x0001

#define AAC_OBJECT_TYPE_MPEG2_AAC_LC	0x80
#define AAC_OBJECT_TYPE_MPEG4_AAC_LC	0x40
#define AAC_OBJECT_TYPE_MPEG4_AAC_LTP	0x20
#define AAC_OBJECT_TYPE_MPEG4_AAC_SCA	0x10
#define AAC_OBJECT_TYPE_MPEG4_AAC_ELD	0x02

#define AAC_SAMPLING_FREQ_8000		0x0800
#define AAC_SAMPLING_FREQ_11025		0x0400
#define AAC_SAMPLING_FREQ_12000		0x0200
#define AAC_SAMPLING_FREQ_16000		0x0100
#define AAC_SAMPLING_FREQ_22050		0x0080
#define AAC_SAMPLING_FREQ_24000		0x0040
#define AAC_SAMPLING_FREQ_32000		0x0020
#define AAC_SAMPLING_FREQ_44100		0x0010
#define AAC_SAMPLING_FREQ_48000		0x0008
#define AAC_SAMPLING_FREQ_64000		0x0004
#define AAC_SAMPLING_FREQ_88200		0x0002
#define AAC_SAMPLING_FREQ_96000		0x0001

#define AAC_CHANNELS_1			0x08
#define AAC_CHANNELS_2			0x04
#define AAC_CHANNELS_5_1		0x02
#define AAC_CHANNELS_7_1		0x01

#define AAC_GET_BITRATE(a) ((a).bitrate1 << 16 | \
					(a).bitrate2 << 8 | (a).bitrate3)
#define AAC_GET_FREQUENCY(a) ((a).frequency1 << 4 | (a).frequency2)

#define AAC_SET_BITRATE(a, b) \
	do { \
		(a).bitrate1 = ((b) >> 16) & 0x7f; \
		(a).bitrate2 = ((b) >> 8) & 0xff; \
		(a).bitrate3 = (b) & 0xff; \
	} while (0)
#define AAC_SET_FREQUENCY(a, f) \
	do { \
		(a).frequency1 = ((f) >> 4) & 0xff; \
		(a).frequency2 = (f) & 0x0f; \
	} while (0)

#define AAC_INIT_BITRATE(b) \
	.bitrate1 = ((b) >> 16) & 0x7f, \
	.bitrate2 = ((b) >> 8) & 0xff, \
	.bitrate3 = (b) & 0xff,
#define AAC_INIT_FREQUENCY(f) \
	.frequency1 = ((f) >> 4) & 0xff, \
	.frequency2 = (f) & 0x0f,

#define APTX_VENDOR_ID			0x0000004f
#define APTX_CODEC_ID			0x0001

#define APTX_CHANNEL_MODE_MONO		0x01
#define APTX_CHANNEL_MODE_STEREO	0x02

#define APTX_SAMPLING_FREQ_16000	0x08
#define APTX_SAMPLING_FREQ_32000	0x04
#define APTX_SAMPLING_FREQ_44100	0x02
#define APTX_SAMPLING_FREQ_48000	0x01

#define APTX_HD_VENDOR_ID               0x000000D7
#define APTX_HD_CODEC_ID                0x0024

#define APTX_HD_CHANNEL_MODE_MONO       0x1
#define APTX_HD_CHANNEL_MODE_STEREO     0x2

#define APTX_HD_SAMPLING_FREQ_16000     0x8
#define APTX_HD_SAMPLING_FREQ_32000     0x4
#define APTX_HD_SAMPLING_FREQ_44100     0x2
#define APTX_HD_SAMPLING_FREQ_48000     0x1

#define APTX_LL_VENDOR_ID		0x0000000a
#define APTX_LL_VENDOR_ID2		0x000000d7
#define APTX_LL_CODEC_ID		0x0002

/**
 * Default parameters for aptX LL (Sprint) encoder
 */
#define APTX_LL_TARGET_CODEC_LEVEL      180  /* target codec buffer level */
#define APTX_LL_INITIAL_CODEC_LEVEL     360  /* initial codec buffer level */
#define APTX_LL_SRA_MAX_RATE            50   /* x/10000 = 0.005 SRA rate */
#define APTX_LL_SRA_AVG_TIME            1    /* SRA averaging time = 1s */
#define APTX_LL_GOOD_WORKING_LEVEL      180  /* good working buffer level */

#define LDAC_VENDOR_ID			0x0000012d
#define LDAC_CODEC_ID			0x00aa

#define LDAC_CHANNEL_MODE_MONO		0x04
#define LDAC_CHANNEL_MODE_DUAL_CHANNEL	0x02
#define LDAC_CHANNEL_MODE_STEREO	0x01

#define LDAC_SAMPLING_FREQ_44100	0x20
#define LDAC_SAMPLING_FREQ_48000	0x10
#define LDAC_SAMPLING_FREQ_88200	0x08
#define LDAC_SAMPLING_FREQ_96000	0x04
#define LDAC_SAMPLING_FREQ_176400	0x02
#define LDAC_SAMPLING_FREQ_192000	0x01

#define FASTSTREAM_VENDOR_ID            0x0000000a
#define FASTSTREAM_CODEC_ID             0x0001

#define FASTSTREAM_DIRECTION_SINK       0x1
#define FASTSTREAM_DIRECTION_SOURCE     0x2

#define FASTSTREAM_SINK_SAMPLING_FREQ_44100     0x2
#define FASTSTREAM_SINK_SAMPLING_FREQ_48000     0x1

#define FASTSTREAM_SOURCE_SAMPLING_FREQ_16000   0x2

#define LC3PLUS_HR_GET_FRAME_DURATION(a) ((a).frame_duration & 0xf0)
#define LC3PLUS_HR_INIT_FRAME_DURATION(v) \
	.frame_duration = ((v) & 0xf0),
#define LC3PLUS_HR_SET_FRAME_DURATION(a, v)		\
	do {						\
		(a).frame_duration = ((v) & 0xf0);	\
	} while (0)

#define LC3PLUS_HR_GET_FREQUENCY(a) (((a).frequency1 << 8) | (a).frequency2)
#define LC3PLUS_HR_INIT_FREQUENCY(v)		\
	.frequency1 = (((v) >> 8) & 0xff),	\
	.frequency2 = ((v) & 0xff),
#define LC3PLUS_HR_SET_FREQUENCY(a, v)			\
	do {						\
		(a).frequency1 = ((v) >> 8) & 0xff;	\
		(a).frequency2 = (v) & 0xff;		\
	} while (0)

#define LC3PLUS_HR_VENDOR_ID		0x000008a9
#define LC3PLUS_HR_CODEC_ID		0x0001

#define LC3PLUS_HR_FRAME_DURATION_10MS	(1 << 6)
#define LC3PLUS_HR_FRAME_DURATION_5MS	(1 << 5)
#define LC3PLUS_HR_FRAME_DURATION_2_5MS	(1 << 4)

#define LC3PLUS_HR_CHANNELS_1		(1 << 7)
#define LC3PLUS_HR_CHANNELS_2		(1 << 6)

#define LC3PLUS_HR_SAMPLING_FREQ_48000	(1 << 8)
#define LC3PLUS_HR_SAMPLING_FREQ_96000	(1 << 7)

#define OPUS_05_VENDOR_ID		0x000005f1
#define OPUS_05_CODEC_ID		0x1005

#define OPUS_05_MAPPING_FAMILY_0	(1 << 0)
#define OPUS_05_MAPPING_FAMILY_1	(1 << 1)
#define OPUS_05_MAPPING_FAMILY_255	(1 << 2)

#define OPUS_05_FRAME_DURATION_25	(1 << 0)
#define OPUS_05_FRAME_DURATION_50	(1 << 1)
#define OPUS_05_FRAME_DURATION_100	(1 << 2)
#define OPUS_05_FRAME_DURATION_200	(1 << 3)
#define OPUS_05_FRAME_DURATION_400	(1 << 4)

#define OPUS_05_GET_UINT16(a, field)			\
	(((a).field ## 2 << 8) | (a).field ## 1)
#define OPUS_05_INIT_UINT16(field, v)			\
	.field ## 1 = ((v) & 0xff),			\
	.field ## 2 = (((v) >> 8) & 0xff),
#define OPUS_05_SET_UINT16(a, field, v)			\
	do {						\
		(a).field ## 1 = ((v) & 0xff);		\
		(a).field ## 2 = (((v) >> 8) & 0xff);	\
	} while (0)
#define OPUS_05_GET_UINT32(a, field)				\
	(((a).field ## 4 << 24) | ((a).field ## 3 << 16) |	\
	((a).field ## 2 << 8) | (a).field ## 1)
#define OPUS_05_INIT_UINT32(field, v)			\
	.field ## 1 = ((v) & 0xff),			\
	.field ## 2 = (((v) >> 8) & 0xff),		\
	.field ## 3 = (((v) >> 16) & 0xff),		\
	.field ## 4 = (((v) >> 24) & 0xff),
#define OPUS_05_SET_UINT32(a, field, v)			\
	do {						\
		(a).field ## 1 = ((v) & 0xff);		\
		(a).field ## 2 = (((v) >> 8) & 0xff);	\
		(a).field ## 3 = (((v) >> 16) & 0xff);	\
		(a).field ## 4 = (((v) >> 24) & 0xff);	\
	} while (0)

#define OPUS_05_GET_LOCATION(a) OPUS_05_GET_UINT32(a, location)
#define OPUS_05_INIT_LOCATION(v) OPUS_05_INIT_UINT32(location, v)
#define OPUS_05_SET_LOCATION(a, v) OPUS_05_SET_UINT32(a, location, v)

#define OPUS_05_GET_BITRATE(a) OPUS_05_GET_UINT16(a, bitrate)
#define OPUS_05_INIT_BITRATE(v) OPUS_05_INIT_UINT16(bitrate, v)
#define OPUS_05_SET_BITRATE(a, v) OPUS_05_SET_UINT16(a, bitrate, v)


#define OPUS_G_VENDOR_ID	0x000000e0
#define OPUS_G_CODEC_ID		0x0001

#define OPUS_G_FREQUENCY_MASK		0x80
#define OPUS_G_FREQUENCY_48000		0x80

#define OPUS_G_DURATION_MASK		0x18
#define OPUS_G_DURATION_100		0x08
#define OPUS_G_DURATION_200		0x10

#define OPUS_G_CHANNELS_MASK		0x07
#define OPUS_G_CHANNELS_MONO		0x01
#define OPUS_G_CHANNELS_STEREO		0x02
#define OPUS_G_CHANNELS_MONO_2		0x04

#define OPUS_G_GET_FREQUENCY(a) ((a).data & OPUS_G_FREQUENCY_MASK)
#define OPUS_G_GET_DURATION(a) ((a).data & OPUS_G_DURATION_MASK)
#define OPUS_G_GET_CHANNELS(a) ((a).data & OPUS_G_CHANNELS_MASK)

#define OPUS_G_SET(a, freq, dur, ch) \
	(a).data = ((freq) & OPUS_G_FREQUENCY_MASK) | ((dur) & OPUS_G_DURATION_MASK) | ((ch) & OPUS_G_CHANNELS_MASK)


typedef struct {
	uint32_t vendor_id;
	uint16_t codec_id;
} __attribute__ ((packed)) a2dp_vendor_codec_t;

typedef struct {
	a2dp_vendor_codec_t info;
	uint8_t frequency;
	uint8_t channel_mode;
} __attribute__ ((packed)) a2dp_ldac_t;

#if __BYTE_ORDER == __LITTLE_ENDIAN

typedef struct {
	uint8_t channel_mode:4;
	uint8_t frequency:4;
	uint8_t allocation_method:2;
	uint8_t subbands:2;
	uint8_t block_length:4;
	uint8_t min_bitpool;
	uint8_t max_bitpool;
} __attribute__ ((packed)) a2dp_sbc_t;

typedef struct {
	uint8_t channel_mode:4;
	uint8_t crc:1;
	uint8_t layer:3;
	uint8_t frequency:6;
	uint8_t mpf:1;
	uint8_t rfa:1;
	uint16_t bitrate;
} __attribute__ ((packed)) a2dp_mpeg_t;

typedef struct {
	uint8_t object_type;
	uint8_t frequency1;
	uint8_t channels:4;
	uint8_t frequency2:4;
	uint8_t bitrate1:7;
	uint8_t vbr:1;
	uint8_t bitrate2;
	uint8_t bitrate3;
} __attribute__ ((packed)) a2dp_aac_t;

typedef struct {
	a2dp_vendor_codec_t info;
	uint8_t channel_mode:4;
	uint8_t frequency:4;
} __attribute__ ((packed)) a2dp_aptx_t;

typedef struct {
	a2dp_vendor_codec_t info;
	uint8_t channel_mode:4;
	uint8_t frequency:4;
	uint32_t rfa;
} __attribute__ ((packed)) a2dp_aptx_hd_t;

typedef struct {
        a2dp_aptx_t aptx;
        uint8_t bidirect_link:1;
        uint8_t has_new_caps:1;
        uint8_t reserved:6;
} __attribute__ ((packed)) a2dp_aptx_ll_t;

typedef struct {
        a2dp_vendor_codec_t info;
        uint8_t direction;
        uint8_t sink_frequency:4;
        uint8_t source_frequency:4;
} __attribute__ ((packed)) a2dp_faststream_t;

#elif __BYTE_ORDER == __BIG_ENDIAN

typedef struct {
	uint8_t frequency:4;
	uint8_t channel_mode:4;
	uint8_t block_length:4;
	uint8_t subbands:2;
	uint8_t allocation_method:2;
	uint8_t min_bitpool;
	uint8_t max_bitpool;
} __attribute__ ((packed)) a2dp_sbc_t;

typedef struct {
	uint8_t layer:3;
	uint8_t crc:1;
	uint8_t channel_mode:4;
	uint8_t rfa:1;
	uint8_t mpf:1;
	uint8_t frequency:6;
	uint16_t bitrate;
} __attribute__ ((packed)) a2dp_mpeg_t;

typedef struct {
	uint8_t object_type;
	uint8_t frequency1;
	uint8_t frequency2:4;
	uint8_t channels:4;
	uint8_t vbr:1;
	uint8_t bitrate1:7;
	uint8_t bitrate2;
	uint8_t bitrate3;
} __attribute__ ((packed)) a2dp_aac_t;

typedef struct {
	a2dp_vendor_codec_t info;
	uint8_t frequency:4;
	uint8_t channel_mode:4;
} __attribute__ ((packed)) a2dp_aptx_t;

typedef struct {
	a2dp_vendor_codec_t info;
	uint8_t frequency:4;
	uint8_t channel_mode:4;
	uint32_t rfa;
} __attribute__ ((packed)) a2dp_aptx_hd_t;

typedef struct {
        a2dp_aptx_t aptx;
        uint8_t reserved:6;
        uint8_t has_new_caps:1;
        uint8_t bidirect_link:1;
} __attribute__ ((packed)) a2dp_aptx_ll_t;

typedef struct {
        a2dp_vendor_codec_t info;
        uint8_t direction;
        uint8_t source_frequency:4;
        uint8_t sink_frequency:4;
} __attribute__ ((packed)) a2dp_faststream_t;

#else
#error "Unknown byte order"
#endif

typedef struct {
        a2dp_aptx_ll_t base;
        uint8_t reserved;
        uint8_t target_level2;
        uint8_t target_level1;
        uint8_t initial_level2;
        uint8_t initial_level1;
        uint8_t sra_max_rate;
        uint8_t sra_avg_time;
        uint8_t good_working_level2;
        uint8_t good_working_level1;
} __attribute__ ((packed)) a2dp_aptx_ll_ext_t;

typedef struct {
        a2dp_vendor_codec_t info;
	uint8_t frame_duration;
	uint8_t channels;
	uint8_t frequency1;
	uint8_t frequency2;
} __attribute__ ((packed)) a2dp_lc3plus_hr_t;

typedef struct {
	uint8_t channels;
	uint8_t coupled_streams;
	uint8_t location1;
	uint8_t location2;
	uint8_t location3;
	uint8_t location4;
	uint8_t frame_duration;
	uint8_t bitrate1;
	uint8_t bitrate2;
} __attribute__ ((packed)) a2dp_opus_05_direction_t;

typedef struct {
	a2dp_vendor_codec_t info;
	a2dp_opus_05_direction_t main;
	a2dp_opus_05_direction_t bidi;
} __attribute__ ((packed)) a2dp_opus_05_t;

typedef struct {
	a2dp_vendor_codec_t info;
	uint8_t data;
} __attribute__ ((packed)) a2dp_opus_g_t;

#define ASHA_CODEC_G722	0x63

#endif
