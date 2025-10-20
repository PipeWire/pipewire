/* Spa HFP LC3-SWB Codec */
/* SPDX-FileCopyrightText: Copyright © 2025 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <arpa/inet.h>

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

#define LC3_SWB_BLOCK_SIZE		960

static struct spa_log *log;

struct impl {
	lc3_encoder_t enc;
	lc3_decoder_t dec;
	struct h2_reader h2;
	uint16_t seq;

	void *data;
	size_t avail;
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
			SPA_FORMAT_AUDIO_rate,     SPA_POD_CHOICE_ENUM_Int(1, 32000),
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
	info->info.raw.rate = 32000;
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

	this->enc = lc3_setup_encoder(7500, 32000, 0,
			calloc(1, lc3_encoder_size(7500, 32000)));
	if (!this->enc)
		goto fail;

	this->dec = lc3_setup_decoder(7500, 32000, 0,
			calloc(1, lc3_decoder_size(7500, 32000)));
	if (!this->dec)
		goto fail;

	spa_assert(lc3_frame_samples(7500, 32000) * sizeof(float) == LC3_SWB_BLOCK_SIZE);

	h2_reader_init(&this->h2, false);

	return spa_steal_ptr(this);

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
	return LC3_SWB_BLOCK_SIZE;
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
	int res;

	if (src_size < LC3_SWB_BLOCK_SIZE)
		return -EINVAL;
	if (dst_size < H2_PACKET_SIZE)
		return -EINVAL;

	h2_write(dst, this->seq);

	res = lc3_encode(this->enc, LC3_PCM_FORMAT_FLOAT, src, 1,
			H2_PACKET_SIZE - 2, SPA_PTROFF(dst, 2, void));
	if (res != 0)
		return -EINVAL;

	*dst_out = H2_PACKET_SIZE;
	*need_flush = NEED_FLUSH_ALL;
	return LC3_SWB_BLOCK_SIZE;
}

static int codec_start_decode (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	struct impl *this = data;
	size_t consumed = 0;

	if (is_zero_packet(src, src_size))
		return -EINVAL;

	if (!this->data)
		this->data = h2_reader_read(&this->h2, src, src_size, &consumed, &this->avail);

	if (seqnum)
		*seqnum = this->h2.seq;
	if (timestamp)
		*timestamp = 0;
	return consumed;
}

static int codec_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct impl *this = data;
	size_t consumed = 0;
	int res;

	*dst_out = 0;
	if (dst_size < LC3_SWB_BLOCK_SIZE)
		return -EINVAL;

	if (!this->data)
		this->data = h2_reader_read(&this->h2, src, src_size, &consumed, &this->avail);
	if (!this->data)
		return consumed;

	res = lc3_decode(this->dec, this->data, this->avail, LC3_PCM_FORMAT_FLOAT, dst, 1);
	this->data = NULL;

	if (res) {
		/* fail decoding silently, so remainder of packet is processed */
		spa_log_debug(log, "decoding failed: %d", res);
		return consumed;
	}

	*dst_out = LC3_SWB_BLOCK_SIZE;
	return consumed;
}

static int codec_produce_plc(void *data, void *dst, size_t dst_size)
{
	struct impl *this = data;
	int res;

	if (dst_size < LC3_SWB_BLOCK_SIZE)
		return -EINVAL;

	res = lc3_decode(this->dec, NULL, 0, LC3_PCM_FORMAT_FLOAT, dst, 1);
	if (res != 1)
		return -EINVAL;

	return LC3_SWB_BLOCK_SIZE;
}

static void codec_set_log(struct spa_log *global_log)
{
	log = global_log;
	spa_log_topic_init(log, &codec_plugin_log_topic);
}

const struct media_codec hfp_codec_msbc = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_LC3_SWB,
	.kind = MEDIA_CODEC_HFP,
	.codec_id = 0x03,
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
	.name = "lc3_swb",
	.description = "LC3-SWB",
	.stream_pkt = true,
};

MEDIA_CODEC_EXPORT_DEF(
	"hfp-lc3-swb",
	&hfp_codec_msbc
);
