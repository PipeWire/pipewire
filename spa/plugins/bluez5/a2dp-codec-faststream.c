/* Spa A2DP FastStream codec */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2021 Pauli Virtanen */
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

#include <sbc/sbc.h>

#include "media-codecs.h"

struct impl {
	sbc_t sbc;

	size_t mtu;
	int codesize;
	int frame_count;
	int max_frames;
};

struct duplex_impl {
	sbc_t sbc;
};

static int codec_fill_caps(const struct media_codec *codec, uint32_t flags,
		uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	const a2dp_faststream_t a2dp_faststream = {
		.info = codec->vendor,
		.direction = FASTSTREAM_DIRECTION_SINK |
			(codec->duplex_codec ? FASTSTREAM_DIRECTION_SOURCE : 0),
		.sink_frequency =
			FASTSTREAM_SINK_SAMPLING_FREQ_44100 |
			FASTSTREAM_SINK_SAMPLING_FREQ_48000,
		.source_frequency =
			FASTSTREAM_SOURCE_SAMPLING_FREQ_16000,
	};

	memcpy(caps, &a2dp_faststream, sizeof(a2dp_faststream));
	return sizeof(a2dp_faststream);
}

static const struct media_codec_config
frequencies[] = {
	{ FASTSTREAM_SINK_SAMPLING_FREQ_48000, 48000, 1 },
	{ FASTSTREAM_SINK_SAMPLING_FREQ_44100, 44100, 0 },
};

static const struct media_codec_config
duplex_frequencies[] = {
	{ FASTSTREAM_SOURCE_SAMPLING_FREQ_16000, 16000, 0 },
};

static int codec_select_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size,
		const struct media_codec_audio_info *info,
		const struct spa_dict *settings, uint8_t config[A2DP_MAX_CAPS_SIZE])
{
	a2dp_faststream_t conf;
	int i;

	if (caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (codec->vendor.vendor_id != conf.info.vendor_id ||
	    codec->vendor.codec_id != conf.info.codec_id)
		return -ENOTSUP;

	if (codec->duplex_codec && !(conf.direction & FASTSTREAM_DIRECTION_SOURCE))
		return -ENOTSUP;

	if (!(conf.direction & FASTSTREAM_DIRECTION_SINK))
		return -ENOTSUP;

	conf.direction = FASTSTREAM_DIRECTION_SINK;

	if (codec->duplex_codec)
		conf.direction |= FASTSTREAM_DIRECTION_SOURCE;

	if ((i = media_codec_select_config(frequencies,
			SPA_N_ELEMENTS(frequencies),
			conf.sink_frequency,
			info ? info->rate : A2DP_CODEC_DEFAULT_RATE
			)) < 0)
		return -ENOTSUP;
	conf.sink_frequency = frequencies[i].config;

	if ((i = media_codec_select_config(duplex_frequencies,
			SPA_N_ELEMENTS(duplex_frequencies),
			conf.source_frequency,
			16000
			)) < 0)
		return -ENOTSUP;
	conf.source_frequency = duplex_frequencies[i].config;

	memcpy(config, &conf, sizeof(conf));

	return sizeof(conf);
}

static int codec_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	a2dp_faststream_t conf;
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
			SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_S16),
			0);
	spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_rate, 0);

	spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_None, 0);
	choice = (struct spa_pod_choice*)spa_pod_builder_frame(b, &f[1]);
	i = 0;
	if (conf.sink_frequency & FASTSTREAM_SINK_SAMPLING_FREQ_48000) {
		if (i++ == 0)
			spa_pod_builder_int(b, 48000);
		spa_pod_builder_int(b, 48000);
	}
	if (conf.sink_frequency & FASTSTREAM_SINK_SAMPLING_FREQ_44100) {
		if (i++ == 0)
			spa_pod_builder_int(b, 44100);
		spa_pod_builder_int(b, 44100);
	}
	if (i > 1)
		choice->body.type = SPA_CHOICE_Enum;
	spa_pod_builder_pop(b, &f[1]);
	if (i == 0)
		return -EINVAL;

	position[0] = SPA_AUDIO_CHANNEL_FL;
	position[1] = SPA_AUDIO_CHANNEL_FR;
	spa_pod_builder_add(b,
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int(2),
			SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
					SPA_TYPE_Id, 2, position),
			0);

	*param = spa_pod_builder_pop(b, &f[0]);
	return *param == NULL ? -EIO : 1;
}

