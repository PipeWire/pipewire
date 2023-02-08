/* Spa A2DP LC3plus HR codec */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2022 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <stddef.h>
#include <errno.h>
#include <arpa/inet.h>
#if __BYTE_ORDER != __LITTLE_ENDIAN
#include <byteswap.h>
#endif

#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>

#ifdef HAVE_LC3PLUS_H
#include <lc3plus.h>
#else
#include <lc3.h>
#endif

#include "rtp.h"
#include "media-codecs.h"

#define BITRATE_MIN 96000
#define BITRATE_MAX 512000
#define BITRATE_DEFAULT 160000

struct dec_data {
	int frame_size;
	int fragment_size;
	int fragment_count;
	uint8_t fragment[LC3PLUS_MAX_BYTES];
};

struct enc_data {
	struct rtp_header *header;
	struct rtp_payload *payload;

	int samples;
	int codesize;

	int packet_size;
	int fragment_size;
	int fragment_count;
	void *fragment;

	int bitrate;
	int next_bitrate;
};

struct impl {
	LC3PLUS_Enc *enc;
	LC3PLUS_Dec *dec;

	int mtu;
	int samplerate;
	int channels;
	int frame_dms;
	int bitrate;

	struct dec_data d;
	struct enc_data e;

	int32_t buf[2][LC3PLUS_MAX_SAMPLES];
};

static int codec_fill_caps(const struct media_codec *codec, uint32_t flags,
		uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	const a2dp_lc3plus_hr_t a2dp_lc3plus_hr = {
		.info = codec->vendor,
		LC3PLUS_HR_INIT_FRAME_DURATION(LC3PLUS_HR_FRAME_DURATION_10MS
				| LC3PLUS_HR_FRAME_DURATION_5MS
				| LC3PLUS_HR_FRAME_DURATION_2_5MS)
		.channels = LC3PLUS_HR_CHANNELS_1 | LC3PLUS_HR_CHANNELS_2,
		LC3PLUS_HR_INIT_FREQUENCY(LC3PLUS_HR_SAMPLING_FREQ_48000
				| (lc3plus_samplerate_supported(96000) ? LC3PLUS_HR_SAMPLING_FREQ_96000 : 0))
	};
	memcpy(caps, &a2dp_lc3plus_hr, sizeof(a2dp_lc3plus_hr));
	return sizeof(a2dp_lc3plus_hr);
}

