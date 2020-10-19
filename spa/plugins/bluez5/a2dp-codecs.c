/*
 * BlueALSA - bluez-a2dp.c
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-codecs.h"

#if ENABLE_MP3
const a2dp_mpeg_t bluez_a2dp_mpeg = {
	.layer =
		MPEG_LAYER_MP1 |
		MPEG_LAYER_MP2 |
		MPEG_LAYER_MP3,
	.crc = 1,
	.channel_mode =
		MPEG_CHANNEL_MODE_MONO |
		MPEG_CHANNEL_MODE_DUAL_CHANNEL |
		MPEG_CHANNEL_MODE_STEREO |
		MPEG_CHANNEL_MODE_JOINT_STEREO,
	.mpf = 1,
	.frequency =
		MPEG_SAMPLING_FREQ_16000 |
		MPEG_SAMPLING_FREQ_22050 |
		MPEG_SAMPLING_FREQ_24000 |
		MPEG_SAMPLING_FREQ_32000 |
		MPEG_SAMPLING_FREQ_44100 |
		MPEG_SAMPLING_FREQ_48000,
	.bitrate =
		MPEG_BIT_RATE_VBR |
		MPEG_BIT_RATE_320000 |
		MPEG_BIT_RATE_256000 |
		MPEG_BIT_RATE_224000 |
		MPEG_BIT_RATE_192000 |
		MPEG_BIT_RATE_160000 |
		MPEG_BIT_RATE_128000 |
		MPEG_BIT_RATE_112000 |
		MPEG_BIT_RATE_96000 |
		MPEG_BIT_RATE_80000 |
		MPEG_BIT_RATE_64000 |
		MPEG_BIT_RATE_56000 |
		MPEG_BIT_RATE_48000 |
		MPEG_BIT_RATE_40000 |
		MPEG_BIT_RATE_32000 |
		MPEG_BIT_RATE_FREE,
};
#endif

#if ENABLE_APTX
const a2dp_aptx_t bluez_a2dp_aptx = {
	.info.vendor_id = APTX_VENDOR_ID,
	.info.codec_id = APTX_CODEC_ID,
	.channel_mode =
		/* NOTE: Used apt-X library does not support
		 *       single channel (mono) mode. */
		APTX_CHANNEL_MODE_DUAL_CHANNEL |
		APTX_CHANNEL_MODE_STEREO |
		APTX_CHANNEL_MODE_JOINT_STEREO,
	.frequency =
		APTX_SAMPLING_FREQ_16000 |
		APTX_SAMPLING_FREQ_32000 |
		APTX_SAMPLING_FREQ_44100 |
		APTX_SAMPLING_FREQ_48000,
};
#endif

extern struct a2dp_codec a2dp_codec_sbc;
#if ENABLE_AAC
extern struct a2dp_codec a2dp_codec_aac;
#endif
#if ENABLE_MP3
extern struct a2dp_codec a2dp_codec_mpeg;
#endif
#if ENABLE_APTX
extern struct a2dp_codec a2dp_codec_aptx;
#endif

const struct a2dp_codec *a2dp_codec_list[] = {
	&a2dp_codec_sbc,
#if ENABLE_AAC
	&a2dp_codec_aac,
#endif
#if ENABLE_MP3
	&a2dp_codec_mpeg,
#endif
#if ENABLE_APTX
	&a2dp_codec_aptx,
#endif
	NULL,
};
const struct a2dp_codec **a2dp_codecs = a2dp_codec_list;
