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

#include <sbc/sbc.h>

#include "defs.h"
#include "rtp.h"
#include "a2dp-codecs.h"

#define MAX_FRAME_COUNT 16

struct impl {
	sbc_t sbc;

	struct rtp_header *header;
	struct rtp_payload *payload;

	int codesize;
	int frame_length;

	int min_bitpool;
	int max_bitpool;
};

static int codec_fill_caps(uint32_t flags, uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	const a2dp_sbc_t a2dp_sbc = {
		.frequency =
			SBC_SAMPLING_FREQ_16000 |
			SBC_SAMPLING_FREQ_32000 |
			SBC_SAMPLING_FREQ_44100 |
			SBC_SAMPLING_FREQ_48000,
		.channel_mode =
			SBC_CHANNEL_MODE_MONO |
			SBC_CHANNEL_MODE_DUAL_CHANNEL |
			SBC_CHANNEL_MODE_STEREO |
			SBC_CHANNEL_MODE_JOINT_STEREO,
		.block_length =
			SBC_BLOCK_LENGTH_4 |
			SBC_BLOCK_LENGTH_8 |
			SBC_BLOCK_LENGTH_12 |
			SBC_BLOCK_LENGTH_16,
		.subbands =
			SBC_SUBBANDS_4 |
			SBC_SUBBANDS_8,
		.allocation_method =
			SBC_ALLOCATION_SNR |
			SBC_ALLOCATION_LOUDNESS,
		.min_bitpool = SBC_MIN_BITPOOL,
		.max_bitpool = SBC_MAX_BITPOOL,
	};
	memcpy(caps, &a2dp_sbc, sizeof(a2dp_sbc));
	return sizeof(a2dp_sbc);
}

static uint8_t default_bitpool(uint8_t freq, uint8_t mode)
{
	/* These bitpool values were chosen based on the A2DP spec recommendation */
	switch (freq) {
	case SBC_SAMPLING_FREQ_16000:
        case SBC_SAMPLING_FREQ_32000:
		return 53;

	case SBC_SAMPLING_FREQ_44100:
		switch (mode) {
		case SBC_CHANNEL_MODE_MONO:
		case SBC_CHANNEL_MODE_DUAL_CHANNEL:
			return 31;

		case SBC_CHANNEL_MODE_STEREO:
		case SBC_CHANNEL_MODE_JOINT_STEREO:
			return 53;
		}
		return 53;
	case SBC_SAMPLING_FREQ_48000:
		switch (mode) {
		case SBC_CHANNEL_MODE_MONO:
		case SBC_CHANNEL_MODE_DUAL_CHANNEL:
			return 29;

		case SBC_CHANNEL_MODE_STEREO:
		case SBC_CHANNEL_MODE_JOINT_STEREO:
			return 51;
		}
		return 51;
	}
	return 53;
}