static int codec_select_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size,
		const struct media_codec_audio_info *info,
		const struct spa_dict *settings, uint8_t config[A2DP_MAX_CAPS_SIZE])
{
	a2dp_lc3plus_hr_t conf;

	if (caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (codec->vendor.vendor_id != conf.info.vendor_id ||
	    codec->vendor.codec_id != conf.info.codec_id)
		return -ENOTSUP;

	if ((LC3PLUS_HR_GET_FREQUENCY(conf) & LC3PLUS_HR_SAMPLING_FREQ_48000)
			&& lc3plus_samplerate_supported(48000))
		LC3PLUS_HR_SET_FREQUENCY(conf, LC3PLUS_HR_SAMPLING_FREQ_48000);
	else if ((LC3PLUS_HR_GET_FREQUENCY(conf) & LC3PLUS_HR_SAMPLING_FREQ_96000)
			&& lc3plus_samplerate_supported(96000))
		LC3PLUS_HR_SET_FREQUENCY(conf, LC3PLUS_HR_SAMPLING_FREQ_96000);
	else
		return -ENOTSUP;

	if ((conf.channels & LC3PLUS_HR_CHANNELS_2) &&
			lc3plus_channels_supported(2))
		conf.channels = LC3PLUS_HR_CHANNELS_2;
	else if ((conf.channels & LC3PLUS_HR_CHANNELS_1) &&
			lc3plus_channels_supported(1))
		conf.channels = LC3PLUS_HR_CHANNELS_1;
	else
		return -ENOTSUP;

	if (LC3PLUS_HR_GET_FRAME_DURATION(conf) & LC3PLUS_HR_FRAME_DURATION_10MS)
		LC3PLUS_HR_SET_FRAME_DURATION(conf, LC3PLUS_HR_FRAME_DURATION_10MS);
	else if (LC3PLUS_HR_GET_FRAME_DURATION(conf) & LC3PLUS_HR_FRAME_DURATION_5MS)
		LC3PLUS_HR_SET_FRAME_DURATION(conf, LC3PLUS_HR_FRAME_DURATION_5MS);
	else if (LC3PLUS_HR_GET_FRAME_DURATION(conf) & LC3PLUS_HR_FRAME_DURATION_2_5MS)
		LC3PLUS_HR_SET_FRAME_DURATION(conf, LC3PLUS_HR_FRAME_DURATION_2_5MS);
	else
		return -ENOTSUP;

	memcpy(config, &conf, sizeof(conf));

	return sizeof(conf);
}

static int codec_caps_preference_cmp(const struct media_codec *codec, uint32_t flags, const void *caps1, size_t caps1_size,
		const void *caps2, size_t caps2_size, const struct media_codec_audio_info *info, const struct spa_dict *global_settings)
{
	a2dp_lc3plus_hr_t conf1, conf2;
	a2dp_lc3plus_hr_t *conf;
	int res1, res2;
	int a, b;

	/* Order selected configurations by preference */
	res1 = codec->select_config(codec, 0, caps1, caps1_size, info, NULL, (uint8_t *)&conf1);
	res2 = codec->select_config(codec, 0, caps2, caps2_size, info , NULL, (uint8_t *)&conf2);

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
	a = (res1 > 0 && (size_t)res1 == sizeof(a2dp_lc3plus_hr_t)) ? 1 : 0;
	b = (res2 > 0 && (size_t)res2 == sizeof(a2dp_lc3plus_hr_t)) ? 1 : 0;
	if (!a || !b)
		return b - a;

	PREFER_BOOL(conf->channels & LC3PLUS_HR_CHANNELS_2);
	PREFER_BOOL(LC3PLUS_HR_GET_FREQUENCY(*conf) & (LC3PLUS_HR_SAMPLING_FREQ_48000 | LC3PLUS_HR_SAMPLING_FREQ_96000));
	PREFER_BOOL(LC3PLUS_HR_GET_FREQUENCY(*conf) & LC3PLUS_HR_SAMPLING_FREQ_48000);

	return 0;

#undef PREFER_EXPR
#undef PREFER_BOOL
}

static int codec_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	a2dp_lc3plus_hr_t conf;
	struct spa_pod_frame f[2];
	struct spa_pod_choice *choice;
	uint32_t position[SPA_AUDIO_MAX_CHANNELS];
	uint32_t i = 0;

	if (caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (idx > 0)
		return 0;

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_S24_32),
			0);
	spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_rate, 0);

	spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_None, 0);
	choice = (struct spa_pod_choice*)spa_pod_builder_frame(b, &f[1]);
	i = 0;
	if ((LC3PLUS_HR_GET_FREQUENCY(conf) & LC3PLUS_HR_SAMPLING_FREQ_96000) &&
			lc3plus_samplerate_supported(96000)) {
		if (i++ == 0)
			spa_pod_builder_int(b, 96000);
		spa_pod_builder_int(b, 96000);
	}
	if ((LC3PLUS_HR_GET_FREQUENCY(conf) & LC3PLUS_HR_SAMPLING_FREQ_48000) &&
			lc3plus_samplerate_supported(48000)) {
		if (i++ == 0)
			spa_pod_builder_int(b, 48000);
		spa_pod_builder_int(b, 48000);
	}
	if (i == 0)
		return -EINVAL;
	if (i > 1)
		choice->body.type = SPA_CHOICE_Enum;
	spa_pod_builder_pop(b, &f[1]);

	if ((conf.channels & (LC3PLUS_HR_CHANNELS_2 | LC3PLUS_HR_CHANNELS_1)) &&
			lc3plus_channels_supported(2) && lc3plus_channels_supported(1)) {
		spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_channels, SPA_POD_CHOICE_RANGE_Int(2, 1, 2),
				0);
	} else if ((conf.channels & LC3PLUS_HR_CHANNELS_2) && lc3plus_channels_supported(2)) {
		position[0] = SPA_AUDIO_CHANNEL_FL;
		position[1] = SPA_AUDIO_CHANNEL_FR;
		spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_channels, SPA_POD_Int(2),
				SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
					SPA_TYPE_Id, 2, position),
				0);
	} else if ((conf.channels & LC3PLUS_HR_CHANNELS_1) && lc3plus_channels_supported(1)) {
		position[0] = SPA_AUDIO_CHANNEL_MONO;
		spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_channels, SPA_POD_Int(1),
				SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
					SPA_TYPE_Id, 1, position),
				0);
	}

	*param = spa_pod_builder_pop(b, &f[0]);
	return *param == NULL ? -EIO : 1;
}

