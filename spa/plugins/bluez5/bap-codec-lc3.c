/* Spa BAP LC3 codec */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2022 Pauli Virtanen */
/* SPDX-FileCopyrightText: Copyright © 2022 Collabora */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <errno.h>
#include <arpa/inet.h>
#include <bluetooth/bluetooth.h>

#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/string.h>
#include <spa/debug/log.h>

#include <lc3.h>

#include "media-codecs.h"
#include "bap-codec-caps.h"

#define MAX_PACS	64

static struct spa_log *log;
static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.codecs.lc3");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

struct impl {
	lc3_encoder_t enc[LC3_MAX_CHANNELS];
	lc3_decoder_t dec[LC3_MAX_CHANNELS];

	int mtu;
	int samplerate;
	int channels;
	int frame_dus;
	int framelen;
	int samples;
	unsigned int codesize;
};

struct pac_data {
	const uint8_t *data;
	size_t size;
	int index;
	uint32_t locations;
};

typedef struct {
	uint8_t rate;
	uint8_t frame_duration;
	uint32_t channels;
	uint16_t framelen;
	uint8_t n_blks;
} bap_lc3_t;

static const struct {
	uint32_t bit;
	enum spa_audio_channel channel;
} channel_bits[] = {
	{ BAP_CHANNEL_FL,   SPA_AUDIO_CHANNEL_FL },
	{ BAP_CHANNEL_FR,   SPA_AUDIO_CHANNEL_FR },
	{ BAP_CHANNEL_FC,   SPA_AUDIO_CHANNEL_FC },
	{ BAP_CHANNEL_LFE,  SPA_AUDIO_CHANNEL_LFE },
	{ BAP_CHANNEL_BL,   SPA_AUDIO_CHANNEL_RL },
	{ BAP_CHANNEL_BR,   SPA_AUDIO_CHANNEL_RR },
	{ BAP_CHANNEL_FLC,  SPA_AUDIO_CHANNEL_FLC },
	{ BAP_CHANNEL_FRC,  SPA_AUDIO_CHANNEL_FRC },
	{ BAP_CHANNEL_BC,   SPA_AUDIO_CHANNEL_BC },
	{ BAP_CHANNEL_LFE2, SPA_AUDIO_CHANNEL_LFE2 },
	{ BAP_CHANNEL_SL,   SPA_AUDIO_CHANNEL_SL },
	{ BAP_CHANNEL_SR,   SPA_AUDIO_CHANNEL_SR },
	{ BAP_CHANNEL_TFL,  SPA_AUDIO_CHANNEL_TFL },
	{ BAP_CHANNEL_TFR,  SPA_AUDIO_CHANNEL_TFR },
	{ BAP_CHANNEL_TFC,  SPA_AUDIO_CHANNEL_TFC },
	{ BAP_CHANNEL_TC,   SPA_AUDIO_CHANNEL_TC },
	{ BAP_CHANNEL_TBL,  SPA_AUDIO_CHANNEL_TRL },
	{ BAP_CHANNEL_TBR,  SPA_AUDIO_CHANNEL_TRR },
	{ BAP_CHANNEL_TSL,  SPA_AUDIO_CHANNEL_TSL },
	{ BAP_CHANNEL_TSR,  SPA_AUDIO_CHANNEL_TSR },
	{ BAP_CHANNEL_TBC,  SPA_AUDIO_CHANNEL_TRC },
	{ BAP_CHANNEL_BFC,  SPA_AUDIO_CHANNEL_BC },
	{ BAP_CHANNEL_BFL,  SPA_AUDIO_CHANNEL_BLC },
	{ BAP_CHANNEL_BFR,  SPA_AUDIO_CHANNEL_BRC },
	{ BAP_CHANNEL_FLW,  SPA_AUDIO_CHANNEL_FLW },
	{ BAP_CHANNEL_FRW,  SPA_AUDIO_CHANNEL_FRW },
	{ BAP_CHANNEL_LS,   SPA_AUDIO_CHANNEL_SL }, /* is it the right mapping? */
	{ BAP_CHANNEL_RS,   SPA_AUDIO_CHANNEL_SR }, /* is it the right mapping? */
};

