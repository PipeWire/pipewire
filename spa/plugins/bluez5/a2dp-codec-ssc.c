/* Spa A2DP Scalable Codec */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2022 Pauli Virtanen */
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

#include <ssc_encoder.h>
#include <ssc_decoder.h>

#include "rtp.h"
#include "media-codecs.h"

// The extra rates supported by encoder (886000, 442000, 308000 and 250000) are not supported by device, these are the valid ones.
static const int bitrate_uhq2[] = {584000, 291000, 152000, 0};
// Newer firmware also supports 229000, but older does not, not sure how to differentiate. (It's unused by vendor)
// 96000 was unused by vendor, but it should work on every device.
static const int bitrate_normal[] = {192000, 128000, 96000, 88000, 0};

#define NORMAL_BITRATES (sizeof(bitrate_normal)/sizeof(bitrate_normal[0]))
#define UHQ2_BITRATES (sizeof(bitrate_uhq2)/sizeof(bitrate_uhq2[0]))
#define MAX_BITRATES (NORMAL_BITRATES>UHQ2_BITRATES?NORMAL_BITRATES:UHQ2_BITRATES)

static struct spa_log *ssc_log;


struct enc_data {
	struct rtp_header *header;

	/* zero-terminated list of valid bitrates for this MTU, device and sample rate combination */
	int bitrates[MAX_BITRATES];
	int bitrate_idx;
	int packet_size;
};

struct impl {
	ssc_encoder *enc;
	ssc_decoder *dec;

	int mtu;
	int samplerate;

	struct enc_data e;
};

static int codec_fill_caps(const struct media_codec *codec, uint32_t flags,
		const struct spa_dict *settings, uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	a2dp_ssc_t conf = {
		.info = codec->vendor,
		.data = SSC_FLAG_SUPPORTS_192KBPS
			| SSC_FLAG_SUPPORTS_44_1KHZ
			| SSC_FLAG_SUPPORTS_48KHZ
			| SSC_FLAG_SUPPORTS_48KHZ_24BIT,
	};

	memcpy(caps, &conf, sizeof(conf));
	return sizeof(conf);
}

static int codec_select_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size,
		const struct media_codec_audio_info *info,
		const struct spa_dict *global_settings, uint8_t config[A2DP_MAX_CAPS_SIZE],
		void **config_data)
{
	a2dp_ssc_t conf;
	char new = 0;

	if (caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (codec->vendor.vendor_id != conf.info.vendor_id ||
	    codec->vendor.codec_id != conf.info.codec_id)
		return -ENOTSUP;

	if (conf.data & SSC_FLAG_SUPPORTS_96KHZ_UHQ2 &&
		!(conf.data & SSC_FLAG_DISABLE_96KHZ))
		new = SSC_FLAG_SUPPORTS_96KHZ_UHQ2;
	else if (conf.data & SSC_FLAG_SUPPORTS_48KHZ_24BIT)
		new = SSC_FLAG_SUPPORTS_48KHZ_24BIT;
	else if (conf.data & SSC_FLAG_SUPPORTS_44_1KHZ)
		new = SSC_FLAG_SUPPORTS_44_1KHZ;
	else if (conf.data & SSC_FLAG_SUPPORTS_48KHZ)
		new = SSC_FLAG_SUPPORTS_48KHZ;
	else
		return -EINVAL;

	if (conf.data & SSC_FLAG_SUPPORTS_192KBPS)
		new |= SSC_FLAG_SUPPORTS_192KBPS;

	conf.data = new;
	memcpy(config, &conf, sizeof(conf));

	return sizeof(conf);
}

static int codec_caps_preference_cmp(const struct media_codec *codec, uint32_t flags, const void *caps1, size_t caps1_size,
		const void *caps2, size_t caps2_size, const struct media_codec_audio_info *info,
		const struct spa_dict *global_settings)
{
	a2dp_ssc_t conf1, conf2;
	a2dp_ssc_t *conf;
	int res1, res2;
	int a, b;

	/* Order selected configurations by preference */
	res1 = codec->select_config(codec, flags, caps1, caps1_size, info, global_settings, (uint8_t *)&conf1, NULL);
	res2 = codec->select_config(codec, flags, caps2, caps2_size, info, global_settings, (uint8_t *)&conf2, NULL);

#define PREFER_EXPR(expr)			\
		do {				\
			conf = &conf1; 		\
			a = (expr);		\
			conf = &conf2;		\
			b = (expr);		\
			if (a != b)		\
				return b - a;	\
		} while (0)

#define PREFER_BOOL(expr)	PREFER_EXPR((expr) ? 1 : 0)

	/* Prefer valid */
	a = (res1 > 0 && (size_t)res1 == sizeof(a2dp_ssc_t)) ? 1 : 0;
	b = (res2 > 0 && (size_t)res2 == sizeof(a2dp_ssc_t)) ? 1 : 0;
	if (!a || !b)
		return b - a;

	PREFER_EXPR(conf->data & SSC_FLAG_SUPPORTS_96KHZ_UHQ2 &&
		!(conf->data & SSC_FLAG_DISABLE_96KHZ));
	PREFER_EXPR(conf->data & SSC_FLAG_SUPPORTS_48KHZ_24BIT);
	PREFER_EXPR(conf->data & SSC_FLAG_SUPPORTS_192KBPS);
	PREFER_EXPR(conf->data & SSC_FLAG_SUPPORTS_44_1KHZ);

	return 0;

#undef PREFER_EXPR
#undef PREFER_BOOL
}

static int codec_validate_config(const struct media_codec *codec, uint32_t flags,
			const void *caps, size_t caps_size,
			struct spa_audio_info *info)
{
	a2dp_ssc_t conf;

	if (caps == NULL || caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	spa_zero(*info);
	info->media_type = SPA_MEDIA_TYPE_audio;
	info->media_subtype = SPA_MEDIA_SUBTYPE_raw;

	info->info.raw.channels = 2;
	info->info.raw.position[0] = SPA_AUDIO_CHANNEL_FL;
	info->info.raw.position[1] = SPA_AUDIO_CHANNEL_FR;
	info->info.raw.format = SPA_AUDIO_FORMAT_F32;
	if (conf.data & SSC_FLAG_SUPPORTS_96KHZ_UHQ2 &&
		!(conf.data & SSC_FLAG_DISABLE_96KHZ)) {
		info->info.raw.rate = 96000;
	} else if (conf.data & SSC_FLAG_SUPPORTS_48KHZ_24BIT) {
		info->info.raw.rate = 48000;
	} else if (conf.data & SSC_FLAG_SUPPORTS_44_1KHZ) {
		info->info.raw.rate = 44100;
	} else if (conf.data & SSC_FLAG_SUPPORTS_48KHZ) {
		info->info.raw.rate = 48000;
	} else
		return -EINVAL;

	return 0;
}

static int codec_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	struct spa_audio_info info;
	struct spa_pod_frame f[1];
	int res;

	if ((res = codec_validate_config(codec, flags, caps, caps_size, &info)) < 0)
		return res;

	if (idx > 0)
		return 0;

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,      SPA_POD_Id(info.media_type),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(info.media_subtype),
			SPA_FORMAT_AUDIO_format,   SPA_POD_Id(info.info.raw.format),
			SPA_FORMAT_AUDIO_rate,     SPA_POD_Int(info.info.raw.rate),
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int(info.info.raw.channels),
			SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
					SPA_TYPE_Id, info.info.raw.channels, info.info.raw.position),
			0);

	*param = spa_pod_builder_pop(b, &f[0]);
	return *param == NULL ? -EIO : 1;
}

