/* Spa A2DP Opus Codec */
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

#include <opus.h>

#include "rtp.h"
#include "media-codecs.h"

static struct spa_log *log;

struct dec_data {
	int32_t delay;
};

struct enc_data {
	struct rtp_header *header;
	struct rtp_payload *payload;

	int samples;
	int codesize;
	int frame_dms;
	int bitrate;
	int packet_size;

	int32_t delay;
};

struct impl {
	OpusEncoder *enc;
	OpusDecoder *dec;

	int mtu;
	int samplerate;
	int channels;
	int application;

	struct dec_data d;
	struct enc_data e;
};

static int codec_fill_caps(const struct media_codec *codec, uint32_t flags,
		const struct spa_dict *settings, uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	a2dp_opus_g_t conf = {
		.info = codec->vendor,
	};

	OPUS_G_SET(conf,
			OPUS_G_FREQUENCY_48000,
			OPUS_G_DURATION_100 | OPUS_G_DURATION_200,
			OPUS_G_CHANNELS_MONO | OPUS_G_CHANNELS_STEREO | OPUS_G_CHANNELS_MONO_2);

	memcpy(caps, &conf, sizeof(conf));
	return sizeof(conf);
}

static int codec_select_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size,
		const struct media_codec_audio_info *info,
		const struct spa_dict *global_settings, uint8_t config[A2DP_MAX_CAPS_SIZE],
		void **config_data)
{
	a2dp_opus_g_t conf;
	int frequency, duration, channels;

	if (caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (codec->vendor.vendor_id != conf.info.vendor_id ||
	    codec->vendor.codec_id != conf.info.codec_id)
		return -ENOTSUP;

	if (OPUS_G_GET_FREQUENCY(conf) & OPUS_G_FREQUENCY_48000)
		frequency = OPUS_G_FREQUENCY_48000;
	else
		return -EINVAL;

	if (OPUS_G_GET_DURATION(conf) & OPUS_G_DURATION_200)
		duration = OPUS_G_DURATION_200;
	else if (OPUS_G_GET_DURATION(conf) & OPUS_G_DURATION_100)
		duration = OPUS_G_DURATION_100;
	else
		return -EINVAL;

	if (OPUS_G_GET_CHANNELS(conf) & OPUS_G_CHANNELS_STEREO)
		channels = OPUS_G_CHANNELS_STEREO;
	else if (OPUS_G_GET_CHANNELS(conf) & OPUS_G_CHANNELS_MONO)
		channels = OPUS_G_CHANNELS_MONO;
	else if (OPUS_G_GET_CHANNELS(conf) & OPUS_G_CHANNELS_MONO_2)
		channels = OPUS_G_CHANNELS_MONO_2;
	else
		return -EINVAL;

	OPUS_G_SET(conf, frequency, duration, channels);

	memcpy(config, &conf, sizeof(conf));

	return sizeof(conf);
}

static int codec_caps_preference_cmp(const struct media_codec *codec, uint32_t flags, const void *caps1, size_t caps1_size,
		const void *caps2, size_t caps2_size, const struct media_codec_audio_info *info,
		const struct spa_dict *global_settings)
{
	a2dp_opus_g_t conf1, conf2, cap1, cap2;
	a2dp_opus_g_t *conf;
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
	a = (res1 > 0 && (size_t)res1 == sizeof(a2dp_opus_g_t)) ? 1 : 0;
	b = (res2 > 0 && (size_t)res2 == sizeof(a2dp_opus_g_t)) ? 1 : 0;
	if (!a || !b)
		return b - a;

	memcpy(&cap1, caps1, sizeof(cap1));
	memcpy(&cap2, caps2, sizeof(cap2));

	PREFER_EXPR(OPUS_G_GET_CHANNELS(*conf) & OPUS_G_CHANNELS_STEREO);

	return 0;

#undef PREFER_EXPR
#undef PREFER_BOOL
}

static int codec_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	a2dp_opus_g_t conf;
	struct spa_pod_frame f[1];
	uint32_t position[2];
	int channels;

	if (caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (idx > 0)
		return 0;

	switch (OPUS_G_GET_CHANNELS(conf)) {
	case OPUS_G_CHANNELS_STEREO:
		channels = 2;
		position[0] = SPA_AUDIO_CHANNEL_FL;
		position[1] = SPA_AUDIO_CHANNEL_FR;
		break;
	case OPUS_G_CHANNELS_MONO:
		channels = 1;
		position[0] = SPA_AUDIO_CHANNEL_MONO;
		break;
	case OPUS_G_CHANNELS_MONO_2:
		channels = 2;
		position[0] = SPA_AUDIO_CHANNEL_AUX0;
		position[1] = SPA_AUDIO_CHANNEL_AUX1;
		break;
	default:
		return -EINVAL;
	}

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_F32),
			SPA_FORMAT_AUDIO_rate,     SPA_POD_CHOICE_ENUM_Int(6,
					48000, 48000, 24000, 16000, 12000, 8000),
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
	a2dp_opus_g_t conf;

	if (caps == NULL || caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	spa_zero(*info);
	info->media_type = SPA_MEDIA_TYPE_audio;
	info->media_subtype = SPA_MEDIA_SUBTYPE_raw;

	info->info.raw.format = SPA_AUDIO_FORMAT_F32;
	info->info.raw.rate = 0;  /* not specified by config */

	switch (OPUS_G_GET_FREQUENCY(conf)) {
	case OPUS_G_FREQUENCY_48000:
		break;
	default:
		return -EINVAL;
	}

	switch (OPUS_G_GET_DURATION(conf)) {
	case OPUS_G_DURATION_100:
	case OPUS_G_DURATION_200:
		break;
	default:
		return -EINVAL;
	}

	switch (OPUS_G_GET_CHANNELS(conf)) {
	case OPUS_G_CHANNELS_STEREO:
		info->info.raw.channels = 2;
		info->info.raw.position[0] = SPA_AUDIO_CHANNEL_FL;
		info->info.raw.position[1] = SPA_AUDIO_CHANNEL_FR;
		break;
	case OPUS_G_CHANNELS_MONO:
		info->info.raw.channels = 1;
		info->info.raw.position[0] = SPA_AUDIO_CHANNEL_MONO;
		break;
	case OPUS_G_CHANNELS_MONO_2:
		info->info.raw.channels = 2;
		info->info.raw.position[0] = SPA_AUDIO_CHANNEL_AUX0;
		info->info.raw.position[1] = SPA_AUDIO_CHANNEL_AUX1;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int parse_frame_dms(int value)
{
	switch (value) {
	case OPUS_G_DURATION_100:
		return 100;
	case OPUS_G_DURATION_200:
		return 200;
	default:
		return -EINVAL;
	}
}

static void *codec_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	a2dp_opus_g_t conf;
	struct impl *this = NULL;
	struct spa_audio_info config_info;
	int res;

	if (config_len < sizeof(conf)) {
		res = -EINVAL;
		goto error;
	}
	memcpy(&conf, config, sizeof(conf));

	if (info->media_type != SPA_MEDIA_TYPE_audio ||
	    info->media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    info->info.raw.format != SPA_AUDIO_FORMAT_F32) {
		res = -EINVAL;
		goto error;
	}

	if ((this = calloc(1, sizeof(struct impl))) == NULL)
		goto error_errno;

	if ((res = codec_validate_config(codec, flags, config, config_len, &config_info)) < 0)
		goto error;
	if (config_info.info.raw.channels != info->info.raw.channels) {
		res = -EINVAL;
		goto error;
	}

	this->mtu = mtu;
	this->samplerate = info->info.raw.rate;
	this->channels = config_info.info.raw.channels;
	this->application = OPUS_APPLICATION_AUDIO;

	/*
	 * Setup encoder
	 */
	this->enc = opus_encoder_create(this->samplerate, this->channels, this->application, &res);
	if (this->enc == NULL) {
		res = -EINVAL;
		goto error;
	}

	if ((this->e.frame_dms = parse_frame_dms(OPUS_G_GET_DURATION(conf))) < 0) {
		res = -EINVAL;
		goto error;
	}

	this->e.samples = this->e.frame_dms * this->samplerate / 10000;
	this->e.codesize = this->e.samples * (int)this->channels * sizeof(float);

	int header_size = sizeof(struct rtp_header) + sizeof(struct rtp_payload);
	this->e.bitrate = SPA_MIN(128000 * this->channels,
			(int64_t)8 * (this->mtu - header_size) * 10000 / this->e.frame_dms);

	opus_encoder_ctl(this->enc, OPUS_SET_BITRATE(this->e.bitrate));

	opus_encoder_ctl(this->enc, OPUS_GET_LOOKAHEAD(&this->e.delay));

	/*
	 * Setup decoder
	 */
	this->dec = opus_decoder_create(this->samplerate, this->channels, &res);
	if (this->dec == NULL) {
		res = -EINVAL;
		goto error;
	}

	opus_decoder_ctl(this->dec, OPUS_GET_LOOKAHEAD(&this->d.delay));

	return this;

error_errno:
	res = -errno;
	goto error;

error:
	if (this && this->enc)
		opus_encoder_destroy(this->enc);
	if (this && this->dec)
		opus_decoder_destroy(this->dec);
	free(this);
	errno = -res;
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;

	opus_encoder_destroy(this->enc);
	opus_decoder_destroy(this->dec);
	free(this);
}

static int codec_get_block_size(void *data)
{
	struct impl *this = data;
	return this->e.codesize;
}

static int codec_start_encode (void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp)
{
	struct impl *this = data;
	size_t header_size = sizeof(struct rtp_header) + sizeof(struct rtp_payload);

	if (dst_size <= header_size)
		return -EINVAL;

	this->e.header = (struct rtp_header *)dst;
	this->e.payload = SPA_PTROFF(dst, sizeof(struct rtp_header), struct rtp_payload);
	memset(dst, 0, header_size);

	this->e.payload->frame_count = 0;
	this->e.header->v = 2;
	this->e.header->pt = 96;
	this->e.header->sequence_number = htons(seqnum);
	this->e.header->timestamp = htonl(timestamp);
	this->e.header->ssrc = htonl(1);

	this->e.packet_size = header_size;
	return this->e.packet_size;
}

static int codec_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush)
{
	struct impl *this = data;
	int res;

	if (src_size < (size_t)this->e.codesize) {
		*dst_out = 0;
		return 0;
	}
	if (this->e.packet_size >= this->mtu)
		return -EINVAL;

	dst_size = SPA_MIN(dst_size, (size_t)(this->mtu - this->e.packet_size));

	res = opus_encode_float(this->enc, src, this->e.samples, dst, dst_size);
	if (res < 0)
		return -EINVAL;

	*dst_out = res;

	this->e.packet_size += res;
	this->e.payload->frame_count++;

	*need_flush = NEED_FLUSH_ALL;

	return this->e.codesize;
}

static int codec_start_decode (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	struct impl SPA_UNUSED *this = data;
	const struct rtp_header *header = src;
	const struct rtp_payload *payload = SPA_PTROFF(src, sizeof(struct rtp_header), void);
	size_t header_size = sizeof(struct rtp_header) + sizeof(struct rtp_payload);

	if (src_size <= header_size)
		return -EINVAL;

	if (seqnum)
		*seqnum = ntohs(header->sequence_number);
	if (timestamp)
		*timestamp = ntohl(header->timestamp);

	if (payload->is_fragmented)
		return -EINVAL;  /* fragmentation not supported */
	if (payload->frame_count != 1)
		return -EINVAL;  /* wrong number of frames in packet */

	return header_size;
}

static int codec_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct impl SPA_UNUSED *this = data;
	int consumed = src_size;
	int res;
	int dst_samples;

	dst_samples = dst_size / (sizeof(float) * this->channels);
	res = opus_decode_float(this->dec, src, src_size, dst, dst_samples, 0);
	if (res < 0)
		return -EINVAL;

	*dst_out = (size_t)res * this->channels * sizeof(float);

	return consumed;
}

