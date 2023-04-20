/* Spa A2DP aptX codec */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <stddef.h>
#include <errno.h>
#include <arpa/inet.h>

#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>

#include <sbc/sbc.h>

#include <freeaptx.h>

#include "rtp.h"
#include "media-codecs.h"

#define APTX_LL_LEVEL1(level) (((level) >> 8) & 0xFF)
#define APTX_LL_LEVEL2(level) (((level) >> 0) & 0xFF)
#define APTX_LL_LEVEL(level1, level2) ((((level1) & 0xFF) << 8) | (((level2) & 0xFF) << 0))

#define MSBC_DECODED_SIZE       240
#define MSBC_ENCODED_SIZE       60
#define MSBC_PAYLOAD_SIZE       57

/*
 * XXX: Bump requested device buffer levels up by 50% from defaults,
 * XXX: increasing latency similarly. This seems to be necessary for
 * XXX: stable output when moving headphones. It might be possible to
 * XXX: reduce this by changing the scheduling of the socket writes.
 */
#define LL_LEVEL_ADJUSTMENT	3/2

struct impl {
	struct aptx_context *aptx;

	struct rtp_header *header;

	size_t mtu;
	int codesize;
	int frame_length;
	int frame_count;
	int max_frames;

	bool hd;
};

struct msbc_impl {
	sbc_t msbc;
};

static inline bool codec_is_hd(const struct media_codec *codec)
{
	return codec->vendor.codec_id == APTX_HD_CODEC_ID
		&& codec->vendor.vendor_id == APTX_HD_VENDOR_ID;
}

static inline bool codec_is_ll(const struct media_codec *codec)
{
	return (codec->id == SPA_BLUETOOTH_AUDIO_CODEC_APTX_LL) ||
		(codec->id == SPA_BLUETOOTH_AUDIO_CODEC_APTX_LL_DUPLEX);
}

static inline size_t codec_get_caps_size(const struct media_codec *codec)
{
	if (codec_is_hd(codec))
		return sizeof(a2dp_aptx_hd_t);
	else if (codec_is_ll(codec))
		return sizeof(a2dp_aptx_ll_t);
	else
		return sizeof(a2dp_aptx_t);
}

static int codec_fill_caps(const struct media_codec *codec, uint32_t flags,
		uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	size_t actual_conf_size = codec_get_caps_size(codec);
	const a2dp_aptx_t a2dp_aptx = {
		.info = codec->vendor,
		.frequency =
			APTX_SAMPLING_FREQ_16000 |
			APTX_SAMPLING_FREQ_32000 |
			APTX_SAMPLING_FREQ_44100 |
			APTX_SAMPLING_FREQ_48000,
		.channel_mode =
			APTX_CHANNEL_MODE_STEREO,
	};
	const a2dp_aptx_ll_t a2dp_aptx_ll = {
		.aptx = a2dp_aptx,
		.bidirect_link = codec->duplex_codec ? true : false,
		.has_new_caps = false,
	};
	if (codec_is_ll(codec))
		memcpy(caps, &a2dp_aptx_ll, sizeof(a2dp_aptx_ll));
	else
		memcpy(caps, &a2dp_aptx, sizeof(a2dp_aptx));
	return actual_conf_size;
}

static const struct media_codec_config
aptx_frequencies[] = {
	{ APTX_SAMPLING_FREQ_48000, 48000, 3 },
	{ APTX_SAMPLING_FREQ_44100, 44100, 2 },
	{ APTX_SAMPLING_FREQ_32000, 32000, 1 },
	{ APTX_SAMPLING_FREQ_16000, 16000, 0 },
};