static void *codec_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	a2dp_ssc_t conf;
	struct impl *this = NULL;
	struct spa_audio_info config_info;
	int res, i = 0, j = 0;
	const int* bitrates_in;

	if (config_len < sizeof(conf)) {
		res = -EINVAL;
		goto error;
	}
	memcpy(&conf, config, sizeof(conf));

	if (info->media_type != SPA_MEDIA_TYPE_audio ||
	    info->media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    info->info.raw.channels != 2 ||
	    info->info.raw.format != SPA_AUDIO_FORMAT_F32) {
		res = -EINVAL;
		goto error;
	}

	if ((this = calloc(1, sizeof(struct impl))) == NULL)
		goto error_errno;

	if ((res = codec_validate_config(codec, flags, config, config_len, &config_info)) < 0)
		goto error;

	this->mtu = mtu;
	this->samplerate = info->info.raw.rate;

	/*
	 * Setup encoder
	 */
	this->enc = ssc_encoder_create(this->samplerate);
	if (this->enc == NULL) {
		res = -EINVAL;
		goto error;
	}

	switch (this->samplerate) {
		case 96000:
			bitrates_in = bitrate_uhq2;
			break;
		case 48000:
		case 44100:
			bitrates_in = bitrate_normal;
			break;
		default:
			res = -EINVAL;
			goto error;
	}

	do {
		int res = ssc_encoder_frame_bytes(this->samplerate, bitrates_in[i]);
		if (res > 0 && (size_t)res <= this->mtu - sizeof(struct rtp_header) &&
		        (this->samplerate == 96000 || conf.data & SSC_FLAG_SUPPORTS_192KBPS
			        || bitrates_in[i] < 192000)) {
			this->e.bitrates[j++] = bitrates_in[i];
		}
	} while (bitrates_in[++i]);
	this->e.bitrates[j] = 0;
	this->e.bitrate_idx = 0;
	if (!j) {
		res = -EINVAL;
		goto error;
	}

	/*
	 * Setup decoder
	 */
	this->dec = ssc_decoder_create(this->samplerate);
	if (this->dec == NULL) {
		res = -EINVAL;
		goto error;
	}

	return this;