static int codec_reduce_bitpool(void *data)
{
	return -ENOTSUP;
}

static int codec_increase_bitpool(void *data)
{
	return -ENOTSUP;
}

static int codec_get_block_size(void *data)
{
	struct impl *this = data;
	return this->codesize;
}

static size_t ceil2(size_t v)
{
	if (v % 2 != 0 && v < SIZE_MAX)
		v += 1;
	return v;
}

static void *codec_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	a2dp_faststream_t *conf = config;
	struct impl *this;
	bool sbc_initialized = false;
	int res;

	if ((this = calloc(1, sizeof(struct impl))) == NULL)
		goto error_errno;

	if ((res = sbc_init(&this->sbc, 0)) < 0)
		goto error;

	sbc_initialized = true;
	this->sbc.endian = SBC_LE;
	this->mtu = mtu;

	if (info->media_type != SPA_MEDIA_TYPE_audio ||
	    info->media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    info->info.raw.format != SPA_AUDIO_FORMAT_S16) {
		res = -EINVAL;
		goto error;
	}

	switch (conf->sink_frequency) {
	case FASTSTREAM_SINK_SAMPLING_FREQ_44100:
		this->sbc.frequency = SBC_FREQ_44100;
		break;
	case FASTSTREAM_SINK_SAMPLING_FREQ_48000:
		this->sbc.frequency = SBC_FREQ_48000;
		break;
	default:
		res = -EINVAL;
                goto error;
        }

	this->sbc.mode = SBC_MODE_JOINT_STEREO;
	this->sbc.subbands = SBC_SB_8;
	this->sbc.allocation = SBC_AM_LOUDNESS;
	this->sbc.blocks = SBC_BLK_16;
	this->sbc.bitpool = 29;

	this->codesize = sbc_get_codesize(&this->sbc);

	this->max_frames = 3;
	if (this->mtu < this->max_frames * ceil2(sbc_get_frame_length(&this->sbc))) {
		res = -EINVAL;
		goto error;
	}

	return this;

error_errno:
	res = -errno;
	goto error;

error:
	if (sbc_initialized)
		sbc_finish(&this->sbc);
	free(this);
	errno = -res;
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;
	sbc_finish(&this->sbc);
	free(this);
}

static int codec_abr_process (void *data, size_t unsent)
{
	return -ENOTSUP;
}

static int codec_start_encode (void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp)
{
	struct impl *this = data;
	this->frame_count = 0;
	return 0;
}

static int codec_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush)
{
	struct impl *this = data;
	int res;

	res = sbc_encode(&this->sbc, src, src_size,
			dst, dst_size, (ssize_t*)dst_out);
	if (SPA_UNLIKELY(res < 0))
		return -EINVAL;
	spa_assert(res == this->codesize);

	if (*dst_out % 2 != 0 && *dst_out < dst_size) {
		/* Pad similarly as in input stream */
		*((uint8_t *)dst + *dst_out) = 0;
		++*dst_out;
	}

	this->frame_count += res / this->codesize;
	*need_flush = (this->frame_count >= this->max_frames) ? NEED_FLUSH_ALL : NEED_FLUSH_NO;
	return res;
}

static SPA_UNUSED int codec_start_decode (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	return 0;
}

static int do_decode(sbc_t *sbc,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	size_t processed = 0;
	int res;

	*dst_out = 0;

	/* Scan for SBC syncword.
	 * We could probably assume 1-byte paddings instead,
	 * which devices seem to be sending.
	 */
	while (src_size >= 1) {
		if (*(uint8_t*)src == 0x9C)
			break;
		src = (uint8_t*)src + 1;
		--src_size;
		++processed;
	}

	res = sbc_decode(sbc, src, src_size,
			dst, dst_size, dst_out);
	if (res <= 0)
		res = SPA_MIN((size_t)1, src_size);    /* skip bad payload */

	processed += res;
	return processed;
}

