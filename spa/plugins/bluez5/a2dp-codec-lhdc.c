/* Spa A2DP LHDC v5 codec */
/* SPDX-License-Identifier: MIT */

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <spa/param/audio/format.h>
#include <spa/utils/dict.h>
#include <spa/utils/string.h>

#include <lhdcv5BT.h>

#include "rtp.h"
#include "media-codecs.h"

#define LHDCV5_CHANNELS 2
#define LHDCV5_FRAME_CONFIG (LHDC_V5_FRAME_LEN_5MS | LHDC_V5_VERSION_1)
#define LHDCV5_LOCAL_MIN_BITRATE LHDC_V5_MIN_BITRATE_64K
#define LHDCV5_LOCAL_MAX_BITRATE LHDC_V5_MAX_BITRATE_1000K
#define LHDCV5_BITRATE_CONFIG (LHDCV5_LOCAL_MIN_BITRATE | LHDCV5_LOCAL_MAX_BITRATE)
#define LHDCV5_FORMAT_CONFIG (LHDC_V5_BIT_FMT_16 | LHDC_V5_BIT_FMT_24)
#define LHDCV5_MAX_FRAME_COUNT 0x3f

static struct spa_log *log_;

static const struct media_codec_config lhdc_frequencies[] = {
	{ LHDC_V5_SAMPLING_FREQ_44100, 44100, 3 },
	{ LHDC_V5_SAMPLING_FREQ_48000, 48000, 2 },
	{ LHDC_V5_SAMPLING_FREQ_96000, 96000, 1 },
	{ LHDC_V5_SAMPLING_FREQ_192000, 192000, 0 },
};

struct lhdc_format_config {
	uint32_t config;
	enum spa_audio_format format;
	uint32_t bits_per_sample;
	uint32_t sample_size;
};

static const struct lhdc_format_config lhdc_formats[] = {
	{ LHDC_V5_BIT_FMT_24, SPA_AUDIO_FORMAT_S24, LHDCBT_SMPL_FMT_S24, 3 },
	{ LHDC_V5_BIT_FMT_16, SPA_AUDIO_FORMAT_S16, LHDCBT_SMPL_FMT_S16, sizeof(int16_t) },
};

struct lhdc_bitrate_config {
	uint32_t config;
	uint32_t bitrate;
};

static const struct lhdc_bitrate_config lhdc_min_bitrates[] = {
	{ LHDC_V5_MIN_BITRATE_64K, 64 },
	{ LHDC_V5_MIN_BITRATE_160K, 160 },
	{ LHDC_V5_MIN_BITRATE_256K, 256 },
	{ LHDC_V5_MIN_BITRATE_400K, 400 },
};

static const struct lhdc_bitrate_config lhdc_max_bitrates[] = {
	{ LHDC_V5_MAX_BITRATE_400K, 400 },
	{ LHDC_V5_MAX_BITRATE_500K, 500 },
	{ LHDC_V5_MAX_BITRATE_900K, 900 },
	{ LHDC_V5_MAX_BITRATE_1000K, 1000 },
};

struct lhdc_quality_config {
	uint32_t quality;
	const char *label;
};

static const struct lhdc_quality_config lhdc_quality_configs[] = {
	{ LHDC_QUALITY_LOW, "400k" },
	{ LHDC_QUALITY_MID, "500k" },
	{ LHDC_QUALITY_HIGH, "900k" },
	{ LHDC_QUALITY_HIGH1, "1000k" },
};

struct props {
	uint32_t quality;
};

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

	uint32_t block_bytes;
};

static const struct lhdc_format_config *select_format_config(uint32_t config)
{
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(lhdc_formats); i++)
		if (config & lhdc_formats[i].config)
			return &lhdc_formats[i];
	return NULL;
}

static const struct lhdc_format_config *get_format_config(uint32_t config)
{
	uint32_t format = config & LHDCV5_FORMAT_CONFIG;
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(lhdc_formats); i++)
		if (format == lhdc_formats[i].config)
			return &lhdc_formats[i];
	return NULL;
}