static int write_ltv(uint8_t *dest, uint8_t type, void* value, size_t len)
{
	struct ltv *ltv = (struct ltv *)dest;

	ltv->len = len + 1;
	ltv->type = type;
	memcpy(ltv->value, value, len);

	return len + 2;
}

static int write_ltv_uint8(uint8_t *dest, uint8_t type, uint8_t value)
{
	return write_ltv(dest, type, &value, sizeof(value));
}

static int write_ltv_uint16(uint8_t *dest, uint8_t type, uint16_t value)
{
	return write_ltv(dest, type, &value, sizeof(value));
}

static int write_ltv_uint32(uint8_t *dest, uint8_t type, uint32_t value)
{
	return write_ltv(dest, type, &value, sizeof(value));
}

static int codec_fill_caps(const struct media_codec *codec, uint32_t flags,
		uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	uint8_t *data = caps;
	uint16_t framelen[2] = {htobs(LC3_MIN_FRAME_BYTES), htobs(LC3_MAX_FRAME_BYTES)};

	data += write_ltv_uint16(data, LC3_TYPE_FREQ,
	                         htobs(LC3_FREQ_48KHZ | LC3_FREQ_24KHZ | LC3_FREQ_16KHZ | LC3_FREQ_8KHZ));
	data += write_ltv_uint8(data, LC3_TYPE_DUR, LC3_DUR_ANY);
	data += write_ltv_uint8(data, LC3_TYPE_CHAN, LC3_CHAN_1 | LC3_CHAN_2);
	data += write_ltv(data, LC3_TYPE_FRAMELEN, framelen, sizeof(framelen));
	/* XXX: we support only one frame block -> max 2 frames per SDU */
	data += write_ltv_uint8(data, LC3_TYPE_BLKS, 2);

	return data - caps;
}

static void debugc_ltv(struct spa_debug_context *debug_ctx, int pac, struct ltv *ltv)
{
	switch (ltv->len) {
	case 0:
		spa_debugc(debug_ctx, "PAC %d: --", pac);
		break;
	case 2:
		spa_debugc(debug_ctx, "PAC %d: 0x%02x %x", pac, ltv->type, ltv->value[0]);
		break;
	case 3:
		spa_debugc(debug_ctx, "PAC %d: 0x%02x %x %x", pac, ltv->type, ltv->value[0], ltv->value[1]);
		break;
	case 5:
		spa_debugc(debug_ctx, "PAC %d: 0x%02x %x %x %x %x", pac, ltv->type,
				ltv->value[0], ltv->value[1], ltv->value[2], ltv->value[3]);
		break;
	default:
		spa_debugc(debug_ctx, "PAC %d: 0x%02x", pac, ltv->type);
		spa_debugc_mem(debug_ctx, 7, ltv->value, ltv->len - 1);
		break;
	}
}

static int parse_bluez_pacs(const uint8_t *data, size_t data_size, struct pac_data pacs[MAX_PACS],
		struct spa_debug_context *debug_ctx)
{
	/*
	 * BlueZ capabilites for the same codec may contain multiple
	 * PACs separated by zero-length LTV (see BlueZ b907befc2d80)
	 */
	int pac = 0;

	pacs[pac] = (struct pac_data){ data, 0 };

	while (data_size > 0) {
		struct ltv *ltv = (struct ltv *)data;

		if (ltv->len == 0) {
			/* delimiter */
			if (pac + 1 >= MAX_PACS)
				break;

			++pac;
			pacs[pac] = (struct pac_data){ data + 1, 0, pac };
		} else if (ltv->len >= data_size) {
			return -EINVAL;
		} else {
			debugc_ltv(debug_ctx, pac, ltv);
			pacs[pac].size += ltv->len + 1;
		}
		data_size -= ltv->len + 1;
		data += ltv->len + 1;
	}

	return pac + 1;
}

static uint8_t get_num_channels(uint32_t channels)
{
	uint8_t num;

	if (channels == 0)
		return 1;  /* MONO */

	for (num = 0; channels; channels >>= 1)
		if (channels & 0x1)
			++num;

	return num;
}