static int codec_select_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size,
		const struct media_codec_audio_info *info,
		const struct spa_dict *settings, uint8_t config[A2DP_MAX_CAPS_SIZE])
{
	a2dp_aptx_t conf;
	int i;
	size_t actual_conf_size = codec_get_caps_size(codec);

	if (caps_size < sizeof(conf) || actual_conf_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (codec->vendor.vendor_id != conf.info.vendor_id ||
	    codec->vendor.codec_id != conf.info.codec_id)
		return -ENOTSUP;

	if ((i = media_codec_select_config(aptx_frequencies,
					  SPA_N_ELEMENTS(aptx_frequencies),
					  conf.frequency,
				    	  info ? info->rate : A2DP_CODEC_DEFAULT_RATE
				    	  )) < 0)
		return -ENOTSUP;
	conf.frequency = aptx_frequencies[i].config;

	if (conf.channel_mode & APTX_CHANNEL_MODE_STEREO)
		conf.channel_mode = APTX_CHANNEL_MODE_STEREO;
	else
		return -ENOTSUP;

	memcpy(config, &conf, sizeof(conf));

	return actual_conf_size;
}

static int codec_select_config_ll(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size,
		const struct media_codec_audio_info *info,
		const struct spa_dict *settings, uint8_t config[A2DP_MAX_CAPS_SIZE])
{
	a2dp_aptx_ll_ext_t conf = { 0 };
	size_t actual_conf_size;
	int res;

	/* caps may contain only conf.base, or also the extended attributes */

	if (caps_size < sizeof(conf.base))
		return -EINVAL;

	memcpy(&conf, caps, SPA_MIN(caps_size, sizeof(conf)));

	actual_conf_size = conf.base.has_new_caps ? sizeof(conf) : sizeof(conf.base);
	if (caps_size < actual_conf_size)
		return -EINVAL;

	if (codec->duplex_codec && !conf.base.bidirect_link)
		return -ENOTSUP;

	if ((res = codec_select_config(codec, flags, caps, caps_size, info, settings, config)) < 0)
		return res;

	memcpy(&conf.base.aptx, config, sizeof(conf.base.aptx));

	if (conf.base.has_new_caps) {
		int target_level = APTX_LL_LEVEL(conf.target_level1, conf.target_level2);
		int initial_level = APTX_LL_LEVEL(conf.initial_level1, conf.initial_level2);
		int good_working_level = APTX_LL_LEVEL(conf.good_working_level1, conf.good_working_level2);

		target_level = SPA_MAX(target_level, APTX_LL_TARGET_CODEC_LEVEL * LL_LEVEL_ADJUSTMENT);
		initial_level = SPA_MAX(initial_level, APTX_LL_INITIAL_CODEC_LEVEL * LL_LEVEL_ADJUSTMENT);
		good_working_level = SPA_MAX(good_working_level, APTX_LL_GOOD_WORKING_LEVEL * LL_LEVEL_ADJUSTMENT);

		conf.target_level1 = APTX_LL_LEVEL1(target_level);
		conf.target_level2 = APTX_LL_LEVEL2(target_level);
		conf.initial_level1 = APTX_LL_LEVEL1(initial_level);
		conf.initial_level2 = APTX_LL_LEVEL2(initial_level);
		conf.good_working_level1 = APTX_LL_LEVEL1(good_working_level);
		conf.good_working_level2 = APTX_LL_LEVEL2(good_working_level);

		if (conf.sra_max_rate == 0)
			conf.sra_max_rate = APTX_LL_SRA_MAX_RATE;
		if (conf.sra_avg_time == 0)
			conf.sra_avg_time = APTX_LL_SRA_AVG_TIME;
	}

	memcpy(config, &conf, actual_conf_size);

	return actual_conf_size;
}

static int codec_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	a2dp_aptx_t conf;
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
			SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_S24),
			0);
	spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_rate, 0);

	spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_None, 0);
	choice = (struct spa_pod_choice*)spa_pod_builder_frame(b, &f[1]);
	i = 0;
	if (conf.frequency & APTX_SAMPLING_FREQ_48000) {
		if (i++ == 0)
			spa_pod_builder_int(b, 48000);
		spa_pod_builder_int(b, 48000);
	}
	if (conf.frequency & APTX_SAMPLING_FREQ_44100) {
		if (i++ == 0)
			spa_pod_builder_int(b, 44100);
		spa_pod_builder_int(b, 44100);
	}
	if (conf.frequency & APTX_SAMPLING_FREQ_32000) {
		if (i++ == 0)
			spa_pod_builder_int(b, 32000);
		spa_pod_builder_int(b, 32000);
	}
	if (conf.frequency & APTX_SAMPLING_FREQ_16000) {
		if (i++ == 0)
			spa_pod_builder_int(b, 16000);
		spa_pod_builder_int(b, 16000);
	}
	if (i > 1)
		choice->body.type = SPA_CHOICE_Enum;
	spa_pod_builder_pop(b, &f[1]);

	if (i == 0)
		return -EINVAL;

	if (SPA_FLAG_IS_SET(conf.channel_mode, APTX_CHANNEL_MODE_MONO | APTX_CHANNEL_MODE_STEREO)) {
		spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_channels, SPA_POD_CHOICE_RANGE_Int(2, 1, 2),
				0);
	} else if (conf.channel_mode & APTX_CHANNEL_MODE_MONO) {
		position[0] = SPA_AUDIO_CHANNEL_MONO;
		spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_channels, SPA_POD_Int(1),
				SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
					SPA_TYPE_Id, 1, position),
				0);
	} else if (conf.channel_mode & APTX_CHANNEL_MODE_STEREO) {
		position[0] = SPA_AUDIO_CHANNEL_FL;
		position[1] = SPA_AUDIO_CHANNEL_FR;
		spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_channels, SPA_POD_Int(2),
				SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
					SPA_TYPE_Id, 2, position),
				0);
	} else
		return -EINVAL;

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