static SPA_UNUSED int codec_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct impl *this = data;
	return do_decode(&this->sbc, src, src_size, dst, dst_size, dst_out);
}

/*
 * Duplex codec
 *
 * When connected as SRC to SNK, FastStream sink may send back SBC data.
 */

static int duplex_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	a2dp_faststream_t conf;
	struct spa_audio_info_raw info = { 0, };

	if (caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (idx > 0)
		return 0;

	switch (conf.source_frequency) {
	case FASTSTREAM_SOURCE_SAMPLING_FREQ_16000:
		info.rate = 16000;
		break;
	default:
		return -EINVAL;
        }

	/*
	 * Some headsets send mono stream, others stereo.  This information
	 * is contained in the SBC headers, and becomes known only when
	 * stream arrives. To be able to work in both cases, we will
	 * produce 2-channel output, and will double the channels
	 * in the decoding step if mono stream was received.
	 */
	info.format = SPA_AUDIO_FORMAT_S16_LE;
	info.channels = 2;
	info.position[0] = SPA_AUDIO_CHANNEL_FL;
	info.position[1] = SPA_AUDIO_CHANNEL_FR;

	*param = spa_format_audio_raw_build(b, id, &info);
	return *param == NULL ? -EIO : 1;
}

static int duplex_validate_config(const struct media_codec *codec, uint32_t flags,
			const void *caps, size_t caps_size,
			struct spa_audio_info *info)
{
	spa_zero(*info);
	info->media_type = SPA_MEDIA_TYPE_audio;
	info->media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info->info.raw.format = SPA_AUDIO_FORMAT_S16_LE;
	info->info.raw.channels = 2;
	info->info.raw.position[0] = SPA_AUDIO_CHANNEL_FL;
	info->info.raw.position[1] = SPA_AUDIO_CHANNEL_FR;
	info->info.raw.rate = 16000;
	return 0;
}

static int duplex_reduce_bitpool(void *data)
{
	return -ENOTSUP;
}

static int duplex_increase_bitpool(void *data)
{
	return -ENOTSUP;
}

static int duplex_get_block_size(void *data)
{
	return 0;
}

static void *duplex_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	a2dp_faststream_t *conf = config;
	struct duplex_impl *this = NULL;
	int res;

	if (info->media_type != SPA_MEDIA_TYPE_audio ||
	    info->media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    info->info.raw.format != SPA_AUDIO_FORMAT_S16_LE) {
		res = -EINVAL;
		goto error;
	}

	if ((this = calloc(1, sizeof(struct duplex_impl))) == NULL)
		goto error_errno;

	if ((res = sbc_init(&this->sbc, 0)) < 0)
		goto error;

	switch (conf->source_frequency) {
	case FASTSTREAM_SOURCE_SAMPLING_FREQ_16000:
		this->sbc.frequency = SBC_FREQ_16000;
		break;
	default:
		res = -EINVAL;
                goto error;
        }

	this->sbc.endian = SBC_LE;
	this->sbc.mode = SBC_MODE_MONO;
	this->sbc.subbands = SBC_SB_8;
	this->sbc.allocation = SBC_AM_LOUDNESS;
	this->sbc.blocks = SBC_BLK_16;
	this->sbc.bitpool = 32;

	return this;

error_errno:
	res = -errno;
	goto error;
error:
	free(this);
	errno = -res;
	return NULL;
}

static void duplex_deinit(void *data)
{
	struct duplex_impl *this = data;
	sbc_finish(&this->sbc);
	free(this);
}

static int duplex_abr_process (void *data, size_t unsent)
{
	return -ENOTSUP;
}

static int duplex_start_encode (void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp)
{
	return -ENOTSUP;
}

static int duplex_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush)
{
	return -ENOTSUP;
}

static int duplex_start_decode (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	return 0;
}