static int codec_select_config(uint32_t flags, const void *caps, size_t caps_size,
			const struct spa_audio_info *info, uint8_t config[A2DP_MAX_CAPS_SIZE])
{
	a2dp_sbc_t conf;
	int bitpool;

	if (caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (conf.frequency & SBC_SAMPLING_FREQ_48000)
		conf.frequency = SBC_SAMPLING_FREQ_48000;
	else if (conf.frequency & SBC_SAMPLING_FREQ_44100)
		conf.frequency = SBC_SAMPLING_FREQ_44100;
	else if (conf.frequency & SBC_SAMPLING_FREQ_32000)
		conf.frequency = SBC_SAMPLING_FREQ_32000;
	else if (conf.frequency & SBC_SAMPLING_FREQ_16000)
		conf.frequency = SBC_SAMPLING_FREQ_16000;
	else
		return -ENOTSUP;

	if (conf.channel_mode & SBC_CHANNEL_MODE_JOINT_STEREO)
		conf.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
	else if (conf.channel_mode & SBC_CHANNEL_MODE_STEREO)
		conf.channel_mode = SBC_CHANNEL_MODE_STEREO;
	else if (conf.channel_mode & SBC_CHANNEL_MODE_DUAL_CHANNEL)
		conf.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
	else if (conf.channel_mode & SBC_CHANNEL_MODE_MONO)
		conf.channel_mode = SBC_CHANNEL_MODE_MONO;
	else
		return -ENOTSUP;

	if (conf.block_length & SBC_BLOCK_LENGTH_16)
		conf.block_length = SBC_BLOCK_LENGTH_16;
	else if (conf.block_length & SBC_BLOCK_LENGTH_12)
		conf.block_length = SBC_BLOCK_LENGTH_12;
	else if (conf.block_length & SBC_BLOCK_LENGTH_8)
		conf.block_length = SBC_BLOCK_LENGTH_8;
	else if (conf.block_length & SBC_BLOCK_LENGTH_4)
		conf.block_length = SBC_BLOCK_LENGTH_4;
	else
		return -ENOTSUP;

	if (conf.subbands & SBC_SUBBANDS_8)
		conf.subbands = SBC_SUBBANDS_8;
	else if (conf.subbands & SBC_SUBBANDS_4)
		conf.subbands = SBC_SUBBANDS_4;
	else
		return -ENOTSUP;

	if (conf.allocation_method & SBC_ALLOCATION_LOUDNESS)
		conf.allocation_method = SBC_ALLOCATION_LOUDNESS;
	else if (conf.allocation_method & SBC_ALLOCATION_SNR)
		conf.allocation_method = SBC_ALLOCATION_SNR;
	else
		return -ENOTSUP;

	bitpool = default_bitpool(conf.frequency, conf.channel_mode);

	conf.min_bitpool = SPA_MAX(SBC_MIN_BITPOOL, conf.min_bitpool);
	conf.max_bitpool = SPA_MIN(bitpool, conf.max_bitpool);
	memcpy(config, &conf, sizeof(conf));

	return sizeof(conf);
}

static int codec_set_bitpool(struct impl *this, int bitpool)
{
	this->sbc.bitpool = SPA_CLAMP(bitpool, this->min_bitpool, this->max_bitpool);
	this->codesize = sbc_get_codesize(&this->sbc);
	this->frame_length = sbc_get_frame_length(&this->sbc);
	return this->sbc.bitpool;
}

static int codec_reduce_bitpool(void *data)
{
	struct impl *this = data;
	return codec_set_bitpool(this, this->sbc.bitpool - 2);
}

static int codec_increase_bitpool(void *data)
{
	struct impl *this = data;
	return codec_set_bitpool(this, this->sbc.bitpool + 1);
}

static int codec_get_num_blocks(void *data, size_t mtu)
{
	struct impl *this = data;
	size_t rtp_size = sizeof(struct rtp_header) + sizeof(struct rtp_payload);
	size_t frame_count = (mtu - rtp_size) / this->frame_length;

	/* frame_count is only 4 bit number */
	if (frame_count > 15)
		frame_count = 15;
	return frame_count;
}

static int codec_get_block_size(void *data)
{
	struct impl *this = data;
	return this->codesize;
}

static void *codec_init(uint32_t flags, void *config, size_t config_len, struct spa_audio_info *info)
{
	struct impl *this;
	a2dp_sbc_t *conf = config;
	int res;

	this = calloc(1, sizeof(struct impl));
	if (this == NULL) {
		res = -errno;
		goto error;
	}

	sbc_init(&this->sbc, 0);
	this->sbc.endian = SBC_LE;

	spa_zero(*info);
	info->media_type = SPA_MEDIA_TYPE_audio;
	info->media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info->info.raw.format = SPA_AUDIO_FORMAT_S16;

	switch (conf->frequency) {
	case SBC_SAMPLING_FREQ_16000:
		this->sbc.frequency = SBC_FREQ_16000;
                info->info.raw.rate = 16000;
		break;
	case SBC_SAMPLING_FREQ_32000:
		this->sbc.frequency = SBC_FREQ_32000;
                info->info.raw.rate = 32000;
		break;
	case SBC_SAMPLING_FREQ_44100:
		this->sbc.frequency = SBC_FREQ_44100;
                info->info.raw.rate = 44100;
		break;
	case SBC_SAMPLING_FREQ_48000:
		this->sbc.frequency = SBC_FREQ_48000;
                info->info.raw.rate = 48000;
		break;
	default:
		res = -EINVAL;
                goto error;
        }

	switch (conf->channel_mode) {
	case SBC_CHANNEL_MODE_MONO:
		this->sbc.mode = SBC_MODE_MONO;
                info->info.raw.channels = 1;
                break;
	case SBC_CHANNEL_MODE_DUAL_CHANNEL:
		this->sbc.mode = SBC_MODE_DUAL_CHANNEL;
                info->info.raw.channels = 2;
		break;
	case SBC_CHANNEL_MODE_STEREO:
		this->sbc.mode = SBC_MODE_STEREO;
                info->info.raw.channels = 2;
		break;
	case SBC_CHANNEL_MODE_JOINT_STEREO:
		this->sbc.mode = SBC_MODE_JOINT_STEREO;
                info->info.raw.channels = 2;
                break;
	default:
		res = -EINVAL;
                goto error;
        }

	switch (info->info.raw.channels) {
	case 1:
		info->info.raw.position[0] = SPA_AUDIO_CHANNEL_MONO;
		break;
	case 2:
		info->info.raw.position[0] = SPA_AUDIO_CHANNEL_FL;
		info->info.raw.position[1] = SPA_AUDIO_CHANNEL_FR;
		break;
	}

	switch (conf->subbands) {
	case SBC_SUBBANDS_4:
		this->sbc.subbands = SBC_SB_4;
		break;
	case SBC_SUBBANDS_8:
		this->sbc.subbands = SBC_SB_8;
		break;
	default:
		res = -EINVAL;
		goto error;
	}

	if (conf->allocation_method & SBC_ALLOCATION_LOUDNESS)
		this->sbc.allocation = SBC_AM_LOUDNESS;
	else
		this->sbc.allocation = SBC_AM_SNR;

	switch (conf->block_length) {
	case SBC_BLOCK_LENGTH_4:
		this->sbc.blocks = SBC_BLK_4;
		break;
	case SBC_BLOCK_LENGTH_8:
		this->sbc.blocks = SBC_BLK_8;
		break;
	case SBC_BLOCK_LENGTH_12:
		this->sbc.blocks = SBC_BLK_12;
		break;
	case SBC_BLOCK_LENGTH_16:
		this->sbc.blocks = SBC_BLK_16;
		break;
	default:
		res = -EINVAL;
		goto error;
	}

	this->min_bitpool = SPA_MAX(conf->min_bitpool, 12);
	this->max_bitpool = conf->max_bitpool;

	codec_set_bitpool(this, conf->max_bitpool);

	return this;
error:
	errno = -res;
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;
	sbc_finish(&this->sbc);
	free(this);
}

static int codec_start_encode (void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp)
{
	struct impl *this = data;

	this->header = (struct rtp_header *)dst;
	this->payload = SPA_MEMBER(dst, sizeof(struct rtp_header), struct rtp_payload);
	memset(this->header, 0, sizeof(struct rtp_header)+sizeof(struct rtp_payload));

	this->payload->frame_count = 0;
	this->header->v = 2;
	this->header->pt = 1;
	this->header->sequence_number = htons(seqnum);
	this->header->timestamp = htonl(timestamp);
	this->header->ssrc = htonl(1);
	return sizeof(struct rtp_header) + sizeof(struct rtp_payload);
}

static int codec_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct impl *this = data;
	int res;

	res = sbc_encode(&this->sbc, src, src_size,
			dst, dst_size, (ssize_t*)dst_out);

	if (res >= this->codesize)
		this->payload->frame_count += res / this->codesize;

	return res;
}

static int codec_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct impl *this = data;
	int res;

	res = sbc_decode(&this->sbc, src, src_size,
			dst, dst_size, dst_out);

	return res;
}

struct a2dp_codec a2dp_codec_sbc = {
	.id = {.codec_id = A2DP_CODEC_SBC},
	.name = "sbc",
	.description = "SBC",
	.fill_caps = codec_fill_caps,
	.select_config = codec_select_config,
	.init = codec_init,
	.deinit = codec_deinit,
	.get_block_size = codec_get_block_size,
	.get_num_blocks = codec_get_num_blocks,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.decode = codec_decode,
	.reduce_bitpool = codec_reduce_bitpool,
	.increase_bitpool = codec_increase_bitpool,
};