static int select_channels(uint8_t channels, uint32_t locations, uint32_t *mapping, unsigned int max_channels)
{
	unsigned int i, num = 0;

	if ((channels & LC3_CHAN_2) && max_channels >= 2)
		num = 2;
	else if ((channels & LC3_CHAN_1) && max_channels >= 1)
		num = 1;
	else
		return -1;

	if (!locations) {
		*mapping = 0;  /* mono (omit Audio_Channel_Allocation) */
		return 0;
	}

	/* XXX: select some channels, but upper level should tell us what */
	*mapping = 0;
	for (i = 0; i < SPA_N_ELEMENTS(channel_bits); ++i) {
		if (locations & channel_bits[i].bit) {
			*mapping |= channel_bits[i].bit;
			--num;
			if (num == 0)
				break;
		}
	}

	return 0;
}

static bool select_config(bap_lc3_t *conf, const struct pac_data *pac,	struct spa_debug_context *debug_ctx)
{
	const uint8_t *data = pac->data;
	size_t data_size = pac->size;
	uint16_t framelen_min = 0, framelen_max = 0;
	int max_frames = -1;
	uint8_t channels = 0;

	if (!data_size)
		return false;
	memset(conf, 0, sizeof(*conf));

	conf->frame_duration = 0xFF;

	/* XXX: we always use one frame block */
	conf->n_blks = 1;

	while (data_size > 0) {
		struct ltv *ltv = (struct ltv *)data;

		if (ltv->len < sizeof(struct ltv) || ltv->len >= data_size) {
			spa_debugc(debug_ctx, "invalid LTV data");
			return false;
		}

		switch (ltv->type) {
		case LC3_TYPE_FREQ:
			spa_return_val_if_fail(ltv->len == 3, false);
			{
				uint16_t rate = ltv->value[0] + (ltv->value[1] << 8);
				if (rate & LC3_FREQ_48KHZ)
					conf->rate = LC3_CONFIG_FREQ_48KHZ;
				else if (rate & LC3_FREQ_24KHZ)
					conf->rate = LC3_CONFIG_FREQ_24KHZ;
				else if (rate & LC3_FREQ_16KHZ)
					conf->rate = LC3_CONFIG_FREQ_16KHZ;
				else if (rate & LC3_FREQ_8KHZ)
					conf->rate = LC3_CONFIG_FREQ_8KHZ;
				else {
					spa_debugc(debug_ctx, "unsupported rate: 0x%04x", rate);
					return false;
				}
			}
			break;
		case LC3_TYPE_DUR:
			spa_return_val_if_fail(ltv->len == 2, false);
			{
				uint8_t duration = ltv->value[0];
				if (duration & LC3_DUR_10)
					conf->frame_duration = LC3_CONFIG_DURATION_10;
				else if (duration & LC3_DUR_7_5)
					conf->frame_duration = LC3_CONFIG_DURATION_7_5;
				else {
					spa_debugc(debug_ctx, "unsupported duration: 0x%02x", duration);
					return false;
				}
			}
			break;
		case LC3_TYPE_CHAN:
			spa_return_val_if_fail(ltv->len == 2, false);
			{
				channels = ltv->value[0];
			}
			break;
		case LC3_TYPE_FRAMELEN:
			spa_return_val_if_fail(ltv->len == 5, false);
			framelen_min = ltv->value[0] + (ltv->value[1] << 8);
			framelen_max = ltv->value[2] + (ltv->value[3] << 8);
			break;
		case LC3_TYPE_BLKS:
			spa_return_val_if_fail(ltv->len == 2, false);
			max_frames = ltv->value[0];
			break;
		default:
			spa_debugc(debug_ctx, "unknown LTV type: 0x%02x", ltv->type);
			break;
		}
		data_size -= ltv->len + 1;
		data += ltv->len + 1;
	}

	if (select_channels(channels, pac->locations, &conf->channels, max_frames) < 0) {
		spa_debugc(debug_ctx, "invalid channel configuration: 0x%02x %u",
				channels, max_frames);
		return false;
	}

	/* Default: 1 per channel (BAP v1.0.1 Sec 4.3.1) */
	if (max_frames < 0)
		max_frames = get_num_channels(conf->channels);
	if (max_frames < get_num_channels(conf->channels)) {
		spa_debugc(debug_ctx, "invalid max frames per SDU: %u", max_frames);
		return false;
	}

	if (framelen_min < LC3_MIN_FRAME_BYTES || framelen_max > LC3_MAX_FRAME_BYTES) {
		spa_debugc(debug_ctx, "invalid framelen: %u %u", framelen_min, framelen_max);
		return false;
	}
	if (conf->frame_duration == 0xFF || !conf->rate) {
		spa_debugc(debug_ctx, "no frame duration or rate");
		return false;
	}

	/* BAP v1.0.1 Table 5.2; high-reliability */
	switch (conf->rate) {
	case LC3_CONFIG_FREQ_48KHZ:
		if (conf->frame_duration == LC3_CONFIG_DURATION_7_5)
			conf->framelen = 117;	/* 48_5_2 */
		else
			conf->framelen = 120;	/* 48_4_2 */
		break;
	case LC3_CONFIG_FREQ_24KHZ:
		if (conf->frame_duration == LC3_CONFIG_DURATION_7_5)
			conf->framelen = 45;	/* 24_1_2 */
		else
			conf->framelen = 60;	/* 24_2_2 */
		break;
	case LC3_CONFIG_FREQ_16KHZ:
		if (conf->frame_duration == LC3_CONFIG_DURATION_7_5)
			conf->framelen = 30;	/* 16_1_2 */
		else
			conf->framelen = 40;	/* 16_2_2 */
		break;
	case LC3_CONFIG_FREQ_8KHZ:
		if (conf->frame_duration == LC3_CONFIG_DURATION_7_5)
			conf->framelen = 26;	/* 8_1_2 */
		else
			conf->framelen = 30;	/* 8_2_2 */
		break;
	default:
		spa_debugc(debug_ctx, "invalid rate");
		return false;
	}

	return true;
}

