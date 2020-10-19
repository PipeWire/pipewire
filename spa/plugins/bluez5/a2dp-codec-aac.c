/* Spa A2DP SBC codec
 *
 * Copyright © 2020 Wim Taymans
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

#include <unistd.h>
#include <stddef.h>
#include <errno.h>
#include <arpa/inet.h>

#include <spa/param/audio/format.h>

#include "defs.h"
#include "rtp.h"
#include "a2dp-codecs.h"

struct impl {
	struct rtp_header *header;
	struct rtp_payload *payload;

	int codesize;
	int frame_length;

	int min_bitpool;
	int max_bitpool;
};

static int codec_fill_caps(uint32_t flags, uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	const a2dp_aac_t a2dp_aac = {
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
	memcpy(caps, &a2dp_aac, sizeof(a2dp_aac));
	return sizeof(a2dp_aac);
}

static int codec_select_config(uint32_t flags, const void *caps, size_t caps_size,
			const struct spa_audio_info *info, uint8_t config[A2DP_MAX_CAPS_SIZE])
{
	a2dp_aac_t conf;
	int freq;

	if (caps_size < sizeof(conf))
		return -EINVAL;

	conf = *(a2dp_aac_t*)caps;

	if (conf.object_type & AAC_OBJECT_TYPE_MPEG2_AAC_LC)
		conf.object_type = AAC_OBJECT_TYPE_MPEG2_AAC_LC;
	else if (conf.object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LC)
		conf.object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LC;
	else if (conf.object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LTP)
		conf.object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LTP;
	else if (conf.object_type & AAC_OBJECT_TYPE_MPEG4_AAC_SCA)
		conf.object_type = AAC_OBJECT_TYPE_MPEG4_AAC_SCA;
	else
		return -ENOTSUP;

	freq = AAC_GET_FREQUENCY(conf);
	if (freq & AAC_SAMPLING_FREQ_48000)
		freq = AAC_SAMPLING_FREQ_48000;
	else if (freq & AAC_SAMPLING_FREQ_44100)
		freq = AAC_SAMPLING_FREQ_44100;
	else if (freq & AAC_SAMPLING_FREQ_64000)
		freq = AAC_SAMPLING_FREQ_64000;
	else if (freq & AAC_SAMPLING_FREQ_32000)
		freq = AAC_SAMPLING_FREQ_32000;
	else if (freq & AAC_SAMPLING_FREQ_88200)
		freq = AAC_SAMPLING_FREQ_88200;
	else if (freq & AAC_SAMPLING_FREQ_96000)
		freq = AAC_SAMPLING_FREQ_96000;
	else if (freq & AAC_SAMPLING_FREQ_24000)
		freq = AAC_SAMPLING_FREQ_24000;
	else if (freq & AAC_SAMPLING_FREQ_22050)
		freq = AAC_SAMPLING_FREQ_22050;
	else if (freq & AAC_SAMPLING_FREQ_16000)
		freq = AAC_SAMPLING_FREQ_16000;
	else if (freq & AAC_SAMPLING_FREQ_12000)
		freq = AAC_SAMPLING_FREQ_12000;
	else if (freq & AAC_SAMPLING_FREQ_11025)
		freq = AAC_SAMPLING_FREQ_11025;
	else if (freq & AAC_SAMPLING_FREQ_8000)
		freq = AAC_SAMPLING_FREQ_8000;
	else
		return -ENOTSUP;

	AAC_SET_FREQUENCY(conf, freq);

	if (conf.channels & AAC_CHANNELS_2)
		conf.channels = AAC_CHANNELS_2;
	else if (conf.channels & AAC_CHANNELS_1)
		conf.channels = AAC_CHANNELS_1;
	else
		return -ENOTSUP;

	memcpy(config, &conf, sizeof(conf));

	return sizeof(conf);
}

static void *codec_init(uint32_t flags, void *config, size_t config_len, struct spa_audio_info *info)
{
	struct impl *this;
	int res;

	this = calloc(1, sizeof(struct impl));
	if (this == NULL) {
		res = -errno;
		goto error;
	}

	spa_zero(*info);
	info->media_type = SPA_MEDIA_TYPE_audio;
	info->media_subtype = SPA_MEDIA_SUBTYPE_aac;

	return this;
error:
	errno = -res;
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;
	free(this);
}

struct a2dp_codec a2dp_codec_aac = {
	.codec_id = A2DP_CODEC_MPEG24,
	.name = "aac",
	.description = "AAC",
	.fill_caps = codec_fill_caps,
	.select_config = codec_select_config,
	.init = codec_init,
	.deinit = codec_deinit,
};
