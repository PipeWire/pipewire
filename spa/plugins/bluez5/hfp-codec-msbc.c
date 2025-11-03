/* Spa HFP MSBC Codec */
/* SPDX-FileCopyrightText: Copyright © 2019 Collabora Ltd. */
/* SPDX-FileCopyrightText: Copyright © 2025 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#include "config.h"

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
#include <spa/utils/cleanup.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/cleanup.h>

#include <sbc/sbc.h>

#include "media-codecs.h"
#include "hfp-h2.h"
#include "plc.h"


#define MSBC_BLOCK_SIZE		240

static struct spa_log *log_;

struct impl {
	sbc_t msbc;
	struct h2_reader h2;
	uint16_t seq;

	void *data;
	size_t avail;

	plc_state_t *plc;
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
			SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_S16),
			SPA_FORMAT_AUDIO_rate,     SPA_POD_Int(16000),
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
	info->info.raw.rate = 16000;
	info->info.raw.channels = 1;
	info->info.raw.position[0] = SPA_AUDIO_CHANNEL_MONO;
	return 0;
}

static void *codec_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	spa_autofree struct impl *this = NULL;
	int res;

	spa_assert(config == NULL && config_len == 0 && props == NULL);

	this = calloc(1, sizeof(*this));
	if (!this)
		return NULL;

	res = sbc_init_msbc(&this->msbc, 0);
	if (res < 0)
		return NULL;

	/* Libsbc expects audio samples by default in host endianness, mSBC requires little endian */
	this->msbc.endian = SBC_LE;

	h2_reader_init(&this->h2, true);

	this->plc = plc_init(NULL);
	if (!this->plc)
		return NULL;

	return spa_steal_ptr(this);
}

static void codec_deinit(void *data)
{
	struct impl *this = data;

	sbc_finish(&this->msbc);
	plc_free(this->plc);
	free(this);
}

static int codec_get_block_size(void *data)
{
	return MSBC_BLOCK_SIZE;
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
	ssize_t written = 0;
	int res;

	if (src_size < MSBC_BLOCK_SIZE)
		return -EINVAL;
	if (dst_size < H2_PACKET_SIZE)
		return -EINVAL;

	h2_write(dst, this->seq);

	res = sbc_encode(&this->msbc, src, src_size, SPA_PTROFF(dst, 2, void), H2_PACKET_SIZE - 3, &written);
	if (res < 0)
		return -EINVAL;

	*dst_out = H2_PACKET_SIZE;
	*need_flush = NEED_FLUSH_ALL;
	return res;
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

	if (!this->data)
		this->data = h2_reader_read(&this->h2, src, src_size, &consumed, &this->avail);
	if (!this->data)
		return consumed;

	res = sbc_decode(&this->msbc, this->data, this->avail, dst, dst_size, dst_out);
	this->data = NULL;

	if (res < 0) {
		/* fail decoding silently, so remainder of packet is processed */
		spa_log_debug(log_, "decoding failed: %d", res);
		return consumed;
	}

	plc_rx(this->plc, dst, *dst_out / sizeof(int16_t));

	return consumed;
}

static int codec_produce_plc(void *data, void *dst, size_t dst_size)
{
	struct impl *this = data;
	int res;

	if (dst_size < MSBC_BLOCK_SIZE)
		return -EINVAL;

	res = plc_fillin(this->plc, dst, MSBC_BLOCK_SIZE / sizeof(int16_t));
	if (res < 0)
		return res;

	return MSBC_BLOCK_SIZE;
}

static void codec_set_log(struct spa_log *global_log)
{
	log_ = global_log;
	spa_log_topic_init(log_, &codec_plugin_log_topic);
}

const struct media_codec hfp_codec_msbc = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_MSBC,
	.kind = MEDIA_CODEC_HFP,
	.codec_id = 0x02,
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
	.name = "msbc",
	.description = "MSBC",
	.stream_pkt = true,
};

MEDIA_CODEC_EXPORT_DEF(
	"hfp-msbc",
	&hfp_codec_msbc
);