static bool parse_conf(bap_lc3_t *conf, const uint8_t *data, size_t data_size)
{
	if (!data_size)
		return false;
	memset(conf, 0, sizeof(*conf));

	conf->frame_duration = 0xFF;

	/* Absent Codec_Frame_Blocks_Per_SDU means 0x1 (BAP v1.0.1 Sec 4.3.2) */
	conf->n_blks = 1;

	while (data_size > 0) {
		struct ltv *ltv = (struct ltv *)data;

		if (ltv->len < sizeof(struct ltv) || ltv->len >= data_size)
			return false;

		switch (ltv->type) {
		case LC3_TYPE_FREQ:
			spa_return_val_if_fail(ltv->len == 2, false);
			conf->rate = ltv->value[0];
			break;
		case LC3_TYPE_DUR:
			spa_return_val_if_fail(ltv->len == 2, false);
			conf->frame_duration = ltv->value[0];
			break;
		case LC3_TYPE_CHAN:
			spa_return_val_if_fail(ltv->len == 5, false);
			conf->channels = ltv->value[0] + (ltv->value[1] << 8) + (ltv->value[2] << 16) + (ltv->value[3] << 24);
			break;
		case LC3_TYPE_FRAMELEN:
			spa_return_val_if_fail(ltv->len == 3, false);
			conf->framelen = ltv->value[0] + (ltv->value[1] << 8);
			break;
		case LC3_TYPE_BLKS:
			spa_return_val_if_fail(ltv->len == 2, false);
			conf->n_blks = ltv->value[0];
			/* XXX: we only support 1 frame block for now */
			if (conf->n_blks != 1)
				return false;
			break;
		default:
			return false;
		}
		data_size -= ltv->len + 1;
		data += ltv->len + 1;
	}

	if (conf->frame_duration == 0xFF || !conf->rate)
		return false;

	return true;
}