/** Convert S16LE stereo -> S16LE mono, in-place (only for testing purposes) */
static SPA_UNUSED size_t convert_s16le_c2_to_c1(int16_t *data, size_t size, size_t max_size)
{
	size_t i;
	for (i = 0; i < size / 2; ++i)
#if __BYTE_ORDER == __LITTLE_ENDIAN
		data[i] = data[2*i]/2 + data[2*i+1]/2;
#else
		data[i] = bswap_16(bswap_16(data[2*i])/2 + bswap_16(data[2*i+1])/2);
#endif
	return size / 2;
}

/** Convert S16LE mono -> S16LE stereo, in-place */
static size_t convert_s16le_c1_to_c2(uint8_t *data, size_t size, size_t max_size)
{
	size_t pos;

	pos = 2 * SPA_MIN(size / 2, max_size / 4);
	size = 2 * pos;

	/* We'll trust the compiler to optimize this */
	while (pos >= 2) {
		pos -= 2;
		data[2*pos+3] = data[pos+1];
		data[2*pos+2] = data[pos];
		data[2*pos+1] = data[pos+1];
		data[2*pos] = data[pos];
	}

	return size;
}

static int duplex_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct duplex_impl *this = data;
	int res;

	*dst_out = 0;
	res = do_decode(&this->sbc, src, src_size, dst, dst_size, dst_out);

	/*
	 * Depending on headers of first frame, libsbc may output either
	 * 1 or 2 channels. This function should always produce 2 channels,
	 * so we'll just double the channels here.
	 */
	if (this->sbc.mode == SBC_MODE_MONO)
		*dst_out = convert_s16le_c1_to_c2(dst, *dst_out, dst_size);

	return res;
}

/* Voice channel SBC, not a real A2DP codec */
static const struct media_codec duplex_codec = {
	.codec_id = A2DP_CODEC_VENDOR,
	.name = "faststream_sbc",
	.description = "FastStream duplex SBC",
	.fill_caps = codec_fill_caps,
	.select_config = codec_select_config,
	.enum_config = duplex_enum_config,
	.validate_config = duplex_validate_config,
	.init = duplex_init,
	.deinit = duplex_deinit,
	.get_block_size = duplex_get_block_size,
	.abr_process = duplex_abr_process,
	.start_encode = duplex_start_encode,
	.encode = duplex_encode,
	.start_decode = duplex_start_decode,
	.decode = duplex_decode,
	.reduce_bitpool = duplex_reduce_bitpool,
	.increase_bitpool = duplex_increase_bitpool,
};

#define FASTSTREAM_COMMON_DEFS				\
	.codec_id = A2DP_CODEC_VENDOR,			\
	.vendor = { .vendor_id = FASTSTREAM_VENDOR_ID,	\
		.codec_id = FASTSTREAM_CODEC_ID },	\
	.description = "FastStream",			\
	.fill_caps = codec_fill_caps,			\
	.select_config = codec_select_config,		\
	.enum_config = codec_enum_config,		\
	.init = codec_init,				\
	.deinit = codec_deinit,				\
	.get_block_size = codec_get_block_size,		\
	.abr_process = codec_abr_process,		\
	.start_encode = codec_start_encode,		\
	.encode = codec_encode,				\
	.reduce_bitpool = codec_reduce_bitpool,		\
	.increase_bitpool = codec_increase_bitpool

const struct media_codec a2dp_codec_faststream = {
	FASTSTREAM_COMMON_DEFS,
	.id = SPA_BLUETOOTH_AUDIO_CODEC_FASTSTREAM,
	.name = "faststream",
};

static const struct spa_dict_item duplex_info_items[] = {
	{ "duplex.boost", "true" },
};
static const struct spa_dict duplex_info = SPA_DICT_INIT_ARRAY(duplex_info_items);

const struct media_codec a2dp_codec_faststream_duplex = {
	FASTSTREAM_COMMON_DEFS,
	.id = SPA_BLUETOOTH_AUDIO_CODEC_FASTSTREAM_DUPLEX,
	.name = "faststream_duplex",
	.duplex_codec = &duplex_codec,
	.info = &duplex_info,
};

MEDIA_CODEC_EXPORT_DEF(
	"faststream",
	&a2dp_codec_faststream,
	&a2dp_codec_faststream_duplex
);