static const struct lhdc_bitrate_config *get_bitrate_config(
		const struct lhdc_bitrate_config configs[], size_t n, uint32_t config)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (config == configs[i].config)
			return &configs[i];
	return NULL;
}

static const struct lhdc_bitrate_config *get_min_bitrate_config(uint32_t config)
{
	return get_bitrate_config(lhdc_min_bitrates, SPA_N_ELEMENTS(lhdc_min_bitrates),
			config & LHDC_V5_MIN_BITRATE_MASK);
}

static const struct lhdc_bitrate_config *get_max_bitrate_config(uint32_t config)
{
	return get_bitrate_config(lhdc_max_bitrates, SPA_N_ELEMENTS(lhdc_max_bitrates),
			config & LHDC_V5_MAX_BITRATE_MASK);
}

static const struct lhdc_bitrate_config *find_min_bitrate_config(uint32_t bitrate)
{
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(lhdc_min_bitrates); i++)
		if (lhdc_min_bitrates[i].bitrate >= bitrate)
			return &lhdc_min_bitrates[i];
	return NULL;
}

static const struct lhdc_bitrate_config *find_max_bitrate_config(uint32_t bitrate)
{
	const struct lhdc_bitrate_config *res = NULL;
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(lhdc_max_bitrates); i++)
		if (lhdc_max_bitrates[i].bitrate <= bitrate)
			res = &lhdc_max_bitrates[i];
	return res;
}

static int select_bitrate_config(uint32_t peer_config, uint8_t *bitrate_config)
{
	const struct lhdc_bitrate_config *peer_min, *peer_max, *local_min, *local_max;
	const struct lhdc_bitrate_config *min, *max;
	uint32_t min_bitrate, max_bitrate;

	peer_min = get_min_bitrate_config(peer_config);
	peer_max = get_max_bitrate_config(peer_config);
	local_min = get_min_bitrate_config(LHDCV5_LOCAL_MIN_BITRATE);
	local_max = get_max_bitrate_config(LHDCV5_LOCAL_MAX_BITRATE);
	if (peer_min == NULL || peer_max == NULL || local_min == NULL || local_max == NULL)
		return -EINVAL;

	min_bitrate = peer_min->bitrate > local_min->bitrate ? peer_min->bitrate : local_min->bitrate;
	max_bitrate = peer_max->bitrate < local_max->bitrate ? peer_max->bitrate : local_max->bitrate;
	if (min_bitrate > max_bitrate)
		return -ENOTSUP;

	min = find_min_bitrate_config(min_bitrate);
	max = find_max_bitrate_config(max_bitrate);
	if (min == NULL || max == NULL || min->bitrate > max->bitrate)
		return -ENOTSUP;

	*bitrate_config = min->config | max->config;
	return 0;
}

static bool get_bitrate_indices(uint32_t config, uint32_t rate,
		uint32_t *min_bitrate_index, uint32_t *max_bitrate_index)
{
	switch (config & LHDC_V5_MIN_BITRATE_MASK) {
	case LHDC_V5_MIN_BITRATE_64K:
		*min_bitrate_index = LHDC_QUALITY_LOW0;
		break;
	case LHDC_V5_MIN_BITRATE_160K:
		*min_bitrate_index = LHDC_QUALITY_LOW1;
		break;
	case LHDC_V5_MIN_BITRATE_256K:
		/* Pick the first liblhdcv5 bitrate table index >= 256k:
		 * 44.1 kHz maps LOW3 to 240k and LOW4 to 320k.
		 */
		*min_bitrate_index = rate == 44100 ? LHDC_QUALITY_LOW4 : LHDC_QUALITY_LOW3;
		break;
	case LHDC_V5_MIN_BITRATE_400K:
		*min_bitrate_index = LHDC_QUALITY_LOW;
		break;
	default:
		return false;
	}

	switch (config & LHDC_V5_MAX_BITRATE_MASK) {
	case LHDC_V5_MAX_BITRATE_400K:
		*max_bitrate_index = LHDC_QUALITY_LOW;
		break;
	case LHDC_V5_MAX_BITRATE_500K:
		*max_bitrate_index = LHDC_QUALITY_MID;
		break;
	case LHDC_V5_MAX_BITRATE_900K:
		*max_bitrate_index = LHDC_QUALITY_HIGH;
		break;
	case LHDC_V5_MAX_BITRATE_1000K:
		*max_bitrate_index = LHDC_QUALITY_HIGH1;
		break;
	default:
		return false;
	}

	return *min_bitrate_index <= *max_bitrate_index;
}