static int codec_validate_config(const struct media_codec *codec, uint32_t flags,
			const void *caps, size_t caps_size,
			struct spa_audio_info *info)
{
	const a2dp_lc3plus_hr_t *conf;

	if (caps == NULL || caps_size < sizeof(*conf))
		return -EINVAL;

	conf = caps;

	spa_zero(*info);
	info->media_type = SPA_MEDIA_TYPE_audio;
	info->media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info->info.raw.format = SPA_AUDIO_FORMAT_S24_32;

	switch (LC3PLUS_HR_GET_FREQUENCY(*conf)) {
	case LC3PLUS_HR_SAMPLING_FREQ_96000:
		if (!lc3plus_samplerate_supported(96000))
			return -EINVAL;
		info->info.raw.rate = 96000;
		break;
	case LC3PLUS_HR_SAMPLING_FREQ_48000:
		if (!lc3plus_samplerate_supported(48000))
			return -EINVAL;
		info->info.raw.rate = 48000;
		break;
	default:
		return -EINVAL;
        }

	switch (conf->channels) {
	case LC3PLUS_HR_CHANNELS_2:
		if (!lc3plus_channels_supported(2))
			return -EINVAL;
		info->info.raw.channels = 2;
		info->info.raw.position[0] = SPA_AUDIO_CHANNEL_FL;
		info->info.raw.position[1] = SPA_AUDIO_CHANNEL_FR;
		break;
	case LC3PLUS_HR_CHANNELS_1:
		if (!lc3plus_channels_supported(1))
			return -EINVAL;
		info->info.raw.channels = 1;
		info->info.raw.position[0] = SPA_AUDIO_CHANNEL_MONO;
                break;
	default:
		return -EINVAL;
        }