static void *codec_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	struct impl *this;
	int res;

	if ((this = calloc(1, sizeof(struct impl))) == NULL)
		goto error_errno;

	this->hd = codec_is_hd(codec);

	if ((this->aptx = aptx_init(this->hd)) == NULL)
		goto error_errno;

	this->mtu = mtu;

	if (info->media_type != SPA_MEDIA_TYPE_audio ||
	    info->media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    info->info.raw.format != SPA_AUDIO_FORMAT_S24) {
		res = -EINVAL;
		goto error;
	}
	this->frame_length = this->hd ? 6 : 4;
	this->codesize = 4 * 3 * 2;

	if (this->hd)
		this->max_frames = (this->mtu - sizeof(struct rtp_header)) / this->frame_length;
	else if (codec_is_ll(codec))
		this->max_frames = SPA_MIN(256u, this->mtu) / this->frame_length;
	else
		this->max_frames = this->mtu / this->frame_length;

	return this;

error_errno:
	res = -errno;
	goto error;
error:
	if (this->aptx)
		aptx_finish(this->aptx);
	free(this);
	errno = -res;
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;
	aptx_finish(this->aptx);
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

	if (!this->hd)
		return 0;

	this->header = (struct rtp_header *)dst;
	memset(this->header, 0, sizeof(struct rtp_header));

	this->header->v = 2;
	this->header->pt = 96;
	this->header->sequence_number = htons(seqnum);
	this->header->timestamp = htonl(timestamp);
	return sizeof(struct rtp_header);
}

static int codec_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush)
{
	struct impl *this = data;
	size_t avail_dst_size;
	int res;

	avail_dst_size = (this->max_frames - this->frame_count) * this->frame_length;
	if (SPA_UNLIKELY(dst_size < avail_dst_size)) {
		*need_flush = NEED_FLUSH_ALL;
		return 0;
	}

	res = aptx_encode(this->aptx, src, src_size,
			dst, avail_dst_size, dst_out);
	if(SPA_UNLIKELY(res < 0))
		return -EINVAL;

	this->frame_count += *dst_out / this->frame_length;
	*need_flush = (this->frame_count >= this->max_frames) ? NEED_FLUSH_ALL : NEED_FLUSH_NO;
	return res;
}

static int codec_start_decode (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	struct impl *this = data;

	if (!this->hd)
		return 0;

	const struct rtp_header *header = src;
	size_t header_size = sizeof(struct rtp_header);

	spa_return_val_if_fail(src_size > header_size, -EINVAL);

	if (seqnum)
		*seqnum = ntohs(header->sequence_number);
	if (timestamp)
		*timestamp = ntohl(header->timestamp);
	return header_size;
}

static int codec_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct impl *this = data;
	int res;

	res = aptx_decode(this->aptx, src, src_size,
			dst, dst_size, dst_out);

	return res;
}

/*
 * mSBC duplex codec
 *
 * When connected as SRC to SNK, aptX-LL sink may send back mSBC data.
 */

static int msbc_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	struct spa_audio_info_raw info = { 0, };

	if (caps_size < sizeof(a2dp_aptx_ll_t))
		return -EINVAL;

	if (idx > 0)
		return 0;

	info.format = SPA_AUDIO_FORMAT_S16_LE;
	info.channels = 1;
	info.position[0] = SPA_AUDIO_CHANNEL_MONO;
	info.rate = 16000;

	*param = spa_format_audio_raw_build(b, id, &info);
	return *param == NULL ? -EIO : 1;
}