static int conf_cmp(const bap_lc3_t *conf1, int res1, const bap_lc3_t *conf2, int res2)
{
	const bap_lc3_t *conf;
	int a, b;

#define PREFER_EXPR(expr)			\
		do {				\
			conf = conf1; 		\
			a = (expr);		\
			conf = conf2;		\
			b = (expr);		\
			if (a != b)		\
				return b - a;	\
		} while (0)

#define PREFER_BOOL(expr)	PREFER_EXPR((expr) ? 1 : 0)

	/* Prefer valid */
	a = (res1 > 0 && (size_t)res1 == sizeof(bap_lc3_t)) ? 1 : 0;
	b = (res2 > 0 && (size_t)res2 == sizeof(bap_lc3_t)) ? 1 : 0;
	if (!a || !b)
		return b - a;

	PREFER_BOOL(conf->channels & LC3_CHAN_2);
	PREFER_BOOL(conf->channels & LC3_CHAN_1);
	PREFER_BOOL(conf->rate & (LC3_CONFIG_FREQ_48KHZ | LC3_CONFIG_FREQ_24KHZ | LC3_CONFIG_FREQ_16KHZ | LC3_CONFIG_FREQ_8KHZ));
	PREFER_BOOL(conf->rate & LC3_CONFIG_FREQ_48KHZ);
	PREFER_BOOL(conf->rate & LC3_CONFIG_FREQ_24KHZ);
	PREFER_BOOL(conf->rate & LC3_CONFIG_FREQ_16KHZ);
	PREFER_BOOL(conf->rate & LC3_CONFIG_FREQ_8KHZ);

	return 0;

#undef PREFER_EXPR
#undef PREFER_BOOL
}

static int pac_cmp(const void *p1, const void *p2)
{
	const struct pac_data *pac1 = p1;
	const struct pac_data *pac2 = p2;
	struct spa_debug_log_ctx debug_ctx = SPA_LOG_DEBUG_INIT(log, SPA_LOG_LEVEL_TRACE);
	bap_lc3_t conf1, conf2;
	int res1, res2;

	res1 = select_config(&conf1, pac1, &debug_ctx.ctx) ? (int)sizeof(bap_lc3_t) : -EINVAL;
	res2 = select_config(&conf2, pac2, &debug_ctx.ctx) ? (int)sizeof(bap_lc3_t) : -EINVAL;

	return conf_cmp(&conf1, res1, &conf2, res2);
}

static int codec_select_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size,
		const struct media_codec_audio_info *info,
		const struct spa_dict *settings, uint8_t config[A2DP_MAX_CAPS_SIZE])
{
	struct pac_data pacs[MAX_PACS];
	int npacs;
	bap_lc3_t conf;
	uint8_t *data = config;
	uint32_t locations = 0;
	struct spa_debug_log_ctx debug_ctx = SPA_LOG_DEBUG_INIT(log, SPA_LOG_LEVEL_TRACE);
	int i;

	if (caps == NULL)
		return -EINVAL;

	if (settings) {
		for (i = 0; i < (int)settings->n_items; ++i)
			if (spa_streq(settings->items[i].key, "bluez5.bap.locations"))
				sscanf(settings->items[i].value, "%"PRIu32, &locations);

		if (spa_atob(spa_dict_lookup(settings, "bluez5.bap.debug")))
			debug_ctx = SPA_LOG_DEBUG_INIT(log, SPA_LOG_LEVEL_DEBUG);
	}

	/* Select best conf from those possible */
	npacs = parse_bluez_pacs(caps, caps_size, pacs, &debug_ctx.ctx);
	if (npacs < 0) {
		spa_debugc(&debug_ctx.ctx, "malformed PACS");
		return npacs;
	} else if (npacs == 0) {
		spa_debugc(&debug_ctx.ctx, "no PACS");
		return -EINVAL;
	}

	for (i = 0; i < npacs; ++i)
		pacs[i].locations = locations;

	qsort(pacs, npacs, sizeof(struct pac_data), pac_cmp);

	spa_debugc(&debug_ctx.ctx, "selected PAC %d", pacs[0].index);

	if (!select_config(&conf, &pacs[0], &debug_ctx.ctx))
		return -ENOTSUP;

	data += write_ltv_uint8(data, LC3_TYPE_FREQ, conf.rate);
	data += write_ltv_uint8(data, LC3_TYPE_DUR, conf.frame_duration);

	/* Indicate MONO with absent Audio_Channel_Allocation (BAP v1.0.1 Sec. 4.3.2) */
	if (conf.channels != 0)
		data += write_ltv_uint32(data, LC3_TYPE_CHAN, htobl(conf.channels));

	data += write_ltv_uint16(data, LC3_TYPE_FRAMELEN, htobs(conf.framelen));
	data += write_ltv_uint8(data, LC3_TYPE_BLKS, conf.n_blks);

	return data - config;
}

