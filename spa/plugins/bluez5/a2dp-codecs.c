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

#if ENABLE_AAC
const a2dp_aac_t bluez_a2dp_aac = {
	.object_type =
		/* NOTE: AAC Long Term Prediction and AAC Scalable are
		 *       not supported by the FDK-AAC library. */
		AAC_OBJECT_TYPE_MPEG2_AAC_LC |
		AAC_OBJECT_TYPE_MPEG4_AAC_LC,
	AAC_INIT_FREQUENCY(
		AAC_SAMPLING_FREQ_8000 |
		AAC_SAMPLING_FREQ_11025 |
		AAC_SAMPLING_FREQ_12000 |
		AAC_SAMPLING_FREQ_16000 |
		AAC_SAMPLING_FREQ_22050 |
		AAC_SAMPLING_FREQ_24000 |
		AAC_SAMPLING_FREQ_32000 |
		AAC_SAMPLING_FREQ_44100 |
		AAC_SAMPLING_FREQ_48000 |
		AAC_SAMPLING_FREQ_64000 |
		AAC_SAMPLING_FREQ_88200 |
		AAC_SAMPLING_FREQ_96000)
	.channels =
		AAC_CHANNELS_1 |
		AAC_CHANNELS_2,
	.vbr = 1,
	AAC_INIT_BITRATE(0xFFFF)
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
