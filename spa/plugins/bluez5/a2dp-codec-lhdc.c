/* Spa A2DP LHDC v5 codec */
/* SPDX-License-Identifier: MIT */

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <spa/param/audio/format.h>
#include <spa/utils/string.h>

#include <lhdcv5BT.h>

#include "rtp.h"
#include "media-codecs.h"

#define LHDCV5_SAMPLE_RATE 48000
#define LHDCV5_CHANNELS 2
#define LHDCV5_BITS_PER_SAMPLE LHDCBT_SMPL_FMT_S16
#define LHDCV5_INTERVAL_MS LHDC_ENC_INTERVAL_10MS
#define LHDCV5_MAX_PACKET_BYTES 504u

static struct spa_log *log_;

struct lhdc_media_payload {
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t frame_count:6;
	uint8_t latency:2;
#else
	uint8_t latency:2;
	uint8_t frame_count:6;
#endif
	uint8_t seq_number;
} __attribute__ ((packed));

struct impl {
	HANDLE_LHDC_BT lhdc;

	struct rtp_header *rtp;
	struct lhdc_media_payload *payload;
	uint8_t media_seqnum;

	uint32_t block_samples;
	uint32_t block_bytes;
};

static int codec_fill_caps(const struct media_codec *codec, uint32_t flags,
		const struct spa_dict *settings, uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	const a2dp_lhdc_v5_t conf = {
		.info = codec->vendor,
		.sampling_freq = LHDC_V5_SAMPLING_FREQ_48000,
		.bitrate_and_depth = LHDC_V5_MIN_BITRATE_64K |
			LHDC_V5_MAX_BITRATE_400K | LHDC_V5_BIT_FMT_16,
		.frame_len_and_version = LHDC_V5_FRAME_LEN_5MS | LHDC_V5_VERSION_1,
		.features = LHDC_V5_FEATURE_LL,
		.reserved = 0,
	};
	memcpy(caps, &conf, sizeof(conf));
	return sizeof(conf);
}

static int codec_select_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size,
		const struct media_codec_audio_info *info,
		const struct spa_dict *settings, uint8_t config[A2DP_MAX_CAPS_SIZE],
		void **config_data)
{
	a2dp_lhdc_v5_t conf;

	if (caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (codec->vendor.vendor_id != conf.info.vendor_id ||
	    codec->vendor.codec_id != conf.info.codec_id)
		return -ENOTSUP;

	if ((conf.sampling_freq & LHDC_V5_SAMPLING_FREQ_48000) == 0)
		return -ENOTSUP;
	if ((conf.bitrate_and_depth & LHDC_V5_BIT_FMT_16) == 0)
		return -ENOTSUP;
	if ((conf.frame_len_and_version & LHDC_V5_FRAME_LEN_5MS) == 0 ||
	    (conf.frame_len_and_version & LHDC_V5_VERSION_1) == 0)
		return -ENOTSUP;

	conf.sampling_freq = LHDC_V5_SAMPLING_FREQ_48000;
	conf.bitrate_and_depth = LHDC_V5_MIN_BITRATE_64K |
		LHDC_V5_MAX_BITRATE_400K | LHDC_V5_BIT_FMT_16;
	conf.frame_len_and_version = LHDC_V5_FRAME_LEN_5MS | LHDC_V5_VERSION_1;
	conf.features &= LHDC_V5_FEATURE_LL;
	conf.reserved = 0;

	memcpy(config, &conf, sizeof(conf));
	return sizeof(conf);
}

static int codec_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	struct spa_pod_frame f[1];
	uint32_t position[2] = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR };

	if (idx > 0)
		return 0;
	if (caps_size < sizeof(a2dp_lhdc_v5_t))
		return -EINVAL;

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(b,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_AUDIO_format, SPA_POD_Id(SPA_AUDIO_FORMAT_S16),
			SPA_FORMAT_AUDIO_rate, SPA_POD_Int(LHDCV5_SAMPLE_RATE),
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int(LHDCV5_CHANNELS),
			SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
				SPA_TYPE_Id, 2, position),
			0);
	*param = spa_pod_builder_pop(b, &f[0]);
	return *param == NULL ? -EIO : 1;
}

static int codec_validate_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, struct spa_audio_info *info)
{
	uint8_t config[A2DP_MAX_CAPS_SIZE];
	struct media_codec_audio_info audio_info = {
		.rate = info->info.raw.rate,
		.channels = info->info.raw.channels,
	};

	if (codec_select_config(codec, flags, caps, caps_size,
				&audio_info, NULL, config, NULL) < 0)
		return -EINVAL;

	info->media_type = SPA_MEDIA_TYPE_audio;
	info->media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info->info.raw.format = SPA_AUDIO_FORMAT_S16;
	info->info.raw.rate = LHDCV5_SAMPLE_RATE;
	info->info.raw.channels = LHDCV5_CHANNELS;
	info->info.raw.position[0] = SPA_AUDIO_CHANNEL_FL;
	info->info.raw.position[1] = SPA_AUDIO_CHANNEL_FR;
	return 0;
}