static int codec_caps_preference_cmp(const struct media_codec *codec, uint32_t flags, const void *caps1, size_t caps1_size,
		const void *caps2, size_t caps2_size, const struct media_codec_audio_info *info, const struct spa_dict *global_settings)
{
	bap_lc3_t conf1, conf2;
	int res1, res2;

	/* Order selected configurations by preference */
	res1 = codec->select_config(codec, 0, caps1, caps1_size, info, global_settings, (uint8_t *)&conf1);
	res2 = codec->select_config(codec, 0, caps2, caps2_size, info, global_settings, (uint8_t *)&conf2);

	return conf_cmp(&conf1, res1, &conf2, res2);
}

static uint8_t channels_to_positions(uint32_t channels, uint32_t *position)
{
	uint8_t n_channels = get_num_channels(channels);
	uint8_t n_positions = 0;

	spa_assert(n_channels <= SPA_AUDIO_MAX_CHANNELS);

	if (channels == 0) {
		position[0] = SPA_AUDIO_CHANNEL_MONO;
		n_positions = 1;
	} else {
		unsigned int i;

		for (i = 0; i < SPA_N_ELEMENTS(channel_bits); ++i)
			if (channels & channel_bits[i].bit)
				position[n_positions++] = channel_bits[i].channel;
	}

	if (n_positions != n_channels)
		return 0;  /* error */

	return n_positions;
}

static int codec_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	bap_lc3_t conf;
	struct spa_pod_frame f[2];
	struct spa_pod_choice *choice;
	uint32_t position[SPA_AUDIO_MAX_CHANNELS];
	uint32_t i = 0;
	uint8_t res;

	if (!parse_conf(&conf, caps, caps_size))
		return -EINVAL;

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
	if (conf.rate == LC3_CONFIG_FREQ_48KHZ) {
		if (i++ == 0)
			spa_pod_builder_int(b, 48000);
		spa_pod_builder_int(b, 48000);
	}
	if (conf.rate == LC3_CONFIG_FREQ_24KHZ) {
		if (i++ == 0)
			spa_pod_builder_int(b, 24000);
		spa_pod_builder_int(b, 24000);
	}
	if (conf.rate == LC3_CONFIG_FREQ_16KHZ) {
		if (i++ == 0)
			spa_pod_builder_int(b, 16000);
		spa_pod_builder_int(b, 16000);
	}
	if (conf.rate == LC3_CONFIG_FREQ_8KHZ) {
		if (i++ == 0)
			spa_pod_builder_int(b, 8000);
		spa_pod_builder_int(b, 8000);
	}
	if (i > 1)
		choice->body.type = SPA_CHOICE_Enum;
	spa_pod_builder_pop(b, &f[1]);

	if (i == 0)
		return -EINVAL;

	res = channels_to_positions(conf.channels, position);
	if (res == 0)
		return -EINVAL;
	spa_pod_builder_add(b,
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int(res),
			SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t),
				SPA_TYPE_Id, res, position),
			0);

	*param = spa_pod_builder_pop(b, &f[0]);
	return *param == NULL ? -EIO : 1;
}

static int codec_validate_config(const struct media_codec *codec, uint32_t flags,
			const void *caps, size_t caps_size,
			struct spa_audio_info *info)
{
	bap_lc3_t conf;
	uint8_t res;

	if (caps == NULL)
		return -EINVAL;

	if (!parse_conf(&conf, caps, caps_size))
		return -ENOTSUP;

	spa_zero(*info);
	info->media_type = SPA_MEDIA_TYPE_audio;
	info->media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info->info.raw.format = SPA_AUDIO_FORMAT_S24_32;

	switch (conf.rate) {
	case LC3_CONFIG_FREQ_48KHZ:
		info->info.raw.rate = 48000U;
		break;
	case LC3_CONFIG_FREQ_24KHZ:
		info->info.raw.rate = 24000U;
		break;
	case LC3_CONFIG_FREQ_16KHZ:
		info->info.raw.rate = 16000U;
		break;
	case LC3_CONFIG_FREQ_8KHZ:
		info->info.raw.rate = 8000U;
		break;
	default:
		return -EINVAL;
	}