static int codec_abr_process (void *data, size_t unsent)
{
	return -ENOTSUP;
}

static int codec_reduce_bitpool(void *data)
{
	return 0;
}

static int codec_increase_bitpool(void *data)
{
	return 0;
}

static void codec_get_delay(void *data, uint32_t *encoder, uint32_t *decoder)
{
	struct impl *this = data;

	if (encoder)
		*encoder = this->e.delay;
	if (decoder)
		*decoder = this->d.delay;
}

static void codec_set_log(struct spa_log *global_log)
{
	log = global_log;
	spa_log_topic_init(log, &codec_plugin_log_topic);
}

const struct media_codec a2dp_codec_opus_g = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_OPUS_G,
	.kind = MEDIA_CODEC_A2DP,
	.codec_id = A2DP_CODEC_VENDOR,
	.vendor = { .vendor_id = OPUS_G_VENDOR_ID,
			.codec_id = OPUS_G_CODEC_ID },
	.select_config = codec_select_config,
	.enum_config = codec_enum_config,
	.validate_config = codec_validate_config,
	.caps_preference_cmp = codec_caps_preference_cmp,
	.init = codec_init,
	.deinit = codec_deinit,
	.get_block_size = codec_get_block_size,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.abr_process = codec_abr_process,
	.reduce_bitpool = codec_reduce_bitpool,
	.increase_bitpool = codec_increase_bitpool,
	.set_log = codec_set_log,
	.start_decode = codec_start_decode,
	.decode = codec_decode,
	.name = "opus_g",
	.description = "Opus",
	.fill_caps = codec_fill_caps,
	.get_delay = codec_get_delay,
};

MEDIA_CODEC_EXPORT_DEF(
	"opus-g",
	&a2dp_codec_opus_g
);