static void *codec_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	struct impl *this;
	int32_t res;

	if (info->info.raw.format != SPA_AUDIO_FORMAT_S16 ||
	    info->info.raw.rate != LHDCV5_SAMPLE_RATE ||
	    info->info.raw.channels != LHDCV5_CHANNELS) {
		errno = EINVAL;
		return NULL;
	}

	this = calloc(1, sizeof(struct impl));
	if (this == NULL)
		return NULL;

	res = lhdcv5BT_get_handle(LHDC_VERSION_1, &this->lhdc);
	if (res != LHDC_FRET_SUCCESS || this->lhdc == NULL)
		goto error;

	res = lhdcv5BT_init_encoder(this->lhdc, LHDCV5_SAMPLE_RATE,
			LHDCV5_BITS_PER_SAMPLE, LHDC_QUALITY_AUTO, mtu,
			LHDCV5_INTERVAL_MS, 0);
	if (res != LHDC_FRET_SUCCESS)
		goto error;

	lhdcv5BT_set_min_bitrate(this->lhdc, LHDC_QUALITY_LOW0);
	lhdcv5BT_set_max_bitrate(this->lhdc, LHDC_QUALITY_HIGH1);

	res = lhdcv5BT_get_block_Size(this->lhdc, &this->block_samples);
	if (res != LHDC_FRET_SUCCESS || this->block_samples == 0)
		goto error;

	this->block_bytes = this->block_samples * LHDCV5_CHANNELS * sizeof(int16_t);
	return this;

error:
	if (this->lhdc)
		lhdcv5BT_free_handle(this->lhdc);
	free(this);
	errno = EIO;
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;
	if (this->lhdc)
		lhdcv5BT_free_handle(this->lhdc);
	free(this);
}

static int codec_get_block_size(void *data)
{
	struct impl *this = data;
	return this->block_bytes;
}

static int codec_abr_process(void *data, size_t unsent)
{
	struct impl *this = data;
	uint32_t queue_len = (unsent + LHDCV5_MAX_PACKET_BYTES - 1) / LHDCV5_MAX_PACKET_BYTES;
	int32_t res = lhdcv5BT_adjust_bitrate(this->lhdc, queue_len);

	return res == LHDC_FRET_SUCCESS ? 0 : -EIO;
}

static int codec_start_encode(void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp)
{
	struct impl *this = data;
	size_t header_size = sizeof(struct rtp_header) + sizeof(struct lhdc_media_payload);

	if (dst_size <= header_size)
		return -EINVAL;

	this->rtp = dst;
	this->payload = SPA_PTROFF(dst, sizeof(struct rtp_header), struct lhdc_media_payload);
	memset(dst, 0, header_size);

	this->rtp->v = 2;
	this->rtp->pt = 96;
	this->rtp->sequence_number = htons(seqnum);
	this->rtp->timestamp = htonl(timestamp);
	this->rtp->ssrc = htonl(1);

	this->payload->latency = 0;
	this->payload->frame_count = 0;
	this->payload->seq_number = this->media_seqnum;

	return header_size;
}

static int codec_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush)
{
	struct impl *this = data;
	uint32_t written = 0;
	uint32_t frames = 0;
	int32_t res;

	*dst_out = 0;
	*need_flush = NEED_FLUSH_NO;

	if (src == NULL)
		return 0;
	if (src_size < this->block_bytes)
		return 0;
	if (dst_size > UINT32_MAX)
		dst_size = UINT32_MAX;

	res = lhdcv5BT_encode(this->lhdc, (void *)src, this->block_bytes,
			dst, (uint32_t)dst_size, &written, &frames);
	if (res != LHDC_FRET_SUCCESS)
		return -EIO;

	if (written == 0)
		return this->block_bytes;

	this->payload->latency = 0;
	this->payload->frame_count = frames;
	this->payload->seq_number = this->media_seqnum++;

	*dst_out = written;
	*need_flush = NEED_FLUSH_ALL;
	return this->block_bytes;
}

static void codec_get_delay(void *data, uint32_t *encoder, uint32_t *decoder)
{
	struct impl *this = data;
	if (encoder)
		*encoder = this->block_samples * 2;
	if (decoder)
		*decoder = 0;
}

static void codec_set_log(struct spa_log *global_log)
{
	log_ = global_log;
	spa_log_topic_init(log_, &codec_plugin_log_topic);
}

const struct media_codec a2dp_codec_lhdc_v5 = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_LHDC_V5,
	.kind = MEDIA_CODEC_A2DP,
	.codec_id = A2DP_CODEC_VENDOR,
	.vendor = { .vendor_id = LHDC_V5_VENDOR_ID,
		.codec_id = LHDC_V5_CODEC_ID },
	.name = "lhdc_v5",
	.description = "LHDC v5",
	.send_buf_size = 16 * 1024,
	.fill_caps = codec_fill_caps,
	.select_config = codec_select_config,
	.enum_config = codec_enum_config,
	.validate_config = codec_validate_config,
	.init = codec_init,
	.deinit = codec_deinit,
	.get_block_size = codec_get_block_size,
	.abr_process = codec_abr_process,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.get_delay = codec_get_delay,
	.set_log = codec_set_log,
};

MEDIA_CODEC_EXPORT_DEF(
	"lhdc",
	&a2dp_codec_lhdc_v5
);