static int msbc_validate_config(const struct media_codec *codec, uint32_t flags,
			const void *caps, size_t caps_size,
			struct spa_audio_info *info)
{
	spa_zero(*info);
	info->media_type = SPA_MEDIA_TYPE_audio;
	info->media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info->info.raw.format = SPA_AUDIO_FORMAT_S16_LE;
	info->info.raw.channels = 1;
	info->info.raw.position[0] = SPA_AUDIO_CHANNEL_MONO;
	info->info.raw.rate = 16000;
	return 0;
}

static int msbc_reduce_bitpool(void *data)
{
	return -ENOTSUP;
}

static int msbc_increase_bitpool(void *data)
{
	return -ENOTSUP;
}

static int msbc_get_block_size(void *data)
{
	return MSBC_DECODED_SIZE;
}

static void *msbc_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	struct msbc_impl *this = NULL;
	int res;

	if (info->media_type != SPA_MEDIA_TYPE_audio ||
	    info->media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    info->info.raw.format != SPA_AUDIO_FORMAT_S16_LE) {
		res = -EINVAL;
		goto error;
	}

	if ((this = calloc(1, sizeof(struct msbc_impl))) == NULL)
		goto error_errno;

	if ((res = sbc_init_msbc(&this->msbc, 0)) < 0)
		goto error;

	this->msbc.endian = SBC_LE;

	return this;

error_errno:
	res = -errno;
	goto error;
error:
	free(this);
	errno = -res;
	return NULL;
}

static void msbc_deinit(void *data)
{
	struct msbc_impl *this = data;
	sbc_finish(&this->msbc);
	free(this);
}

static int msbc_abr_process (void *data, size_t unsent)
{
	return -ENOTSUP;
}

static int msbc_start_encode (void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp)
{
	return -ENOTSUP;
}

static int msbc_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush)
{
	return -ENOTSUP;
}

static int msbc_start_decode (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	return 0;
}

static int msbc_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct msbc_impl *this = data;
	const uint8_t sync[3] = { 0xAD, 0x00, 0x00 };
	size_t processed = 0;
	int res;

	spa_assert(sizeof(sync) <= MSBC_PAYLOAD_SIZE);

	*dst_out = 0;

	/* Scan for msbc sync sequence.
	 * We could probably assume fixed (<57-byte payload><1-byte pad>)+ format
	 * which devices seem to be sending. Don't know if there are variations,
	 * so we make weaker assumption here.
	 */
	while (src_size >= MSBC_PAYLOAD_SIZE) {
		if (memcmp(src, sync, sizeof(sync)) == 0)
			break;
		src = (uint8_t*)src + 1;
		--src_size;
		++processed;
	}

	res = sbc_decode(&this->msbc, src, src_size,
			dst, dst_size, dst_out);
	if (res <= 0)
		res = SPA_MIN((size_t)MSBC_PAYLOAD_SIZE, src_size);    /* skip bad payload */

	processed += res;
	return processed;
}


const struct media_codec a2dp_codec_aptx = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_APTX,
	.codec_id = A2DP_CODEC_VENDOR,
	.vendor = { .vendor_id = APTX_VENDOR_ID,
		.codec_id = APTX_CODEC_ID },
	.name = "aptx",
	.description = "aptX",
	.fill_caps = codec_fill_caps,
	.select_config = codec_select_config,
	.enum_config = codec_enum_config,
	.init = codec_init,
	.deinit = codec_deinit,
	.get_block_size = codec_get_block_size,
	.abr_process = codec_abr_process,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.start_decode = codec_start_decode,
	.decode = codec_decode,
	.reduce_bitpool = codec_reduce_bitpool,
	.increase_bitpool = codec_increase_bitpool,
};


const struct media_codec a2dp_codec_aptx_hd = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_APTX_HD,
	.codec_id = A2DP_CODEC_VENDOR,
	.vendor = { .vendor_id = APTX_HD_VENDOR_ID,
		.codec_id = APTX_HD_CODEC_ID },
	.name = "aptx_hd",
	.description = "aptX HD",
	.fill_caps = codec_fill_caps,
	.select_config = codec_select_config,
	.enum_config = codec_enum_config,
	.init = codec_init,
	.deinit = codec_deinit,
	.get_block_size = codec_get_block_size,
	.abr_process = codec_abr_process,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.start_decode = codec_start_decode,
	.decode = codec_decode,
	.reduce_bitpool = codec_reduce_bitpool,
	.increase_bitpool = codec_increase_bitpool,
};

