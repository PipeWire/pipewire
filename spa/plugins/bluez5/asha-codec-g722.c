/* Spa ASHA G722 codec */
/* SPDX-FileCopyrightText: Copyright © 2024 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <spa/param/audio/format.h>
#include <spa/utils/dict.h>
#include <spa/debug/log.h>

#include "rtp.h"
#include "media-codecs.h"
#include "g722/g722_enc_dec.h"

#define ASHA_HEADER_SZ       1 /* 1 byte sequence number */
#define ASHA_ENCODED_PKT_SZ  160

static struct spa_log *spalog;

struct impl {
	g722_encode_state_t encode;
	unsigned int codesize;
};

static int codec_reduce_bitpool(void *data)
{
	return -ENOTSUP;
}

static int codec_increase_bitpool(void *data)
{
	return -ENOTSUP;
}

static int codec_abr_process (void *data, size_t unsent)
{
	return -ENOTSUP;
}

static int codec_get_block_size(void *data)
{
	struct impl *this = data;
	return this->codesize;
}

static int codec_start_encode (void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp)
{
	/* Payload for ASHA must be preceded by 1-byte sequence number */
	*(uint8_t *)dst = seqnum % 256;

	return 1;
}

static int codec_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	struct spa_pod_frame f[1];
	uint32_t position[1];

	if (idx > 0)
		return 0;

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_S16),
			0);
	spa_pod_builder_add(b,
		SPA_FORMAT_AUDIO_rate, SPA_POD_Int(16000),
		0);

	spa_pod_builder_add(b,
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int(1),
			0);
	position[0] = SPA_AUDIO_CHANNEL_MONO;
	spa_pod_builder_add(b,
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int(1),
			SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
				SPA_TYPE_Id, 1, position),
			0);

	*param = spa_pod_builder_pop(b, &f[0]);

	return *param == NULL ? -EIO : 1;
}

static void codec_deinit(void *data)
{
	return;
}

static void *codec_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	struct impl *this;

	if ((this = calloc(1, sizeof(struct impl))) == NULL)
		return NULL;

	g722_encode_init(&this->encode, 64000, G722_PACKED);

	/*
	 * G722 has a compression ratio of 4. Considering 160 bytes of encoded
	 * payload, we need 640 bytes for generating an encoded frame.
	 */
	this->codesize = ASHA_ENCODED_PKT_SZ * 4;

	spa_log_debug(spalog, "Codec initialized");

	return this;
}

static int codec_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush)
{
	struct impl *this = data;
	size_t src_sz;
	int ret;

	if (src_size < this->codesize) {
		spa_log_trace(spalog, "Insufficient bytes for encoding, %zd", src_size);
		return 0;
	}

	if (dst_size < (ASHA_HEADER_SZ + ASHA_ENCODED_PKT_SZ)) {
		spa_log_trace(spalog, "No space for encoded output, %zd", dst_size);
		return 0;
	}

	src_sz = (src_size > this->codesize) ? this->codesize : src_size;

	ret = g722_encode(&this->encode, dst, src, src_sz / 2 /* S16LE */);
	if (ret < 0) {
		spa_log_error(spalog, "encode error: %d", ret);
		return -EIO;
	}

	*dst_out = ret;
	*need_flush = NEED_FLUSH_ALL;

	return src_sz;
}

static void codec_set_log(struct spa_log *global_log)
{
	spalog = global_log;
	spa_log_topic_init(spalog, &codec_plugin_log_topic);
}

const struct media_codec asha_codec_g722 = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_G722,
	.codec_id = ASHA_CODEC_G722,
	.name = "g722",
	.asha = true,
	.description = "G722",
	.fill_caps = NULL,
	.enum_config = codec_enum_config,
	.init = codec_init,
	.deinit = codec_deinit,
	.get_block_size = codec_get_block_size,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.abr_process = codec_abr_process,
	.reduce_bitpool = codec_reduce_bitpool,
	.increase_bitpool = codec_increase_bitpool,
	.set_log = codec_set_log,
};

MEDIA_CODEC_EXPORT_DEF(
	"g722",
	&asha_codec_g722
);