static const struct lhdc_quality_config *find_quality_config(uint32_t quality)
{
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(lhdc_quality_configs); i++)
		if (lhdc_quality_configs[i].quality == quality)
			return &lhdc_quality_configs[i];
	return NULL;
}

static bool is_fixed_quality(uint32_t quality)
{
	return find_quality_config(quality) != NULL;
}

static uint32_t clamp_quality(uint32_t quality,
		uint32_t min_bitrate_index, uint32_t max_bitrate_index)
{
	if (quality == LHDC_QUALITY_AUTO)
		return quality;
	if (quality < min_bitrate_index)
		return min_bitrate_index;
	if (!is_fixed_quality(quality) && quality > max_bitrate_index)
		return max_bitrate_index;
	return quality;
}

static uint32_t get_effective_max_bitrate_index(uint32_t max_bitrate_index,
		uint32_t quality)
{
	if (is_fixed_quality(quality) && max_bitrate_index < quality)
		return quality;
	return max_bitrate_index;
}

static int codec_fill_caps(const struct media_codec *codec, uint32_t flags,
		const struct spa_dict *settings, uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	const a2dp_lhdc_v5_t conf = {
		.info = codec->vendor,
		.sampling_freq = LHDC_V5_SAMPLING_FREQ_44100 |
			LHDC_V5_SAMPLING_FREQ_48000 |
			LHDC_V5_SAMPLING_FREQ_96000 |
			LHDC_V5_SAMPLING_FREQ_192000,
		.bitrate_and_depth = LHDCV5_BITRATE_CONFIG | LHDCV5_FORMAT_CONFIG,
		.frame_len_and_version = LHDCV5_FRAME_CONFIG,
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
	int i;
	const struct lhdc_format_config *format;
	uint8_t bitrate_config;

	if (caps_size < sizeof(conf))
		return -EINVAL;

	memcpy(&conf, caps, sizeof(conf));

	if (codec->vendor.vendor_id != conf.info.vendor_id ||
	    codec->vendor.codec_id != conf.info.codec_id)
		return -ENOTSUP;

	i = media_codec_select_config(lhdc_frequencies,
			SPA_N_ELEMENTS(lhdc_frequencies), conf.sampling_freq,
			info ? info->rate : A2DP_CODEC_DEFAULT_RATE);
	if (i < 0)
		return -ENOTSUP;
	conf.sampling_freq = lhdc_frequencies[i].config;

	format = select_format_config(conf.bitrate_and_depth);
	if (format == NULL)
		return -ENOTSUP;

	if (select_bitrate_config(conf.bitrate_and_depth, &bitrate_config) < 0)
		return -ENOTSUP;

	if ((conf.frame_len_and_version & LHDCV5_FRAME_CONFIG) != LHDCV5_FRAME_CONFIG)
		return -ENOTSUP;

	conf.bitrate_and_depth = bitrate_config | format->config;
	conf.frame_len_and_version = LHDCV5_FRAME_CONFIG;
	conf.features &= LHDC_V5_FEATURE_LL;
	conf.reserved = 0;

	memcpy(config, &conf, sizeof(conf));
	return sizeof(conf);
}

static int codec_validate_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, struct spa_audio_info *info)
{
	const a2dp_lhdc_v5_t *conf = caps;
	const struct lhdc_format_config *format;
	uint32_t min_bitrate_index, max_bitrate_index;
	int rate;

	if (caps == NULL || caps_size < sizeof(*conf))
		return -EINVAL;

	if (codec->vendor.vendor_id != conf->info.vendor_id ||
	    codec->vendor.codec_id != conf->info.codec_id)
		return -EINVAL;

	rate = media_codec_get_config(lhdc_frequencies,
			SPA_N_ELEMENTS(lhdc_frequencies), conf->sampling_freq);
	if (rate < 0)
		return -EINVAL;

	format = get_format_config(conf->bitrate_and_depth);
	if (format == NULL)
		return -EINVAL;

	if ((conf->frame_len_and_version & LHDCV5_FRAME_CONFIG) != LHDCV5_FRAME_CONFIG)
		return -EINVAL;

	if (!get_bitrate_indices(conf->bitrate_and_depth, rate,
				&min_bitrate_index, &max_bitrate_index))
		return -EINVAL;

	spa_zero(*info);
	info->media_type = SPA_MEDIA_TYPE_audio;
	info->media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info->info.raw.format = format->format;
	info->info.raw.rate = rate;
	info->info.raw.channels = LHDCV5_CHANNELS;
	info->info.raw.position[0] = SPA_AUDIO_CHANNEL_FL;
	info->info.raw.position[1] = SPA_AUDIO_CHANNEL_FR;
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
			SPA_FORMAT_mediaType, SPA_POD_Id(info.media_type),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(info.media_subtype),
			SPA_FORMAT_AUDIO_format, SPA_POD_Id(info.info.raw.format),
			SPA_FORMAT_AUDIO_rate, SPA_POD_Int(info.info.raw.rate),
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int(info.info.raw.channels),
			SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
				SPA_TYPE_Id, info.info.raw.channels, info.info.raw.position),
			0);
	*param = spa_pod_builder_pop(b, &f[0]);
	return *param == NULL ? -EIO : 1;
}