error_errno:
	res = -errno;
	goto error;

error:
	if (this && this->enc)
		ssc_encoder_destroy(this->enc);
	if (this && this->dec)
		ssc_decoder_destroy(this->dec);
	free(this);
	errno = -res;
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;

	ssc_encoder_destroy(this->enc);
	ssc_decoder_destroy(this->dec);
	free(this);
}

static int codec_get_block_size(void *data)
{
	struct impl SPA_UNUSED *this = data;
	return 864 * 2 * sizeof(float);
}

static int codec_start_encode (void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp)
{
	struct impl *this = data;

	if (dst_size <= sizeof(struct rtp_header))
		return -EINVAL;

	this->e.header = (struct rtp_header *)dst;
	memset(dst, 0, sizeof(struct rtp_header));

	this->e.header->v = 2;
	this->e.header->pt = 96;
	this->e.header->sequence_number = htons(seqnum);
	this->e.header->timestamp = htonl(timestamp);
	this->e.header->ssrc = htonl(1);

	this->e.packet_size = sizeof(struct rtp_header);
	return sizeof(struct rtp_header);
}

static int codec_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush)
{
	struct impl *this = data;
	int res, bitrate;

	if (src_size < (size_t)codec_get_block_size(data)) {
		*dst_out = 0;
		return 0;
	}
	if (this->e.packet_size >= this->mtu)
		return -EINVAL;

	dst_size = SPA_MIN(dst_size, (size_t)(this->mtu - this->e.packet_size));

	bitrate = this->e.bitrates[this->e.bitrate_idx];

	res = ssc_encode_float(this->enc, src, bitrate, 0, dst, dst_size);
	if (res < 0)
		return -EINVAL;

	*dst_out = res;

	this->e.packet_size += res;

	*need_flush = NEED_FLUSH_ALL;

	return codec_get_block_size(data);
}

static int codec_start_decode (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	struct impl SPA_UNUSED *this = data;
	const struct rtp_header *header = src;

	if (src_size <= sizeof(struct rtp_header))
		return -EINVAL;

	if (seqnum)
		*seqnum = ntohs(header->sequence_number);
	if (timestamp)
		*timestamp = ntohl(header->timestamp);

	return sizeof(struct rtp_header);
}

static int codec_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct impl *this = data;
	int res, consumed;

	if (dst_size < codec_get_block_size(data))
		return -EINVAL;
	consumed = ssc_frame_bytes(src, src_size, this->samplerate);
	if (!consumed || consumed > src_size)
		return -EINVAL;
	res = ssc_decode_float(this->dec, src, consumed, dst);
	if (res < 0)
		return -EINVAL;

	*dst_out = (size_t)res * 2 * sizeof(float);
	return consumed;
}

static int codec_reduce_bitpool(void *data)
{
	struct impl *this = data;
	if (!this->e.bitrates[this->e.bitrate_idx + 1]) {
		return 0; // This is the lowest bitrate we have
	}
	this->e.bitrate_idx++;
	return this->e.bitrates[this->e.bitrate_idx - 1] - this->e.bitrates[this->e.bitrate_idx];
}

static int codec_increase_bitpool(void *data)
{
	struct impl *this = data;
	if (!this->e.bitrate_idx) {
		return 0; // This is the highest bitrate we have
	}
	this->e.bitrate_idx--;
	return this->e.bitrates[this->e.bitrate_idx] - this->e.bitrates[this->e.bitrate_idx + 1];
}

static void codec_get_delay(void *data, uint32_t *encoder, uint32_t *decoder)
{
	struct impl SPA_UNUSED *this = data;

	if (encoder)
		*encoder = 108;
	if (decoder)
		*decoder = 0;
}

static void codec_set_log(struct spa_log *global_log)
{
	ssc_log = global_log;
	spa_log_topic_init(ssc_log, &codec_plugin_log_topic);
}

const struct media_codec a2dp_codec_ssc = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_SSC,
	.kind = MEDIA_CODEC_A2DP,
	.codec_id = A2DP_CODEC_VENDOR,
	.vendor = { .vendor_id = SSC_VENDOR_ID,
			.codec_id = SSC_CODEC_ID },
	.select_config = codec_select_config,
	.enum_config = codec_enum_config,
	.validate_config = codec_validate_config,
	.caps_preference_cmp = codec_caps_preference_cmp,
	.init = codec_init,
	.deinit = codec_deinit,
	.get_block_size = codec_get_block_size,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.reduce_bitpool = codec_reduce_bitpool,
	.increase_bitpool = codec_increase_bitpool,
	.set_log = codec_set_log,
	.start_decode = codec_start_decode,
	.decode = codec_decode,
	.name = "ssc",
	.description = "Scalable / Seamless",
	.fill_caps = codec_fill_caps,
	.get_delay = codec_get_delay,
};

MEDIA_CODEC_EXPORT_DEF(
	"ssc",
	&a2dp_codec_ssc
);