	switch (LC3PLUS_HR_GET_FRAME_DURATION(*conf)) {
	case LC3PLUS_HR_FRAME_DURATION_10MS:
	case LC3PLUS_HR_FRAME_DURATION_5MS:
	case LC3PLUS_HR_FRAME_DURATION_2_5MS:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static size_t ceildiv(size_t v, size_t divisor)
{
	if (v % divisor == 0)
		return v / divisor;
	else
		return v / divisor + 1;
}

static bool check_mtu_vs_frame_dms(struct impl *this)
{
	/* Only 10ms frames can be fragmented (max 0xf fragments);
	 * others must fit in single MTU */
	size_t header_size = sizeof(struct rtp_header) + sizeof(struct rtp_payload);
	size_t max_fragments = (this->frame_dms == 100) ? 0xf : 1;
	size_t payload_size = lc3plus_enc_get_num_bytes(this->enc);
	return (size_t)this->mtu >= header_size + ceildiv(payload_size, max_fragments);
}

static void *codec_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	a2dp_lc3plus_hr_t *conf = config;
	struct impl *this = NULL;
	struct spa_audio_info config_info;
	int size;
	int res;

	if (info->media_type != SPA_MEDIA_TYPE_audio ||
	    info->media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    info->info.raw.format != SPA_AUDIO_FORMAT_S24_32) {
		res = -EINVAL;
		goto error;
	}

	if ((this = calloc(1, sizeof(struct impl))) == NULL)
		goto error_errno;

	if ((res = codec_validate_config(codec, flags, config, config_len, &config_info)) < 0)
		goto error;

	this->mtu = mtu;
	this->samplerate = config_info.info.raw.rate;
	this->channels = config_info.info.raw.channels;
	this->bitrate = BITRATE_DEFAULT * this->channels;

	switch (LC3PLUS_HR_GET_FRAME_DURATION(*conf)) {
	case LC3PLUS_HR_FRAME_DURATION_10MS:
		this->frame_dms = 100;
		break;
	case LC3PLUS_HR_FRAME_DURATION_5MS:
		this->frame_dms = 50;
		break;
	case LC3PLUS_HR_FRAME_DURATION_2_5MS:
		this->frame_dms = 25;
		break;
	default:
		res = -EINVAL;
		goto error;
	}

	if ((size = lc3plus_enc_get_size(this->samplerate, this->channels)) == 0) {
		res = -EIO;
		goto error;
	}
	if ((this->enc = calloc(1, size)) == NULL)
		goto error_errno;
	if (lc3plus_enc_init(this->enc, this->samplerate, this->channels) != LC3PLUS_OK) {
		res = -EINVAL;
		goto error;
	}
	if (lc3plus_enc_set_frame_ms(this->enc, this->frame_dms/10.0f) != LC3PLUS_OK) {
		res = -EINVAL;
		goto error;
	}
	if (lc3plus_enc_set_hrmode(this->enc, 1) != LC3PLUS_OK) {
		res = -EINVAL;
		goto error;
	}
	while (1) {
		/* Find a valid bitrate */
		if (lc3plus_enc_set_bitrate(this->enc, this->bitrate) != LC3PLUS_OK) {
			res = -EINVAL;
			goto error;
		}
		if (check_mtu_vs_frame_dms(this))
			break;
		this->bitrate = this->bitrate * 3/4;
	}

	if ((size = lc3plus_dec_get_size(this->samplerate, this->channels)) == 0) {
		res = -EINVAL;
		goto error;
	}
	if ((this->dec = calloc(1, size)) == NULL)
		goto error_errno;
	if (lc3plus_dec_init(this->dec, this->samplerate, this->channels, LC3PLUS_PLC_ADVANCED) != LC3PLUS_OK) {
		res = -EINVAL;
		goto error;
	}
	if (lc3plus_dec_set_frame_ms(this->dec, this->frame_dms/10.0f) != LC3PLUS_OK) {
		res = -EINVAL;
		goto error;
	}
	if (lc3plus_dec_set_hrmode(this->dec, 1) != LC3PLUS_OK) {
		res = -EINVAL;
		goto error;
	}

	this->e.samples = lc3plus_enc_get_input_samples(this->enc);
	this->e.codesize = this->e.samples * this->channels * sizeof(int32_t);

	spa_assert(this->e.samples <= LC3PLUS_MAX_SAMPLES);

	this->e.bitrate = this->bitrate;
	this->e.next_bitrate = this->bitrate;

	return this;

error_errno:
	res = -errno;
	goto error;

error:
	if (this && this->enc)
		lc3plus_enc_free_memory(this->enc);
	if (this && this->dec)
		lc3plus_dec_free_memory(this->dec);
	free(this);
	errno = -res;
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;
	lc3plus_enc_free_memory(this->enc);
	lc3plus_dec_free_memory(this->dec);
	free(this);
}

static int codec_get_block_size(void *data)
{
	struct impl *this = data;
	return this->e.codesize;
}

static int codec_abr_process (void *data, size_t unsent)
{
	return -ENOTSUP;
}

static int codec_update_bitrate(struct impl *this)
{
	this->e.next_bitrate = SPA_CLAMP(this->e.next_bitrate,
			BITRATE_MIN * this->channels, BITRATE_MAX * this->channels);

	if (this->e.next_bitrate == this->e.bitrate)
		return 0;

	this->e.bitrate = this->e.next_bitrate;

	if (lc3plus_enc_set_bitrate(this->enc, this->e.bitrate) != LC3PLUS_OK ||
			!check_mtu_vs_frame_dms(this)) {
		lc3plus_enc_set_bitrate(this->enc, this->bitrate);
		return -EINVAL;
	}

	this->bitrate = this->e.bitrate;

	return 0;
}

static int codec_start_encode (void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp)
{
	struct impl *this = data;
	size_t header_size = sizeof(struct rtp_header) + sizeof(struct rtp_payload);

	if (dst_size <= header_size)
		return -EINVAL;

	codec_update_bitrate(this);

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

static void deinterleave_32_c2(int32_t * SPA_RESTRICT * SPA_RESTRICT dst, const int32_t * SPA_RESTRICT src, size_t n_samples)
{
	/* We'll trust the compiler to optimize this */
	const size_t n_channels = 2;
	size_t i, j;
	for (j = 0; j < n_samples; ++j)
		for (i = 0; i < n_channels; ++i)
			dst[i][j] = *src++;
}

static void interleave_32_c2(int32_t * SPA_RESTRICT dst, const int32_t * SPA_RESTRICT * SPA_RESTRICT src, size_t n_samples)
{
	const size_t n_channels = 2;
	size_t i, j;
	for (j = 0; j < n_samples; ++j)
		for (i = 0; i < n_channels; ++i)
			*dst++ = src[i][j];
}

static int codec_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush)
{
	struct impl *this = data;
	int frame_bytes;
	LC3PLUS_Error res;
	int size, processed;
	int header_size = sizeof(struct rtp_header) + sizeof(struct rtp_payload);
	int32_t *inputs[2];

	if (src == NULL) {
		/* Produce fragment packets.
		 *
		 * We assume the caller gives the same buffer here as in the previous
		 * calls to encode(), without changes in the buffer content.
		 */
		if (this->e.fragment == NULL ||
				this->e.fragment_count <= 1 ||
				this->e.fragment < dst ||
				SPA_PTROFF(this->e.fragment, this->e.fragment_size, void) > SPA_PTROFF(dst, dst_size, void)) {
			this->e.fragment = NULL;
			return -EINVAL;
		}

		size = SPA_MIN(this->mtu - header_size, this->e.fragment_size);
		memmove(dst, this->e.fragment, size);
		*dst_out = size;

		this->e.payload->is_fragmented = 1;
		this->e.payload->frame_count = --this->e.fragment_count;
		this->e.payload->is_last_fragment = (this->e.fragment_count == 1);

		if (this->e.fragment_size > size && this->e.fragment_count > 1) {
			this->e.fragment = SPA_PTROFF(this->e.fragment, size, void);
			this->e.fragment_size -= size;
			*need_flush = NEED_FLUSH_FRAGMENT;
		} else {
			this->e.fragment = NULL;
			*need_flush = NEED_FLUSH_ALL;
		}
		return 0;
	}

	frame_bytes = lc3plus_enc_get_num_bytes(this->enc);
	processed = 0;

	if (src_size < (size_t)this->e.codesize)
		goto done;
	if (dst_size < (size_t)frame_bytes)
		goto done;
	if (this->e.payload->frame_count > 0 &&
			this->e.packet_size + frame_bytes > this->mtu)
		goto done;

	if (this->channels == 1) {
		inputs[0] = (int32_t *)src;
		res = lc3plus_enc24(this->enc, inputs, dst, &size);
	} else {
		inputs[0] = this->buf[0];
		inputs[1] = this->buf[1];
		deinterleave_32_c2(inputs, src, this->e.samples);
		res = lc3plus_enc24(this->enc, inputs, dst, &size);
	}
	if (SPA_UNLIKELY(res != LC3PLUS_OK))
		return -EINVAL;
	*dst_out = size;

	processed += this->e.codesize;
	this->e.packet_size += size;
	this->e.payload->frame_count++;

done:
	if (this->e.payload->frame_count == 0)
		return processed;
	if (this->e.payload->frame_count < 0xf &&
			this->frame_dms * (this->e.payload->frame_count + 1) < 200 &&
			this->e.packet_size + frame_bytes <= this->mtu)
		return processed;  /* add another frame */

	if (this->e.packet_size > this->mtu) {
		/* Fragment packet */
		spa_assert(this->e.payload->frame_count == 1);
		spa_assert(this->frame_dms == 100);

		this->e.fragment_count = ceildiv(this->e.packet_size - header_size,
				this->mtu - header_size);

		this->e.payload->is_fragmented = 1;
		this->e.payload->is_first_fragment = 1;
		this->e.payload->frame_count = this->e.fragment_count;

		this->e.fragment_size = this->e.packet_size - this->mtu;
		this->e.fragment = SPA_PTROFF(dst, *dst_out - this->e.fragment_size, void);
		*need_flush = NEED_FLUSH_FRAGMENT;

		/*
		 * We keep the rest of the encoded frame in the same buffer, and rely
		 * that the caller won't overwrite it before the next call to encode()
		 */
		*dst_out = SPA_PTRDIFF(this->e.fragment, dst);
	} else {
		*need_flush = NEED_FLUSH_ALL;
	}

	return processed;
}

static SPA_UNUSED int codec_start_decode (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	struct impl *this = data;
	const struct rtp_header *header = src;
	const struct rtp_payload *payload = SPA_PTROFF(src, sizeof(struct rtp_header), void);
	size_t header_size = sizeof(struct rtp_header) + sizeof(struct rtp_payload);

	spa_return_val_if_fail (src_size > header_size, -EINVAL);

	if (seqnum)
		*seqnum = ntohs(header->sequence_number);
	if (timestamp)
		*timestamp = ntohl(header->timestamp);

	if (payload->is_fragmented) {
		if (payload->is_first_fragment) {
			this->d.fragment_size = 0;
		} else if (payload->frame_count + 1 != this->d.fragment_count ||
				(payload->frame_count == 1 && !payload->is_last_fragment)){
			/* Fragments not in right order: drop packet */
			return -EINVAL;
		}
		this->d.fragment_count = payload->frame_count;
		this->d.frame_size = src_size - header_size;
	} else {
		if (payload->frame_count <= 0)
			return -EINVAL;
		this->d.fragment_count = 0;
		this->d.frame_size = (src_size - header_size) / payload->frame_count;
		if (this->d.frame_size <= 0)
			return -EINVAL;
	}

	return header_size;
}

static SPA_UNUSED int codec_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct impl *this = data;
	LC3PLUS_Error res;
	int32_t *outputs[2];
	int consumed;
	int samples;