static uint32_t string_to_quality(const char *quality)
{
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(lhdc_quality_configs); i++)
		if (spa_streq(quality, lhdc_quality_configs[i].label))
			return lhdc_quality_configs[i].quality;
	return LHDC_QUALITY_AUTO;
}

static void *codec_init_props(const struct media_codec *codec, uint32_t flags,
		const struct spa_dict *settings)
{
	struct props *p = calloc(1, sizeof(struct props));
	const char *str;

	if (p == NULL)
		return NULL;

	if (settings == NULL || (str = spa_dict_lookup(settings, "bluez5.a2dp.lhdc.quality")) == NULL)
		str = "auto";

	p->quality = string_to_quality(str);
	return p;
}

static void codec_clear_props(void *props)
{
	free(props);
}

static int init_lhdc_handle(HANDLE_LHDC_BT *handle, uint32_t rate,
		uint32_t bits_per_sample, uint32_t quality, uint32_t mtu, uint32_t interval,
		uint32_t min_bitrate_index, uint32_t max_bitrate_index,
		uint32_t *block_samples)
{
	HANDLE_LHDC_BT lhdc = NULL;
	uint32_t effective_max_bitrate_index;
	int32_t res;

	res = lhdcv5BT_get_handle(LHDC_VERSION_1, &lhdc);
	if (res != LHDC_FRET_SUCCESS || lhdc == NULL) {
		spa_log_error(log_, "LHDC v5 get_handle failed: %d", res);
		return -EIO;
	}

	res = lhdcv5BT_init_encoder(lhdc, rate, bits_per_sample, quality, mtu,
			interval, 0);
	if (res != LHDC_FRET_SUCCESS) {
		spa_log_error(log_, "LHDC v5 encoder initialization failed: %d", res);
		goto error;
	}

	res = lhdcv5BT_set_min_bitrate(lhdc, min_bitrate_index);
	if (res != LHDC_FRET_SUCCESS) {
		spa_log_error(log_, "LHDC v5 set min bitrate failed: %d", res);
		goto error;
	}

	effective_max_bitrate_index =
		get_effective_max_bitrate_index(max_bitrate_index, quality);
	res = lhdcv5BT_set_max_bitrate(lhdc, effective_max_bitrate_index);
	if (res != LHDC_FRET_SUCCESS) {
		spa_log_error(log_, "LHDC v5 set max bitrate failed: %d", res);
		goto error;
	}

	if (is_fixed_quality(quality)) {
		res = lhdcv5BT_set_bitrate(lhdc, quality);
		if (res != LHDC_FRET_SUCCESS) {
			spa_log_error(log_, "LHDC v5 set bitrate failed: %d", res);
			goto error;
		}
	}

	res = lhdcv5BT_get_block_Size(lhdc, block_samples);
	if (res != LHDC_FRET_SUCCESS || *block_samples == 0) {
		spa_log_error(log_, "LHDC v5 get block size failed: %d", res);
		goto error;
	}

	*handle = lhdc;
	return 0;

error:
	lhdcv5BT_free_handle(lhdc);
	return -EIO;
}