	res = channels_to_positions(conf.channels, info->info.raw.position);
	if (res == 0)
		return -EINVAL;
	info->info.raw.channels = res;

	switch (conf.frame_duration) {
	case LC3_CONFIG_DURATION_10:
	case LC3_CONFIG_DURATION_7_5:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int codec_get_qos(const struct media_codec *codec,
		const void *config, size_t config_size,
		const struct bap_endpoint_qos *endpoint_qos,
		struct bap_codec_qos *qos)
{
	bap_lc3_t conf;

	spa_zero(*qos);

	if (!parse_conf(&conf, config, config_size))
		return -EINVAL;

	qos->framing = false;
	if (endpoint_qos->phy & 0x2)
		qos->phy = 0x2;
	else if (endpoint_qos->phy & 0x1)
		qos->phy = 0x1;
	else
		qos->phy = 0x2;
	qos->sdu = conf.framelen * conf.n_blks * get_num_channels(conf.channels);
	qos->interval = (conf.frame_duration == LC3_CONFIG_DURATION_7_5 ? 7500 : 10000);
	qos->target_latency = BT_ISO_QOS_TARGET_LATENCY_RELIABILITY;

	/* Default values from BAP v1.0.1 Table 5.2; high-reliability */
	qos->delay = 40000U;
	qos->retransmission = 13;

	switch (conf.rate) {
	case LC3_CONFIG_FREQ_8KHZ:
	case LC3_CONFIG_FREQ_16KHZ:
	case LC3_CONFIG_FREQ_24KHZ:
	case LC3_CONFIG_FREQ_32KHZ:
		/* F_1_2, F_2_2 */
		qos->latency = (conf.frame_duration == LC3_CONFIG_DURATION_7_5 ? 75 : 95);
		break;
	case LC3_CONFIG_FREQ_48KHZ:
		/* 48_5_2, 48_4_2 */
		qos->latency = (conf.frame_duration == LC3_CONFIG_DURATION_7_5 ? 75 : 100);
		break;
	default:
		qos->latency = 100;
		break;
	}

	/* Clamp to ASE values (if known) */
	if (endpoint_qos->latency >= 0x0005 && endpoint_qos->latency <= 0x0FA0)
		/* Values outside the range are RFU */
		qos->latency = endpoint_qos->latency;
	if (endpoint_qos->retransmission)
		qos->retransmission = endpoint_qos->retransmission;
	if (endpoint_qos->delay_min)
		qos->delay = SPA_MAX(qos->delay, endpoint_qos->delay_min);
	if (endpoint_qos->delay_max)
		qos->delay = SPA_MIN(qos->delay, endpoint_qos->delay_max);

	return 0;
}

static void *codec_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	bap_lc3_t conf;
	struct impl *this = NULL;
	struct spa_audio_info config_info;
	int res, ich;

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

	if (!parse_conf(&conf, config, config_len)) {
		res = -ENOTSUP;
		goto error;
	}

	this->mtu = mtu;
	this->samplerate = config_info.info.raw.rate;
	this->channels = config_info.info.raw.channels;
	this->framelen = conf.framelen;

	switch (conf.frame_duration) {
	case LC3_CONFIG_DURATION_10:
		this->frame_dus = 10000;
		break;
	case LC3_CONFIG_DURATION_7_5:
		this->frame_dus = 7500;
		break;
	default:
		res = -EINVAL;
		goto error;
	}

	this->samples = lc3_frame_samples(this->frame_dus, this->samplerate);
	if (this->samples < 0) {
		res = -EINVAL;
		goto error;
	}
	this->codesize = this->samples * this->channels * conf.n_blks * sizeof(int32_t);

	if (!(flags & MEDIA_CODEC_FLAG_SINK)) {
		for (ich = 0; ich < this->channels; ich++) {
			this->enc[ich] = lc3_setup_encoder(this->frame_dus, this->samplerate, 0, calloc(1, lc3_encoder_size(this->frame_dus, this->samplerate)));
			if (this->enc[ich] == NULL) {
				res = -EINVAL;
				goto error;
			}
		}
	} else {
		for (ich = 0; ich < this->channels; ich++) {
			this->dec[ich] = lc3_setup_decoder(this->frame_dus, this->samplerate, 0, calloc(1, lc3_decoder_size(this->frame_dus, this->samplerate)));
			if (this->dec[ich] == NULL) {
				res = -EINVAL;
				goto error;
			}
		}
	}

	return this;

error_errno:
	res = -errno;
	goto error;

error:
	if (this) {
		for (ich = 0; ich < this->channels; ich++) {
			if (this->enc[ich])
				free(this->enc[ich]);
			if (this->dec[ich])
				free(this->dec[ich]);
		}
	}
	free(this);
	errno = -res;
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;
	int ich;

	for (ich = 0; ich < this->channels; ich++) {
		if (this->enc[ich])
			free(this->enc[ich]);
		if (this->dec[ich])
			free(this->dec[ich]);
	}
	free(this);
}

static int codec_get_block_size(void *data)
{
	struct impl *this = data;
	return this->codesize;
}

static int codec_abr_process (void *data, size_t unsent)
{
	return -ENOTSUP;
}

static int codec_start_encode (void *data,
		void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp)
{
	return 0;
}

static int codec_encode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out, int *need_flush)
{
	struct impl *this = data;
	int frame_bytes;
	int ich, res;
	int size, processed;

	frame_bytes = lc3_frame_bytes(this->frame_dus, this->samplerate);
	processed = 0;
	size = 0;

	if (src_size < (size_t)this->codesize)
		goto done;
	if (dst_size < (size_t)frame_bytes)
		goto done;

	for (ich = 0; ich < this->channels; ich++) {
		uint8_t *in = (uint8_t *)src + (ich * 4);
		uint8_t *out = (uint8_t *)dst + ich * this->framelen;
		res = lc3_encode(this->enc[ich], LC3_PCM_FORMAT_S24, in, this->channels, this->framelen, out);
		size += this->framelen;
		if (SPA_UNLIKELY(res != 0))
			return -EINVAL;
	}
	*dst_out = size;

	processed += this->codesize;

done:
	spa_assert(size <= this->mtu);
	*need_flush = NEED_FLUSH_ALL;

	return processed;
}

