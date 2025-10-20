/* Spa HFP LC3-24kHz (Apple) Codec */
/* SPDX-FileCopyrightText: Copyright © 2025 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>

#include <spa/debug/types.h>
#include <spa/param/audio/type-info.h>
#include <spa/param/audio/raw.h>
#include <spa/utils/string.h>
#include <spa/utils/dict.h>
#include <spa/utils/endian.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/cleanup.h>

#include "media-codecs.h"
#include "hfp-h2.h"

#include <lc3.h>

#define LC3_A127_BLOCK_SIZE		720

static struct spa_log *log;

struct impl {
	lc3_encoder_t enc;
	lc3_decoder_t dec;

	int prev_hwseq;
	uint16_t seq;
};

static int codec_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	struct spa_pod_frame f[1];
	const uint32_t position[1] = { SPA_AUDIO_CHANNEL_MONO };
	const int channels = 1;

	spa_assert(caps == NULL && caps_size == 0);

	if (idx > 0)
		return 0;

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_F32),
			SPA_FORMAT_AUDIO_rate,     SPA_POD_CHOICE_ENUM_Int(1, 24000),
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int(channels),
			SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
					SPA_TYPE_Id, channels, position),
			0);

	*param = spa_pod_builder_pop(b, &f[0]);
	return *param == NULL ? -EIO : 1;
}

static int codec_validate_config(const struct media_codec *codec, uint32_t flags,
			const void *caps, size_t caps_size,
			struct spa_audio_info *info)
{
	spa_assert(caps == NULL && caps_size == 0);

	spa_zero(*info);
	info->media_type = SPA_MEDIA_TYPE_audio;
	info->media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info->info.raw.format = SPA_AUDIO_FORMAT_F32;
	info->info.raw.rate = 24000;
	info->info.raw.channels = 1;
	info->info.raw.position[0] = SPA_AUDIO_CHANNEL_MONO;
	return 0;
}

static void *codec_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	struct impl *this = NULL;

	spa_assert(config == NULL && config_len == 0 && props == NULL);

	this = calloc(1, sizeof(*this));
	if (!this)
		goto fail;

	this->enc = lc3_setup_encoder(7500, 24000, 0,
			calloc(1, lc3_encoder_size(7500, 24000)));
	if (!this->enc)
		goto fail;

	this->dec = lc3_setup_decoder(7500, 24000, 0,
			calloc(1, lc3_decoder_size(7500, 24000)));
	if (!this->dec)
		goto fail;

	spa_assert(lc3_frame_samples(7500, 24000) * sizeof(float) == LC3_A127_BLOCK_SIZE);

	this->prev_hwseq = -1;
	return this;

fail:
	if (this) {
		free(this->enc);
		free(this->dec);
		free(this);
	}
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;

	free(this->enc);
	free(this->dec);
	free(this);
}

static int codec_get_block_size(void *data)
{
	return LC3_A127_BLOCK_SIZE;
}

static int codec_start_encode (void *data, void *dst, size_t dst_size,
		uint16_t seqnum, uint32_t timestamp)
{
	struct impl *this = data;

	this->seq = seqnum;
	return 0;
}

static int codec_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush)
{
	struct impl *this = data;
	uint8_t *buf = dst;
	int res;

	if (src_size < LC3_A127_BLOCK_SIZE)
		return -EINVAL;
	if (dst_size < H2_PACKET_SIZE)
		return -EINVAL;

	buf[0] = this->seq;
	buf[1] = H2_PACKET_SIZE - 2;
	this->seq++;

	res = lc3_encode(this->enc, LC3_PCM_FORMAT_FLOAT, src, 1,
			H2_PACKET_SIZE - 2, SPA_PTROFF(buf, 2, void));
	if (res != 0)
		return -EINVAL;

	*dst_out = H2_PACKET_SIZE;
	*need_flush = NEED_FLUSH_ALL;
	return LC3_A127_BLOCK_SIZE;
}

static int codec_start_decode (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	struct impl *this = data;
	const uint8_t *buf = src;

	if (is_zero_packet(src, src_size))
		return -EINVAL;
	if (src_size < 2)
		return -EINVAL;
	if (buf[1] != H2_PACKET_SIZE - 2)
		return -EINVAL;

	if (this->prev_hwseq >= 0)
		this->seq += (uint8_t)(buf[0] - this->prev_hwseq);
	this->prev_hwseq = buf[0];

	if (seqnum)
		*seqnum = this->seq;
	if (timestamp)
		*timestamp = 0;

	return 2;
}

static int codec_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct impl *this = data;
	int res;

	*dst_out = 0;

	if (src_size < H2_PACKET_SIZE - 2)
		return -EINVAL;
	if (dst_size < LC3_A127_BLOCK_SIZE)
		return -EINVAL;

	res = lc3_decode(this->dec, src, H2_PACKET_SIZE - 2, LC3_PCM_FORMAT_FLOAT, dst, 1);
	if (res)
		return -EINVAL;

	*dst_out = LC3_A127_BLOCK_SIZE;
	return H2_PACKET_SIZE - 2;
}

static int codec_produce_plc(void *data, void *dst, size_t dst_size)
{
	struct impl *this = data;
	int res;

	if (dst_size < LC3_A127_BLOCK_SIZE)
		return -EINVAL;

	res = lc3_decode(this->dec, NULL, 0, LC3_PCM_FORMAT_FLOAT, dst, 1);
	if (res != 1)
		return -EINVAL;

	return LC3_A127_BLOCK_SIZE;
}

static void codec_set_log(struct spa_log *global_log)
{
	log = global_log;
	spa_log_topic_init(log, &codec_plugin_log_topic);
}

static const struct media_codec hfp_codec_a127 = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_LC3_A127,
	.kind = MEDIA_CODEC_HFP,
	.codec_id = 127,
	.enum_config = codec_enum_config,
	.validate_config = codec_validate_config,
	.init = codec_init,
	.deinit = codec_deinit,
	.get_block_size = codec_get_block_size,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.set_log = codec_set_log,
	.start_decode = codec_start_decode,
	.decode = codec_decode,
	.produce_plc = codec_produce_plc,
	.name = "lc3_a127",
	.description = "LC3-24kHz",
};

MEDIA_CODEC_EXPORT_DEF(
	"hfp-lc3-a127",
	&hfp_codec_a127
);