#define APTX_LL_COMMON_DEFS				\
	.codec_id = A2DP_CODEC_VENDOR,			\
	.description = "aptX-LL",			\
	.fill_caps = codec_fill_caps,			\
	.select_config = codec_select_config_ll,	\
	.enum_config = codec_enum_config,		\
	.init = codec_init,				\
	.deinit = codec_deinit,				\
	.get_block_size = codec_get_block_size,		\
	.abr_process = codec_abr_process,		\
	.start_encode = codec_start_encode,		\
	.encode = codec_encode,				\
	.reduce_bitpool = codec_reduce_bitpool,		\
	.increase_bitpool = codec_increase_bitpool


const struct media_codec a2dp_codec_aptx_ll_0 = {
	APTX_LL_COMMON_DEFS,
	.id = SPA_BLUETOOTH_AUDIO_CODEC_APTX_LL,
	.vendor = { .vendor_id = APTX_LL_VENDOR_ID,
		.codec_id = APTX_LL_CODEC_ID },
	.name = "aptx_ll",
	.endpoint_name = "aptx_ll_0",
};

const struct media_codec a2dp_codec_aptx_ll_1 = {
	APTX_LL_COMMON_DEFS,
	.id = SPA_BLUETOOTH_AUDIO_CODEC_APTX_LL,
	.vendor = { .vendor_id = APTX_LL_VENDOR_ID2,
		.codec_id = APTX_LL_CODEC_ID },
	.name = "aptx_ll",
	.endpoint_name = "aptx_ll_1",
};

/* Voice channel mSBC, not a real A2DP codec */
static const struct media_codec aptx_ll_msbc = {
	.codec_id = A2DP_CODEC_VENDOR,
	.name = "aptx_ll_msbc",
	.description = "aptX-LL mSBC",
	.fill_caps = codec_fill_caps,
	.select_config = codec_select_config_ll,
	.enum_config = msbc_enum_config,
	.validate_config = msbc_validate_config,
	.init = msbc_init,
	.deinit = msbc_deinit,
	.get_block_size = msbc_get_block_size,
	.abr_process = msbc_abr_process,
	.start_encode = msbc_start_encode,
	.encode = msbc_encode,
	.start_decode = msbc_start_decode,
	.decode = msbc_decode,
	.reduce_bitpool = msbc_reduce_bitpool,
	.increase_bitpool = msbc_increase_bitpool,
};

static const struct spa_dict_item duplex_info_items[] = {
	{ "duplex.boost", "true" },
};
static const struct spa_dict duplex_info = SPA_DICT_INIT_ARRAY(duplex_info_items);

const struct media_codec a2dp_codec_aptx_ll_duplex_0 = {
	APTX_LL_COMMON_DEFS,
	.id = SPA_BLUETOOTH_AUDIO_CODEC_APTX_LL_DUPLEX,
	.vendor = { .vendor_id = APTX_LL_VENDOR_ID,
		.codec_id = APTX_LL_CODEC_ID },
	.name = "aptx_ll_duplex",
	.endpoint_name = "aptx_ll_duplex_0",
	.duplex_codec = &aptx_ll_msbc,
	.info = &duplex_info,
};

const struct media_codec a2dp_codec_aptx_ll_duplex_1 = {
	APTX_LL_COMMON_DEFS,
	.id = SPA_BLUETOOTH_AUDIO_CODEC_APTX_LL_DUPLEX,
	.vendor = { .vendor_id = APTX_LL_VENDOR_ID2,
		.codec_id = APTX_LL_CODEC_ID },
	.name = "aptx_ll_duplex",
	.endpoint_name = "aptx_ll_duplex_1",
	.duplex_codec = &aptx_ll_msbc,
	.info = &duplex_info,
};

MEDIA_CODEC_EXPORT_DEF(
	"aptx",
	&a2dp_codec_aptx_hd,
	&a2dp_codec_aptx,
	&a2dp_codec_aptx_ll_0,
	&a2dp_codec_aptx_ll_1,
	&a2dp_codec_aptx_ll_duplex_0,
	&a2dp_codec_aptx_ll_duplex_1
);