static void *codec_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	struct impl *this;
	struct spa_audio_info config_info;
	const a2dp_lhdc_v5_t *conf = config;
	const struct lhdc_format_config *format;
	const struct props *p = props;
	const size_t header_size = sizeof(struct rtp_header) + sizeof(struct lhdc_media_payload);
	uint32_t rate;
	uint32_t block_samples;
	uint32_t encoder_mtu;
	uint32_t interval;
	uint32_t min_bitrate_index, max_bitrate_index;
	uint32_t quality = p != NULL ? p->quality : LHDC_QUALITY_AUTO;
	int res;

	if (codec_validate_config(codec, flags, config, config_len, &config_info) < 0) {
		errno = EINVAL;
		return NULL;
	}

	format = get_format_config(conf->bitrate_and_depth);
	if (format == NULL) {
		errno = EINVAL;
		return NULL;
	}

	if (info->media_type != SPA_MEDIA_TYPE_audio ||
	    info->media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    info->info.raw.format != config_info.info.raw.format ||
	    info->info.raw.rate != config_info.info.raw.rate ||
	    info->info.raw.channels != config_info.info.raw.channels) {
		spa_log_error(log_, "LHDC v5 invalid audio format");
		errno = EINVAL;
		return NULL;
	}
	if (mtu <= header_size || mtu - header_size > UINT32_MAX) {
		spa_log_error(log_, "LHDC v5 invalid MTU: %zu", mtu);
		errno = EINVAL;
		return NULL;
	}
	encoder_mtu = (uint32_t)(mtu - header_size);
	interval = conf->features & LHDC_V5_FEATURE_LL ?
		LHDC_ENC_INTERVAL_10MS : LHDC_ENC_INTERVAL_20MS;

	rate = config_info.info.raw.rate;
	if (!get_bitrate_indices(conf->bitrate_and_depth, rate,
				&min_bitrate_index, &max_bitrate_index)) {
		errno = EINVAL;
		return NULL;
	}
	quality = clamp_quality(quality, min_bitrate_index, max_bitrate_index);

	this = calloc(1, sizeof(struct impl));
	if (this == NULL)
		return NULL;

	res = init_lhdc_handle(&this->lhdc, rate, format->bits_per_sample,
			quality, encoder_mtu, interval, min_bitrate_index,
			max_bitrate_index, &block_samples);
	if (res < 0)
		goto error;

	if (block_samples > UINT32_MAX / LHDCV5_CHANNELS / format->sample_size) {
		spa_log_error(log_, "LHDC v5 block size overflow");
		goto error;
	}
	this->block_bytes = block_samples * LHDCV5_CHANNELS * format->sample_size;
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
	if (res != LHDC_FRET_SUCCESS) {
		spa_log_error(log_, "LHDC v5 encode failed: %d", res);
		return -EIO;
	}

	if (written == 0)
		return this->block_bytes;
	if (written > dst_size)
		return -EIO;
	if (frames == 0 || frames > LHDCV5_MAX_FRAME_COUNT)
		return -EIO;

	this->payload->latency = 0;
	this->payload->frame_count = frames;
	this->payload->seq_number = this->media_seqnum++;

	*dst_out = written;
	*need_flush = NEED_FLUSH_ALL;
	return this->block_bytes;
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
	.init_props = codec_init_props,
	.clear_props = codec_clear_props,
	.init = codec_init,
	.deinit = codec_deinit,
	.get_block_size = codec_get_block_size,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.set_log = codec_set_log,
};

MEDIA_CODEC_EXPORT_DEF(
	"lhdc",
	&a2dp_codec_lhdc_v5
);