static SPA_UNUSED int codec_start_decode (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	return 0;
}

static SPA_UNUSED int codec_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct impl *this = data;
	int ich, res;
	int consumed;
	int samples;

	spa_return_val_if_fail((size_t)(this->framelen * this->channels) == src_size, -EINVAL);
	consumed = 0;

	samples = lc3_frame_samples(this->frame_dus, this->samplerate);
	if (samples == -1)
		return -EINVAL;
	if (dst_size < this->codesize)
		return -EINVAL;

	for (ich = 0; ich < this->channels; ich++) {
		uint8_t *in = (uint8_t *)src + ich * this->framelen;
		uint8_t *out = (uint8_t *)dst + (ich * 4);
		res = lc3_decode(this->dec[ich], in, this->framelen, LC3_PCM_FORMAT_S24, out, this->channels);
		if (SPA_UNLIKELY(res < 0))
			return -EINVAL;
		consumed += this->framelen;
	}

	*dst_out = this->codesize;

	return consumed;
}

static int codec_reduce_bitpool(void *data)
{
	return -ENOTSUP;
}

static int codec_increase_bitpool(void *data)
{
	return -ENOTSUP;
}

static void codec_set_log(struct spa_log *global_log)
{
	log = global_log;
	spa_log_topic_init(log, &log_topic);
}

const struct media_codec bap_codec_lc3 = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_LC3,
	.name = "lc3",
	.codec_id = BAP_CODEC_LC3,
	.bap = true,
	.description = "LC3",
	.fill_caps = codec_fill_caps,
	.select_config = codec_select_config,
	.enum_config = codec_enum_config,
	.validate_config = codec_validate_config,
	.get_qos = codec_get_qos,
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
	.increase_bitpool = codec_increase_bitpool,
	.set_log = codec_set_log,
};

MEDIA_CODEC_EXPORT_DEF(
	"lc3",
	&bap_codec_lc3
);