	if (this->d.fragment_count > 0) {
		/* Fragmented frame */
		size_t avail;
		avail = SPA_MIN(sizeof(this->d.fragment) - this->d.fragment_size, src_size);
		memcpy(SPA_PTROFF(this->d.fragment, this->d.fragment_size, void), src, avail);

		this->d.fragment_size += avail;
		consumed = src_size;

		if (this->d.fragment_count > 1) {
			/* More fragments to come */
			*dst_out = 0;
			return consumed;
		}

		src = this->d.fragment;
		src_size = this->d.fragment_size;

		this->d.fragment_count = 0;
		this->d.fragment_size = 0;
	} else {
		src_size = SPA_MIN((size_t)this->d.frame_size, src_size);
		consumed = src_size;
	}

	samples = lc3plus_dec_get_output_samples(this->dec);
	*dst_out = samples * this->channels * sizeof(int32_t);
	if (dst_size < *dst_out)
		return -EINVAL;

	if (this->channels == 1) {
		outputs[0] = (int32_t *)dst;
		res = lc3plus_dec24(this->dec, (void *)src, src_size, outputs, 0);
	} else {
		outputs[0] = this->buf[0];
		outputs[1] = this->buf[1];
		res = lc3plus_dec24(this->dec, (void *)src, src_size, outputs, 0);
		interleave_32_c2(dst, (const int32_t**)outputs, samples);
	}
	if (SPA_UNLIKELY(res != LC3PLUS_OK && res != LC3PLUS_DECODE_ERROR))
		return -EINVAL;

