/* Spa HFP CVSD Codec */
/* SPDX-FileCopyrightText: Copyright © 2019 Collabora Ltd. */
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

static struct spa_log *log;

struct impl {
	size_t block_size;
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
			SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_S16_LE),
			SPA_FORMAT_AUDIO_rate,     SPA_POD_Int(8000),
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
	info->info.raw.format = SPA_AUDIO_FORMAT_S16_LE;
	info->info.raw.rate = 8000;
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

	if (mtu < 2)
		return NULL;

	this = calloc(1, sizeof(struct impl));
	if (!this)
		return NULL;

	this->block_size = SPA_MIN(2 * (mtu/2), 144u);	/* cap to 9 ms */
	return this;
}

static void codec_deinit(void *data)
{
	free(data);
}

static int codec_get_block_size(void *data)
{
	struct impl *this = data;

	return this->block_size;
}

static int codec_start_encode (void *data, void *dst, size_t dst_size,
		uint16_t seqnum, uint32_t timestamp)
{
	return 0;
}

static int codec_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush)
{
	struct impl *this = data;

	if (src_size < this->block_size)
		return -EINVAL;
	if (dst_size < this->block_size)
		return -EINVAL;

	spa_memmove(dst, src, this->block_size);
	*dst_out = this->block_size;
	*need_flush = NEED_FLUSH_ALL;
	return this->block_size;
}

static int codec_start_decode (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	struct impl *this = data;

	if (src_size != 48 && is_zero_packet(src, src_size)) {
		/* Adapter is returning non-standard CVSD stream. For example
		 * Intel 8087:0029 at Firmware revision 0.0 build 191 week 21 2021
		 * on kernel 5.13.19 produces such data.
		 */
		return -EINVAL;
	}

	if (src_size % 2 != 0) {
		/* Unaligned data: reception or adapter problem.
		 * Consider the whole packet lost and report.
		 */
		return -EINVAL;
	}

	if (seqnum)
		*seqnum = this->seq;

	if (timestamp)
		*timestamp = 0;

	this->seq++;
	return 0;
}

static int codec_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	uint32_t avail;

	avail = SPA_MIN(src_size, dst_size);
	if (avail)
		spa_memcpy(dst, src, avail);

	*dst_out = avail;
	return avail;
}

static void codec_set_log(struct spa_log *global_log)
{
	log = global_log;
	spa_log_topic_init(log, &codec_plugin_log_topic);
}

const struct media_codec hfp_codec_cvsd = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_CVSD,
	.kind = MEDIA_CODEC_HFP,
	.codec_id = 0x01,
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
	.name = "cvsd",
	.description = "CVSD",
};

MEDIA_CODEC_EXPORT_DEF(
	"hfp-cvsd",
	&hfp_codec_cvsd
);