	return consumed;
}

static int codec_reduce_bitpool(void *data)
{
	struct impl *this = data;
	this->e.next_bitrate = SPA_CLAMP(this->bitrate * 3 / 4,
			BITRATE_MIN * this->channels, BITRATE_MAX * this->channels);
	return this->e.next_bitrate;
}

static int codec_increase_bitpool(void *data)
{
	struct impl *this = data;
	this->e.next_bitrate = SPA_CLAMP(this->bitrate * 5 / 4,
			BITRATE_MIN * this->channels, BITRATE_MAX * this->channels);
	return this->e.next_bitrate;
}

const struct media_codec a2dp_codec_lc3plus_hr = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_LC3PLUS_HR,
	.name = "lc3plus_hr",
	.codec_id = A2DP_CODEC_VENDOR,
	.vendor = { .vendor_id = LC3PLUS_HR_VENDOR_ID,
		.codec_id = LC3PLUS_HR_CODEC_ID },
	.description = "LC3plus HR",
	.fill_caps = codec_fill_caps,
	.select_config = codec_select_config,
	.enum_config = codec_enum_config,
	.validate_config = codec_validate_config,
	.caps_preference_cmp = codec_caps_preference_cmp,
	.init = codec_init,
	.deinit = codec_deinit,
	.get_block_size = codec_get_block_size,
	.abr_process = codec_abr_process,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.start_decode = codec_start_decode,
	.decode = codec_decode,
	.reduce_bitpool = codec_reduce_bitpool,
	.increase_bitpool = codec_increase_bitpool
};

MEDIA_CODEC_EXPORT_DEF(
	"lc3plus",
	&a2dp_codec_lc3plus_hr
);
