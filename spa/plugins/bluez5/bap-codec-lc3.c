/* Spa BAP LC3 codec */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2022 Pauli Virtanen */
/* SPDX-FileCopyrightText: Copyright © 2022 Collabora */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <errno.h>
#include <limits.h>
#include <arpa/inet.h>
#include <bluetooth/bluetooth.h>

#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/utils/cleanup.h>
#include <spa/debug/log.h>

#include <lc3.h>

#include "media-codecs.h"
#include "bap-codec-caps.h"

#define MAX_PACS	64

static struct spa_log *log_;

struct impl {
	lc3_encoder_t enc[LC3_MAX_CHANNELS];
	lc3_decoder_t dec[LC3_MAX_CHANNELS];

	int samplerate;
	int codec_samplerate;
	int channels;
	int frame_dus;
	int framelen;
	int samples;
	unsigned int codesize;

	uint16_t seqnum;
};

struct settings {
	uint32_t locations;
	uint32_t channel_allocation;
	uint16_t supported_context;
	uint16_t available_context;
	bool sink;
	bool duplex;
	char *qos_name;
	int retransmission;
	int latency;
	int64_t delay;
	int framing;
	char *force_target_latency;
};

struct pac_data {
	const void *data;
	size_t size;
	const void *metadata;
	size_t metadata_size;
	int index;
	const struct settings *settings;
};

struct bap_qos {
	char *name;
	uint8_t rate;
	uint8_t frame_duration;
	bool framing;
	uint16_t framelen;
	uint8_t retransmission;
	uint16_t latency;
	uint32_t delay;
	unsigned int priority;
	char *tag;
};

typedef struct {
	uint8_t rate;
	uint8_t frame_duration;
	uint32_t channels;
	uint16_t framelen;
	uint8_t n_blks;
	bool sink;
	bool duplex;
	uint16_t preferred_context;
	unsigned int priority;
} bap_lc3_t;

struct config_data {
	bap_lc3_t conf;
	int pac_index;
	struct settings settings;
};

#define BAP_QOS(name_, rate_, duration_, framing_, framelen_, rtn_, latency_, delay_, priority_, tag_) \
	((struct bap_qos){ .name = (name_), .rate = (rate_), .frame_duration = (duration_), .framing = (framing_), \
			 .framelen = (framelen_), .retransmission = (rtn_), .latency = (latency_),	\
			 .delay = (delay_), .priority = (priority_), .tag = (tag_) })

static const struct bap_qos bap_qos_configs[] = {
	/* Priority: low-latency > high-reliability, 7.5ms > 10ms,
	 * bigger frequency and sdu better */

	/* BAP v1.0.1 Table 5.2; low-latency */
	BAP_QOS("8_1_1",   LC3_CONFIG_FREQ_8KHZ,  LC3_CONFIG_DURATION_7_5, false,  26, 2,  8, 40000, 30, "low-latency"), /* 8_1_1 */
	BAP_QOS("8_2_1",   LC3_CONFIG_FREQ_8KHZ,  LC3_CONFIG_DURATION_10,  false,  30, 2, 10, 40000, 20, "low-latency"), /* 8_2_1 */
	BAP_QOS("16_1_1",  LC3_CONFIG_FREQ_16KHZ, LC3_CONFIG_DURATION_7_5, false,  30, 2,  8, 40000, 31, "low-latency"), /* 16_1_1 */
	BAP_QOS("16_2_1",  LC3_CONFIG_FREQ_16KHZ, LC3_CONFIG_DURATION_10,  false,  40, 2, 10, 40000, 21, "low-latency"), /* 16_2_1 (mandatory) */
	BAP_QOS("24_1_1",  LC3_CONFIG_FREQ_24KHZ, LC3_CONFIG_DURATION_7_5, false,  45, 2,  8, 40000, 32, "low-latency"), /* 24_1_1 */
	BAP_QOS("24_2_1",  LC3_CONFIG_FREQ_24KHZ, LC3_CONFIG_DURATION_10,  false,  60, 2, 10, 40000, 22, "low-latency"), /* 24_2_1 */
	BAP_QOS("32_1_1",  LC3_CONFIG_FREQ_32KHZ, LC3_CONFIG_DURATION_7_5, false,  60, 2,  8, 40000, 33, "low-latency"), /* 32_1_1 */
	BAP_QOS("32_2_1",  LC3_CONFIG_FREQ_32KHZ, LC3_CONFIG_DURATION_10,  false,  80, 2, 10, 40000, 23, "low-latency"), /* 32_2_1 */
	BAP_QOS("441_1_1", LC3_CONFIG_FREQ_44KHZ, LC3_CONFIG_DURATION_7_5,  true,  97, 5, 24, 40000, 34, "low-latency"), /* 441_1_1 */
	BAP_QOS("441_2_1", LC3_CONFIG_FREQ_44KHZ, LC3_CONFIG_DURATION_10,   true, 130, 5, 31, 40000, 24, "low-latency"), /* 441_2_1 */
	BAP_QOS("48_1_1",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_7_5, false,  75, 5, 15, 40000, 35, "low-latency"), /* 48_1_1 */
	BAP_QOS("48_2_1",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_10,  false, 100, 5, 20, 40000, 25, "low-latency"), /* 48_2_1 */
	BAP_QOS("48_3_1",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_7_5, false,  90, 5, 15, 40000, 36, "low-latency"), /* 48_3_1 */
	BAP_QOS("48_4_1",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_10,  false, 120, 5, 20, 40000, 26, "low-latency"), /* 48_4_1 */
	BAP_QOS("48_5_1",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_7_5, false, 117, 5, 15, 40000, 37, "low-latency"), /* 48_5_1 */
	BAP_QOS("48_6_1",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_10,  false, 155, 5, 20, 40000, 27, "low-latency"), /* 48_6_1 */

	/* BAP v1.0.1 Table 5.2; high-reliability */
	BAP_QOS("8_1_2",   LC3_CONFIG_FREQ_8KHZ,  LC3_CONFIG_DURATION_7_5, false,  26, 13,  75, 40000, 10, "high-reliability"), /* 8_1_2 */
	BAP_QOS("8_2_2",   LC3_CONFIG_FREQ_8KHZ,  LC3_CONFIG_DURATION_10,  false,  30, 13,  95, 40000,  0, "high-reliability"), /* 8_2_2 */
	BAP_QOS("16_1_2",  LC3_CONFIG_FREQ_16KHZ, LC3_CONFIG_DURATION_7_5, false,  30, 13,  75, 40000, 11, "high-reliability"), /* 16_1_2 */
	BAP_QOS("16_2_2",  LC3_CONFIG_FREQ_16KHZ, LC3_CONFIG_DURATION_10,  false,  40, 13,  95, 40000,  1, "high-reliability"), /* 16_2_2 */
	BAP_QOS("24_1_2",  LC3_CONFIG_FREQ_24KHZ, LC3_CONFIG_DURATION_7_5, false,  45, 13,  75, 40000, 12, "high-reliability"), /* 24_1_2 */
	BAP_QOS("24_2_2",  LC3_CONFIG_FREQ_24KHZ, LC3_CONFIG_DURATION_10,  false,  60, 13,  95, 40000,  2, "high-reliability"), /* 24_2_2 */
	BAP_QOS("32_1_2",  LC3_CONFIG_FREQ_32KHZ, LC3_CONFIG_DURATION_7_5, false,  60, 13,  75, 40000, 13, "high-reliability"), /* 32_1_2 */
	BAP_QOS("32_2_2",  LC3_CONFIG_FREQ_32KHZ, LC3_CONFIG_DURATION_10,  false,  80, 13,  95, 40000,  3, "high-reliability"), /* 32_2_2 */
	BAP_QOS("441_1_2", LC3_CONFIG_FREQ_44KHZ, LC3_CONFIG_DURATION_7_5,  true,  97, 13,  80, 40000, 54, "high-reliability"), /* 441_1_2 */
	BAP_QOS("441_2_2", LC3_CONFIG_FREQ_44KHZ, LC3_CONFIG_DURATION_10,   true, 130, 13,  85, 40000, 44, "high-reliability"), /* 441_2_2 */
	BAP_QOS("48_1_2",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_7_5, false,  75, 13,  75, 40000, 55, "high-reliability"), /* 48_1_2 */
	BAP_QOS("48_2_2",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_10,  false, 100, 13,  95, 40000, 45, "high-reliability"), /* 48_2_2 */
	BAP_QOS("48_3_2",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_7_5, false,  90, 13,  75, 40000, 56, "high-reliability"), /* 48_3_2 */
	BAP_QOS("48_4_2",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_10,  false, 120, 13, 100, 40000, 46, "high-reliability"), /* 48_4_2 */
	BAP_QOS("48_5_2",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_7_5, false, 117, 13,  75, 40000, 57, "high-reliability"), /* 48_5_2 */
	BAP_QOS("48_6_2",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_10,  false, 155, 13, 100, 40000, 47, "high-reliability"), /* 48_6_2 */
};

static const struct bap_qos bap_bcast_qos_configs[] = {
	/* Priority: low-latency > high-reliability, 7.5ms > 10ms,
	 * bigger frequency and sdu better */

	/* BAP v1.0.1 Table 6.4; low-latency */
	BAP_QOS("8_1_1",   LC3_CONFIG_FREQ_8KHZ,  LC3_CONFIG_DURATION_7_5, false,  26, 2,  8, 40000, 30, "low-latency"), /* 8_1_1 */
	BAP_QOS("8_2_1",   LC3_CONFIG_FREQ_8KHZ,  LC3_CONFIG_DURATION_10,  false,  30, 2, 10, 40000, 20, "low-latency"), /* 8_2_1 */
	BAP_QOS("16_1_1",  LC3_CONFIG_FREQ_16KHZ, LC3_CONFIG_DURATION_7_5, false,  30, 2,  8, 40000, 31, "low-latency"), /* 16_1_1 */
	BAP_QOS("16_2_1",  LC3_CONFIG_FREQ_16KHZ, LC3_CONFIG_DURATION_10,  false,  40, 2, 10, 40000, 21, "low-latency"), /* 16_2_1 (mandatory) */
	BAP_QOS("24_1_1",  LC3_CONFIG_FREQ_24KHZ, LC3_CONFIG_DURATION_7_5, false,  45, 2,  8, 40000, 32, "low-latency"), /* 24_1_1 */
	BAP_QOS("24_2_1",  LC3_CONFIG_FREQ_24KHZ, LC3_CONFIG_DURATION_10,  false,  60, 2, 10, 40000, 22, "low-latency"), /* 24_2_1 */
	BAP_QOS("32_1_1",  LC3_CONFIG_FREQ_32KHZ, LC3_CONFIG_DURATION_7_5, false,  60, 2,  8, 40000, 33, "low-latency"), /* 32_1_1 */
	BAP_QOS("32_2_1",  LC3_CONFIG_FREQ_32KHZ, LC3_CONFIG_DURATION_10,  false,  80, 2, 10, 40000, 23, "low-latency"), /* 32_2_1 */
	BAP_QOS("441_1_1", LC3_CONFIG_FREQ_44KHZ, LC3_CONFIG_DURATION_7_5, true,   97, 4, 24, 40000, 34, "low-latency"), /* 441_1_1 */
	BAP_QOS("441_2_1", LC3_CONFIG_FREQ_44KHZ, LC3_CONFIG_DURATION_10,  true,  130, 4, 31, 40000, 24, "low-latency"), /* 441_2_1 */
	BAP_QOS("48_1_1",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_7_5, false,  75, 4, 15, 40000, 35, "low-latency"), /* 48_1_1 */
	BAP_QOS("48_2_1",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_10,  false, 100, 4, 20, 40000, 25, "low-latency"), /* 48_2_1 */
	BAP_QOS("48_3_1",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_7_5, false,  90, 4, 15, 40000, 36, "low-latency"), /* 48_3_1 */
	BAP_QOS("48_4_1",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_10,  false, 120, 4, 20, 40000, 26, "low-latency"), /* 48_4_1 */
	BAP_QOS("48_5_1",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_7_5, false, 117, 4, 15, 40000, 37, "low-latency"), /* 48_5_1 */
	BAP_QOS("48_6_1",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_10,  false, 155, 4, 20, 40000, 27, "low-latency"), /* 48_6_1 */

	/* BAP v1.0.1 Table 6.4; high-reliability */
	BAP_QOS("8_1_2",   LC3_CONFIG_FREQ_8KHZ,  LC3_CONFIG_DURATION_7_5, false,  26, 4,  45, 40000, 10, "high-reliability"), /* 8_1_2 */
	BAP_QOS("8_2_2",   LC3_CONFIG_FREQ_8KHZ,  LC3_CONFIG_DURATION_10,  false,  30, 4,  60, 40000,  0, "high-reliability"), /* 8_2_2 */
	BAP_QOS("16_1_2",  LC3_CONFIG_FREQ_16KHZ, LC3_CONFIG_DURATION_7_5, false,  30, 4,  45, 40000, 11, "high-reliability"), /* 16_1_2 */
	BAP_QOS("16_2_2",  LC3_CONFIG_FREQ_16KHZ, LC3_CONFIG_DURATION_10,  false,  40, 4,  60, 40000,  1, "high-reliability"), /* 16_2_2 */
	BAP_QOS("24_1_2",  LC3_CONFIG_FREQ_24KHZ, LC3_CONFIG_DURATION_7_5, false,  45, 4,  45, 40000, 12, "high-reliability"), /* 24_1_2 */
	BAP_QOS("24_2_2",  LC3_CONFIG_FREQ_24KHZ, LC3_CONFIG_DURATION_10,  false,  60, 4,  60, 40000,  2, "high-reliability"), /* 24_2_2 */
	BAP_QOS("32_1_2",  LC3_CONFIG_FREQ_32KHZ, LC3_CONFIG_DURATION_7_5, false,  60, 4,  45, 40000, 13, "high-reliability"), /* 32_1_2 */
	BAP_QOS("32_2_2",  LC3_CONFIG_FREQ_32KHZ, LC3_CONFIG_DURATION_10,  false,  80, 4,  60, 40000,  3, "high-reliability"), /* 32_2_2 */
	BAP_QOS("441_1_2", LC3_CONFIG_FREQ_44KHZ, LC3_CONFIG_DURATION_7_5, true,   97, 4,  54, 40000, 14, "high-reliability"), /* 441_1_2 */
	BAP_QOS("441_2_2", LC3_CONFIG_FREQ_44KHZ, LC3_CONFIG_DURATION_10,  true,  130, 4,  60, 40000,  4, "high-reliability"), /* 441_2_2 */
	BAP_QOS("48_1_2",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_7_5, false,  75, 4,  50, 40000, 15, "high-reliability"), /* 48_1_2 */
	BAP_QOS("48_2_2",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_10,  false, 100, 4,  65, 40000,  5, "high-reliability"), /* 48_2_2 */
	BAP_QOS("48_3_2",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_7_5, false,  90, 4,  50, 40000, 16, "high-reliability"), /* 48_3_2 */
	BAP_QOS("48_4_2",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_10,  false, 120, 4,  65, 40000,  6, "high-reliability"), /* 48_4_2 */
	BAP_QOS("48_5_2",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_7_5, false, 117, 4,  50, 40000, 17, "high-reliability"), /* 48_5_2 */
	BAP_QOS("48_6_2",  LC3_CONFIG_FREQ_48KHZ, LC3_CONFIG_DURATION_10,  false, 155, 4,  65, 40000,  7, "high-reliability"), /* 48_6_2 */
};

static unsigned int get_rate_mask(uint8_t rate) {
	switch (rate) {
	case LC3_CONFIG_FREQ_8KHZ: return LC3_FREQ_8KHZ;
	case LC3_CONFIG_FREQ_16KHZ: return LC3_FREQ_16KHZ;
	case LC3_CONFIG_FREQ_24KHZ: return LC3_FREQ_24KHZ;
	case LC3_CONFIG_FREQ_32KHZ: return LC3_FREQ_32KHZ;
	case LC3_CONFIG_FREQ_44KHZ: return LC3_FREQ_44KHZ;
	case LC3_CONFIG_FREQ_48KHZ: return LC3_FREQ_48KHZ;
	}
	return 0;
}

static unsigned int get_duration_mask(uint8_t rate) {
	switch (rate) {
	case LC3_CONFIG_DURATION_7_5: return LC3_DUR_7_5;
	case LC3_CONFIG_DURATION_10: return LC3_DUR_10;
	}
	return 0;
}

static uint16_t parse_rates(const char *str)
{
	struct spa_json it;
	uint16_t rate_mask = 0;
	int value;

	if (spa_json_begin_array_relax(&it, str, strlen(str)) <= 0)
		return rate_mask;

	while (spa_json_get_int(&it, &value) > 0) {
		switch (value) {
		case LC3_VAL_FREQ_8KHZ:
			rate_mask |= LC3_FREQ_8KHZ;
			break;
		case LC3_VAL_FREQ_16KHZ:
			rate_mask |= LC3_FREQ_16KHZ;
			break;
		case LC3_VAL_FREQ_24KHZ:
			rate_mask |=  LC3_FREQ_24KHZ;
			break;
		case LC3_VAL_FREQ_32KHZ:
			rate_mask |=  LC3_FREQ_32KHZ;
			break;
		case LC3_VAL_FREQ_44KHZ:
			rate_mask |=  LC3_FREQ_44KHZ;
			break;
		case LC3_VAL_FREQ_48KHZ:
			rate_mask |=  LC3_FREQ_48KHZ;
			break;
		default:
			break;
		}
	}

	return rate_mask;
}

static uint8_t parse_durations(const char *str)
{
	struct spa_json it;
	uint8_t duration_mask = 0;
	float value;

	if (spa_json_begin_array_relax(&it, str, strlen(str)) <= 0)
		return duration_mask;

	while (spa_json_get_float(&it, &value) > 0) {
		if (value == (float)LC3_VAL_DUR_7_5)
			duration_mask |= LC3_DUR_7_5;
		else if (value == (float)LC3_VAL_DUR_10)
			duration_mask |= LC3_DUR_10;
	}

	return duration_mask;
}

static uint8_t parse_channel_counts(const char *str)
{
	struct spa_json it;
	uint8_t channel_counts = 0;
	int value;

	if (spa_json_begin_array_relax(&it, str, strlen(str)) <= 0)
		return channel_counts;

	while (spa_json_get_int(&it, &value) > 0) {
		switch (value) {
		case LC3_VAL_CHAN_1:
			channel_counts |= LC3_CHAN_1;
			break;
		case LC3_VAL_CHAN_2:
			channel_counts |= LC3_CHAN_2;
			break;
		case LC3_VAL_CHAN_3:
			channel_counts |= LC3_CHAN_3;
			break;
		case LC3_VAL_CHAN_4:
			channel_counts |= LC3_CHAN_4;
			break;
		case LC3_VAL_CHAN_5:
			channel_counts |= LC3_CHAN_5;
			break;
		case LC3_VAL_CHAN_6:
			channel_counts |= LC3_CHAN_6;
			break;
		case LC3_VAL_CHAN_7:
			channel_counts |= LC3_CHAN_7;
			break;
		case LC3_VAL_CHAN_8:
			channel_counts |= LC3_CHAN_8;
			break;
		default:
			break;
		}
	}

	return channel_counts;
}

static int codec_fill_caps(const struct media_codec *codec, uint32_t flags,
		const struct spa_dict *settings, uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	const char *str;
	uint16_t framelen[2];
	uint16_t rate_mask = LC3_FREQ_48KHZ | LC3_FREQ_44KHZ | LC3_FREQ_32KHZ | \
				LC3_FREQ_24KHZ | LC3_FREQ_16KHZ | LC3_FREQ_8KHZ;
	uint8_t duration_mask = LC3_DUR_ANY;
	uint8_t channel_counts = LC3_CHAN_1 | LC3_CHAN_2;
	uint16_t framelen_min = LC3_MIN_FRAME_BYTES;
	uint16_t framelen_max = LC3_MAX_FRAME_BYTES;
	uint8_t max_frames = 2;
	uint32_t value;
	struct ltv_writer writer = LTV_WRITER(caps, A2DP_MAX_CAPS_SIZE);

	if (settings && (str = spa_dict_lookup(settings, "bluez5.bap-server-capabilities.rates")))
		rate_mask = parse_rates(str);

	if (settings && (str = spa_dict_lookup(settings, "bluez5.bap-server-capabilities.durations")))
		duration_mask = parse_durations(str);

	if (settings && (str = spa_dict_lookup(settings, "bluez5.bap-server-capabilities.channels")))
		channel_counts = parse_channel_counts(str);

	if (settings && (str = spa_dict_lookup(settings, "bluez5.bap-server-capabilities.framelen_min")))
		if (spa_atou32(str, &value, 0))
			framelen_min = value;

	if (settings && (str = spa_dict_lookup(settings, "bluez5.bap-server-capabilities.framelen_max")))
		if (spa_atou32(str, &value, 0))
			framelen_max = value;

	if (settings && (str = spa_dict_lookup(settings, "bluez5.bap-server-capabilities.max_frames")))
		if (spa_atou32(str, &value, 0))
			max_frames = value;

	framelen[0] = htobs(framelen_min);
	framelen[1] = htobs(framelen_max);

	ltv_writer_uint16(&writer, LC3_TYPE_FREQ, rate_mask);
	ltv_writer_uint8(&writer, LC3_TYPE_DUR, duration_mask);
	ltv_writer_uint8(&writer, LC3_TYPE_CHAN, channel_counts);
	ltv_writer_data(&writer, LC3_TYPE_FRAMELEN, framelen, sizeof(framelen));
	/* XXX: we support only one frame block -> max 2 frames per SDU */
	if (max_frames > 2)
		max_frames = 2;

	ltv_writer_uint8(&writer, LC3_TYPE_BLKS, max_frames);

	return ltv_writer_end(&writer);
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

static int parse_bluez_pacs_data(const uint8_t *data, size_t data_size, struct pac_data pacs[MAX_PACS],
		struct spa_debug_context *debug_ctx)
{
	/*
	 * BlueZ capabilities for the same codec may contain multiple
	 * PACs separated by zero-length LTV (see BlueZ b907befc2d80)
	 */
	int pac = 0;

	pacs[pac] = (struct pac_data){ .data = data };

	while (data_size > 0) {
		struct ltv *ltv = (struct ltv *)data;

		if (ltv->len == 0) {
			/* delimiter */
			if (pac + 1 >= MAX_PACS)
				break;

			++pac;
			pacs[pac] = (struct pac_data){ .data = data + 1, .index = pac };
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

static int parse_bluez_pacs(const uint8_t *data, size_t data_size,
		const uint8_t *metadata, size_t metadata_size, struct pac_data pacs[MAX_PACS],
		struct spa_debug_context *debug_ctx)
{
	struct pac_data meta[MAX_PACS];
	int pac_count, meta_count;

	pac_count = parse_bluez_pacs_data(data, data_size, pacs, debug_ctx);
	if (pac_count < 0)
		return pac_count;

	meta_count = parse_bluez_pacs_data(metadata, metadata_size, meta, debug_ctx);
	if (meta_count == pac_count) {
		for (int i = 0; i < pac_count; ++i) {
			pacs[i].metadata = meta[i].data;
			pacs[i].metadata_size = meta[i].size;
		}
	}

	return pac_count;
}

static uint8_t get_channel_count(uint32_t channels)
{
	uint8_t num;

	channels &= BAP_CHANNEL_ALL;

	if (channels == 0)
		return 1;  /* MONO */

	for (num = 0; channels; channels >>= 1)
		if (channels & 0x1)
			++num;

	return num;
}

static bool supports_channel_count(uint8_t mask, uint8_t count)
{
	if (count == 0 || count > 8)
		return false;
	return mask & (1u << (count - 1));
}

static bool select_bap_qos(struct bap_qos *conf,
		const struct settings *s, unsigned int rate_mask,
		unsigned int duration_mask, uint16_t framelen_min,
		uint16_t framelen_max)
{
	bool found = false;
	conf->name = NULL;
	conf->priority = 0;

	SPA_FOR_EACH_ELEMENT_VAR(bap_qos_configs, cur_conf) {
		struct bap_qos c = *cur_conf;

		/* Check if custom QoS settings are configured. If so, we check if
		 * the configured settings are compatible with unicast server
		 */
		if (spa_streq(c.name, s->qos_name))
			c.priority = UINT_MAX;
		else if (c.priority < conf->priority)
			continue;

		if (s->force_target_latency && !spa_streq(s->force_target_latency, c.tag))
			continue;

		if (s->retransmission >= 0)
			c.retransmission = s->retransmission;
		if (s->latency >= 0)
			c.latency = s->latency;
		if (s->delay >= 0)
			c.delay = s->delay;
		if (s->framing >= 0)
			c.framing = s->framing;

		if (!(get_rate_mask(c.rate) & rate_mask))
			continue;
		if (!(get_duration_mask(c.frame_duration) & duration_mask))
			continue;
		if (c.framelen < framelen_min || c.framelen > framelen_max)
			continue;

		*conf = c;
		found = true;
	}

	return found;
}

static int select_channels(uint8_t channel_counts, uint32_t locations, uint32_t channel_allocation,
		uint32_t *allocation)
{
	unsigned int i, num;

	locations &= BAP_CHANNEL_ALL;

	if (!channel_counts)
		return -1;

	if (!locations) {
		*allocation = 0;  /* mono (omit Audio_Channel_Allocation) */
		return 0;
	}

	if (channel_allocation) {
		channel_allocation &= locations;

		/* sanity check channel allocation */
		while (!supports_channel_count(channel_counts, get_channel_count(channel_allocation))) {
			for (i = 32; i > 0; --i) {
				uint32_t mask = (1u << (i-1));
				if (channel_allocation & mask) {
					channel_allocation &= ~mask;
					break;
				}
			}
			if (i == 0)
				break;
		}

		*allocation = channel_allocation;
		return 0;
	}

	/* XXX: select some channels, but upper level should tell us what */
	if ((channel_counts & LC3_CHAN_2) && get_channel_count(locations) >= 2)
		num = 2;
	else if ((channel_counts & LC3_CHAN_1) && get_channel_count(locations) >= 1)
		num = 1;
	else
		return -1;

	*allocation = 0;
	for (i = 0; i < SPA_N_ELEMENTS(bap_channel_bits); ++i) {
		if (locations & bap_channel_bits[i].bit) {
			*allocation |= bap_channel_bits[i].bit;
			--num;
			if (num == 0)
				break;
		}
	}

	return 0;
}

static bool select_config(bap_lc3_t *conf, const struct pac_data *pac,	struct spa_debug_context *debug_ctx)
{
	const void *data = pac->data;
	size_t data_size = pac->size;
	uint16_t framelen_min = 0, framelen_max = 0;
	int max_frames = -1;
	uint8_t channel_counts = LC3_CHAN_1; /* Default: 1 channel (BAP v1.0.1 Sec 4.3.1) */
	uint8_t max_channels = 0;
	uint8_t duration_mask = 0;
	uint16_t rate_mask = 0;
	uint16_t preferred_context = 0;
	struct bap_qos bap_qos;
	unsigned int i;
	bool found = false;
	const struct ltv *ltv;

	if (!data_size)
		return false;
	memset(conf, 0, sizeof(*conf));

	conf->sink = pac->settings->sink;
	conf->duplex = pac->settings->duplex;

	/* XXX: we always use one frame block */
	conf->n_blks = 1;

	while ((ltv = ltv_next(&data, &data_size))) {
		switch (ltv->type) {
		case LC3_TYPE_FREQ:
			spa_return_val_if_fail(ltv->len == 3, false);
			rate_mask = ltv->value[0] + (ltv->value[1] << 8);
			break;
		case LC3_TYPE_DUR:
			spa_return_val_if_fail(ltv->len == 2, false);
			duration_mask = ltv->value[0];
			break;
		case LC3_TYPE_CHAN:
			spa_return_val_if_fail(ltv->len == 2, false);
			channel_counts = ltv->value[0];
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
	}
	if (data) {
		spa_debugc(debug_ctx, "invalid LTV data");
		return false;
	}

	data = pac->metadata;
	data_size = pac->metadata_size;
	while ((ltv = ltv_next(&data, &data_size))) {
		switch (ltv->type) {
		case BAP_META_TYPE_PREFERRED_CONTEXT:
			if (ltv->len != 3)
				break;
			preferred_context = ltv->value[0] + (ltv->value[1] << 8);
			break;
		}
	}
	if (data)
		spa_debugc(debug_ctx, "malformed metadata");

	for (i = 0; i < 8; ++i)
		if (channel_counts & (1u << i))
			max_channels = i + 1;

	/* Default: 1 frame per channel (BAP v1.0.1 Sec 4.3.1) */
	if (max_frames < 0)
		max_frames = max_channels;

	/*
	 * Workaround:
	 * Creative Zen Hybrid Pro sets Supported_Max_Codec_Frames_Per_SDU == 1
	 * but channels == 0x3, and the 2-channel audio stream works.
	 */
	if (max_frames < max_channels) {
		spa_debugc(debug_ctx, "workaround: fixing bad Supported_Max_Codec_Frames_Per_SDU: %u->%u",
				max_frames, max_channels);
		max_frames = max_channels;
	}

	if (select_channels(channel_counts, pac->settings->locations, pac->settings->channel_allocation, &conf->channels) < 0) {
		spa_debugc(debug_ctx, "invalid channel configuration: 0x%02x %u",
				channel_counts, max_frames);
		return false;
	}

	if (max_frames < get_channel_count(conf->channels)) {
		spa_debugc(debug_ctx, "invalid max frames per SDU: %u", max_frames);
		return false;
	}

	/*
	 * Select supported rate + frame length combination.
	 *
	 * Frame length is not limited by ISO MTU, as kernel will fragment
	 * and reassemble SDUs as needed.
	 */
	if (pac->settings->duplex) {
		/* 16KHz input is mandatory in BAP v1.0.1 Table 3.5, and 32KHz in TMAP,
		 * so prefer those for now for input rate in duplex configuration.
		 *
		 * It appears few devices support 48kHz out + input, so in duplex mode
		 * try 32 kHz or 16 kHz also for output direction.
		 *
		 * Devices may list other values but not certain they will work properly.
		 */
		found = select_bap_qos(&bap_qos, pac->settings, rate_mask & (LC3_FREQ_16KHZ | LC3_FREQ_32KHZ),
				duration_mask, framelen_min, framelen_max);
	}
	if (!found)
		found = select_bap_qos(&bap_qos, pac->settings, rate_mask, duration_mask, framelen_min, framelen_max);

	if (!found) {
		spa_debugc(debug_ctx, "no compatible configuration found, rate:0x%08x, duration:0x%08x frame:%u-%u",
				rate_mask, duration_mask, framelen_min, framelen_max);
		return false;
	}

	conf->rate = bap_qos.rate;
	conf->frame_duration = bap_qos.frame_duration;
	conf->framelen = bap_qos.framelen;
	conf->priority = bap_qos.priority;
	conf->preferred_context = preferred_context;

	return true;
}

static bool parse_conf(bap_lc3_t *conf, const void *data, size_t data_size)
{
	const struct ltv *ltv;

	if (!data_size)
		return false;
	memset(conf, 0, sizeof(*conf));

	conf->frame_duration = 0xFF;

	/* Absent Codec_Frame_Blocks_Per_SDU means 0x1 (BAP v1.0.1 Sec 4.3.2) */
	conf->n_blks = 1;

	while ((ltv = ltv_next(&data, &data_size))) {
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
	}
	if (data)
		return false;

	if (conf->frame_duration == 0xFF || !conf->rate)
		return false;

	return true;
}

static uint16_t get_wanted_context(const struct settings *settings)
{
	uint16_t context;

	/* Stick with contexts specified in TMAP. Anything else is probably crapshoot due
	 * to interesting device firmware behavior.
	 *
	 * Eg. some Earfun firmwares fail if we set MEDIA | CONVERSATIONAL, other versions
	 * disconnect if LIVE is set, Samsung Galaxy Buds fail on microphone Enable if
	 * CONVERSATIONAL is not set.
	 */
	if (settings->sink || settings->duplex)
		context = BAP_CONTEXT_CONVERSATIONAL;
	else
		context = BAP_CONTEXT_MEDIA;

	/* CAP v1.0.1 Sec 7.3.1.2.1: drop contexts if not available, otherwise unspecified */
	context &= settings->available_context;
	if (!context)
		context = BAP_CONTEXT_UNSPECIFIED & settings->available_context;

	return context;
}

static int conf_cmp(const bap_lc3_t *conf1, int res1, const bap_lc3_t *conf2, int res2,
		const struct settings *settings)
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

	PREFER_EXPR(conf->priority == UINT_MAX);

	/* CAP v1.0.1 Sec 7.3.1.2.4: should use PAC preferred context if possible */
	PREFER_BOOL(conf->preferred_context & get_wanted_context(settings));

	PREFER_BOOL(conf->channels & LC3_CHAN_2);
	PREFER_BOOL(conf->channels & LC3_CHAN_1);

	if (conf->duplex)
		PREFER_BOOL(conf->rate & (LC3_CONFIG_FREQ_16KHZ | LC3_CONFIG_FREQ_32KHZ));

	PREFER_EXPR(conf->priority);

	return 0;

#undef PREFER_EXPR
#undef PREFER_BOOL
}

static int pac_cmp(const void *p1, const void *p2)
{
	const struct pac_data *pac1 = p1;
	const struct pac_data *pac2 = p2;
	struct spa_debug_log_ctx debug_ctx = SPA_LOG_DEBUG_INIT(log_, SPA_LOG_LEVEL_TRACE);
	bap_lc3_t conf1, conf2;
	int res1, res2;

	res1 = select_config(&conf1, pac1, &debug_ctx.ctx) ? (int)sizeof(bap_lc3_t) : -EINVAL;
	res2 = select_config(&conf2, pac2, &debug_ctx.ctx) ? (int)sizeof(bap_lc3_t) : -EINVAL;

	return conf_cmp(&conf1, res1, &conf2, res2, pac1->settings);
}

static void parse_settings(struct settings *s, const struct spa_dict *settings,
		struct spa_debug_log_ctx *debug_ctx)
{
	const char *str;
	uint32_t value;

	spa_zero(*s);
	s->retransmission = -1;
	s->latency = -1;
	s->delay = -1;
	s->framing = -1;

	if (!settings)
		return;

	if ((str = spa_dict_lookup(settings, "bluez5.bap.preset")))
		s->qos_name = strdup(str);

	if ((str = spa_dict_lookup(settings, "bluez5.bap.force-target-latency")))
		s->force_target_latency = strdup(str);

	if (spa_atou32(spa_dict_lookup(settings, "bluez5.bap.rtn"), &value, 0))
		s->retransmission = value;

	if (spa_atou32(spa_dict_lookup(settings, "bluez5.bap.latency"), &value, 0))
		s->latency = value;

	if (spa_atou32(spa_dict_lookup(settings, "bluez5.bap.delay"), &value, 0))
		s->delay = value;

	if ((str = spa_dict_lookup(settings, "bluez5.bap.framing")))
		s->framing = spa_atob(str);

	if (spa_atou32(spa_dict_lookup(settings, "bluez5.bap.locations"), &value, 0))
		s->locations = value;

	if (spa_atou32(spa_dict_lookup(settings, "bluez5.bap.channel-allocation"), &value, 0))
		s->channel_allocation = value;

	if (spa_atou32(spa_dict_lookup(settings, "bluez5.bap.supported-context"), &value, 0))
		s->supported_context = value;

	if (spa_atou32(spa_dict_lookup(settings, "bluez5.bap.available-context"), &value, 0))
		s->available_context = value;

	if (spa_atob(spa_dict_lookup(settings, "bluez5.bap.debug")))
		*debug_ctx = SPA_LOG_DEBUG_INIT(log_, SPA_LOG_LEVEL_DEBUG);
	else
		*debug_ctx = SPA_LOG_DEBUG_INIT(NULL, SPA_LOG_LEVEL_TRACE);

	/* Is local endpoint sink or source */
	s->sink = spa_atob(spa_dict_lookup(settings, "bluez5.bap.sink"));

	/* Is remote endpoint duplex */
	s->duplex = spa_atob(spa_dict_lookup(settings, "bluez5.bap.duplex"));

	spa_debugc(&debug_ctx->ctx,
			"BAP LC3 settings: preset:%s rtn:%d latency:%d delay:%d framing:%d "
			"locations:%x chnalloc:%x sink:%d duplex:%d force-target-latency:%s",
			s->qos_name ? s->qos_name : "auto",
			s->retransmission, s->latency, (int)s->delay, s->framing,
			(unsigned int)s->locations, (unsigned int)s->channel_allocation,
			(int)s->sink, (int)s->duplex, s->force_target_latency);
}

static void free_config_data(struct config_data *d)
{
	if (!d)
		return;
	free(d->settings.qos_name);
	free(d->settings.force_target_latency);
	free(d);
}

SPA_DEFINE_AUTOPTR_CLEANUP(config_data, struct config_data, { spa_clear_ptr(*thing, free_config_data); });

static int codec_select_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size,
		const struct media_codec_audio_info *info,
		const struct spa_dict *settings, uint8_t config[A2DP_MAX_CAPS_SIZE],
		void **config_data)
{
	struct pac_data pacs[MAX_PACS];
	int npacs;
	bap_lc3_t conf;
	struct spa_debug_log_ctx debug_ctx;
	spa_autoptr(config_data) d = NULL;
	int i, ret;
	struct ltv_writer writer = LTV_WRITER(config, A2DP_MAX_CAPS_SIZE);
	const void *metadata = NULL;
	uint32_t metadata_len = 0;

	if (caps == NULL)
		return -EINVAL;

	d = calloc(1, sizeof(*d));
	if (!d)
		return -ENOMEM;

	parse_settings(&d->settings, settings, &debug_ctx);

	if (spa_atou32(spa_dict_lookup(settings, "bluez5.bap.metadata"), &metadata_len, 0))
		metadata = spa_dict_lookup(settings, "bluez5.bap.metadata");
	if (!metadata)
		metadata_len = 0;

	/* Select best conf from those possible */
	npacs = parse_bluez_pacs(caps, caps_size, metadata, metadata_len, pacs, &debug_ctx.ctx);
	if (npacs < 0) {
		spa_debugc(&debug_ctx.ctx, "malformed PACS");
		return npacs;
	} else if (npacs == 0) {
		spa_debugc(&debug_ctx.ctx, "no PACS");
		return -EINVAL;
	}

	for (i = 0; i < npacs; ++i)
		pacs[i].settings = &d->settings;

	qsort(pacs, npacs, sizeof(struct pac_data), pac_cmp);

	spa_debugc(&debug_ctx.ctx, "selected PAC %d", pacs[0].index);

	if (!select_config(&conf, &pacs[0], &debug_ctx.ctx))
		return -ENOTSUP;

	d->conf = conf;
	d->pac_index = pacs[0].index;

	ltv_writer_uint8(&writer, LC3_TYPE_FREQ, conf.rate);
	ltv_writer_uint8(&writer, LC3_TYPE_DUR, conf.frame_duration);

	/* Indicate MONO with absent Audio_Channel_Allocation (BAP v1.0.1 Sec. 4.3.2) */
	if (conf.channels != 0)
		ltv_writer_uint32(&writer, LC3_TYPE_CHAN, conf.channels);

	ltv_writer_uint16(&writer, LC3_TYPE_FRAMELEN, conf.framelen);
	ltv_writer_uint8(&writer, LC3_TYPE_BLKS, conf.n_blks);

	ret = ltv_writer_end(&writer);

	if (ret >= 0 && config_data)
		*config_data = spa_steal_ptr(d);

	return ret;
}

static int codec_caps_preference_cmp(const struct media_codec *codec, uint32_t flags, const void *caps1, size_t caps1_size,
		const void *caps2, size_t caps2_size, const struct media_codec_audio_info *info, const struct spa_dict *global_settings)
{
	bap_lc3_t conf1, conf2;
	int res1, res2, res;
	void *data1 = NULL;
	void *data2 = NULL;
	const struct config_data *d;

	/* Order selected configurations by preference */
	res1 = codec->select_config(codec, 0, caps1, caps1_size, info, global_settings, (uint8_t *)&conf1, &data1);
	res2 = codec->select_config(codec, 0, caps2, caps2_size, info, global_settings, (uint8_t *)&conf2, &data2);

	d = data1 ? data1 : data2;
	res = conf_cmp(&conf1, res1, &conf2, res2, d ? &d->settings : NULL);

	codec->free_config_data(codec, data1);
	codec->free_config_data(codec, data2);

	return res;
}

static uint8_t channels_to_positions(uint32_t channels, uint32_t *position, uint32_t max_position)
{
	uint32_t n_channels = get_channel_count(channels);
	uint8_t n_positions = 0;

	spa_assert(n_channels <= max_position);

	if (channels == 0) {
		position[0] = SPA_AUDIO_CHANNEL_MONO;
		n_positions = 1;
	} else {
		unsigned int i;

		for (i = 0; i < SPA_N_ELEMENTS(bap_channel_bits); ++i)
			if (channels & bap_channel_bits[i].bit)
				position[n_positions++] = bap_channel_bits[i].channel;
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
	uint32_t position[LC3_MAX_CHANNELS];
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
	if (conf.rate == LC3_CONFIG_FREQ_44KHZ) {
		if (i++ == 0)
			spa_pod_builder_int(b, 44100);
		spa_pod_builder_int(b, 44100);
	}
	if (conf.rate == LC3_CONFIG_FREQ_32KHZ) {
		if (i++ == 0)
			spa_pod_builder_int(b, 32000);
		spa_pod_builder_int(b, 32000);
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

	res = channels_to_positions(conf.channels, position, SPA_N_ELEMENTS(position));
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
	case LC3_CONFIG_FREQ_44KHZ:
		info->info.raw.rate = 44100U;
		break;
	case LC3_CONFIG_FREQ_32KHZ:
		info->info.raw.rate = 32000U;
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

	res = channels_to_positions(conf.channels, info->info.raw.position,
			SPA_N_ELEMENTS(info->info.raw.position));
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
		const struct bap_endpoint_qos *endpoint_qos,
		const void *config_data,
		struct bap_codec_qos *qos)
{
	struct bap_qos bap_qos;
	bap_lc3_t conf;
	bool found = false;
	const struct config_data *d = config_data;

	spa_zero(*qos);

	if (!d)
		return -EINVAL;

	conf = d->conf;

	found = select_bap_qos(&bap_qos, &d->settings, get_rate_mask(conf.rate), get_duration_mask(conf.frame_duration),
			conf.framelen, conf.framelen);
	if (!found) {
		/* shouldn't happen: select_config should pick existing one */
		spa_log_error(log_, "no QoS settings found");
		return -EINVAL;
	}

	if (endpoint_qos->framing == 0x01)
		qos->framing = true;
	else
		qos->framing = bap_qos.framing;

	if (endpoint_qos->phy & 0x2)
		qos->phy = 0x2;
	else if (endpoint_qos->phy & 0x1)
		qos->phy = 0x1;
	else
		qos->phy = 0x2;

	qos->sdu = conf.framelen * conf.n_blks * get_channel_count(conf.channels);
	qos->interval = (conf.frame_duration == LC3_CONFIG_DURATION_7_5 ? 7500 : 10000);
	qos->target_latency = BT_ISO_QOS_TARGET_LATENCY_BALANCED;

	qos->delay = bap_qos.delay;
	qos->latency = bap_qos.latency;
	qos->retransmission = bap_qos.retransmission;

	/* Clamp to ASE values (if known) */
	if (endpoint_qos->delay_min)
		qos->delay = SPA_MAX(qos->delay, endpoint_qos->delay_min);
	if (endpoint_qos->delay_max)
		qos->delay = SPA_MIN(qos->delay, endpoint_qos->delay_max);

	/*
	 * We ignore endpoint suggested latency and RTN. On current devices
	 * these do not appear to be very useful numbers, so it's better
	 * to just pick one from the table in the spec.
	 */

	return 0;
}

static int codec_get_metadata(const struct media_codec *codec, const void *config_data,
		uint8_t *meta, size_t meta_max_size)
{
	const struct config_data *d = config_data;
	struct ltv_writer writer = LTV_WRITER(meta, meta_max_size);
	uint16_t ctx;

	ctx = get_wanted_context(&d->settings);
	if (!ctx)
		ctx = BAP_CONTEXT_UNSPECIFIED;

	ltv_writer_uint16(&writer, BAP_META_TYPE_STREAMING_CONTEXT, ctx);

	return ltv_writer_end(&writer);
}

static void codec_free_config_data(const struct media_codec *codec, void *config_data)
{
	free_config_data(config_data);
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
		spa_log_error(log_, "invalid LC3 config");
		res = -ENOTSUP;
		goto error;
	}

	this->samplerate = config_info.info.raw.rate;
	this->channels = config_info.info.raw.channels;
	this->framelen = conf.framelen;

	/* Google liblc3 doesn't have direct support for encoding to 44.1kHz; instead
	 * lc3.h suggests using a nearby samplerate, so we do just that */
	this->codec_samplerate = (this->samplerate == 44100) ? 48000 : this->samplerate;

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

	spa_log_info(log_, "LC3 rate:%d frame_duration:%d channels:%d framelen:%d nblks:%d",
			this->samplerate, this->frame_dus, this->channels, this->framelen, conf.n_blks);

	res = lc3_frame_samples(this->frame_dus, this->codec_samplerate);
	if (res < 0) {
		spa_log_error(log_, "invalid LC3 frame samples");
		res = -EINVAL;
		goto error;
	}
	this->samples = res;
	this->codesize = (size_t)this->samples * this->channels * conf.n_blks * sizeof(int32_t);

	if (!(flags & MEDIA_CODEC_FLAG_SINK)) {
		for (ich = 0; ich < this->channels; ich++) {
			this->enc[ich] = lc3_setup_encoder(this->frame_dus, this->codec_samplerate, 0,
					calloc(1, lc3_encoder_size(this->frame_dus, this->codec_samplerate)));
			if (this->enc[ich] == NULL) {
				res = -EINVAL;
				goto error;
			}
		}
	} else {
		for (ich = 0; ich < this->channels; ich++) {
			this->dec[ich] = lc3_setup_decoder(this->frame_dus, this->codec_samplerate, 0,
					calloc(1, lc3_decoder_size(this->frame_dus, this->codec_samplerate)));
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

static uint64_t codec_get_interval(void *data)
{
	struct impl *this = data;

	return (uint64_t)this->samples * SPA_NSEC_PER_SEC / this->samplerate;
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
	int ich, res;
	int size, processed;

	processed = 0;
	size = 0;

	if (src_size < (size_t)this->codesize)
		return -EINVAL;
	if (dst_size < (size_t)this->framelen * this->channels)
		return -EINVAL;

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

	*need_flush = NEED_FLUSH_ALL;

	return processed;
}

static int codec_start_decode (void *data,
		const void *src, size_t src_size, uint16_t *seqnum, uint32_t *timestamp)
{
	struct impl *this = data;

	/* packets come from controller, so also invalid ones bump seqnum */
	this->seqnum++;

	if (!src_size)
		return -EINVAL;

	if (*seqnum)
		*seqnum = this->seqnum;
	return 0;
}

static int codec_decode(void *data,
		const void *src, size_t src_size,
		void *dst, size_t dst_size,
		size_t *dst_out)
{
	struct impl *this = data;
	int ich, res;
	int consumed;

	consumed = 0;

	if (src_size < (size_t)this->framelen * this->channels)
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

static int codec_produce_plc(void *data, void *dst, size_t dst_size)
{
	struct impl *this = data;
	int ich, res;

	if (dst_size < this->codesize)
		return -EINVAL;

	for (ich = 0; ich < this->channels; ich++) {
		uint8_t *out = (uint8_t *)dst + (ich * 4);
		res = lc3_decode(this->dec[ich], NULL, 0, LC3_PCM_FORMAT_S24, out, this->channels);
		if (SPA_UNLIKELY(res < 0))
			return -EINVAL;
	}

	return this->codesize;
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
	log_ = global_log;
	spa_log_topic_init(log_, &codec_plugin_log_topic);
}

static int codec_get_bis_config(const struct media_codec *codec, uint8_t *caps,
				uint8_t *caps_size, struct spa_dict *settings,
				struct bap_codec_qos *qos)
{
	const char *preset_name = NULL;
	int channel_allocation = 0;
	int i, ret;
	struct ltv_writer writer = LTV_WRITER(caps, *caps_size);
	const struct bap_qos *preset = NULL;

	*caps_size = 0;

	if (settings) {
		for (i = 0; i < (int)settings->n_items; ++i) {
			if (spa_streq(settings->items[i].key, "channel_allocation"))
				sscanf(settings->items[i].value, "%"PRIu32, &channel_allocation);
			if (spa_streq(settings->items[i].key, "preset"))
				preset_name = settings->items[i].value;
		}
	}

	if (preset_name == NULL)
		return -EINVAL;

	SPA_FOR_EACH_ELEMENT_VAR(bap_bcast_qos_configs, c) {
		if (spa_streq(c->name, preset_name)) {
			preset = c;
			break;
		}
	}

	if (!preset)
		return -EINVAL;

	ltv_writer_uint8(&writer, LC3_TYPE_FREQ, preset->rate);
	ltv_writer_uint16(&writer, LC3_TYPE_FRAMELEN, preset->framelen);
	ltv_writer_uint8(&writer, LC3_TYPE_DUR, preset->frame_duration);
	ltv_writer_uint32(&writer, LC3_TYPE_CHAN, channel_allocation);

	if (preset->framing)
		qos->framing = 1;
	else
		qos->framing = 0;
	qos->sdu = preset->framelen * get_channel_count(channel_allocation);
	qos->retransmission = preset->retransmission;
	qos->latency = preset->latency;
	qos->delay = preset->delay;
	qos->phy = 2;
	qos->interval = (preset->frame_duration == LC3_CONFIG_DURATION_7_5 ? 7500 : 10000);

	ret = ltv_writer_end(&writer);
	if (ret < 0)
		return ret;
	if (ret > UINT8_MAX)
		return -ENOSPC;

	*caps_size = ret;
	return 0;
}

const struct media_codec bap_codec_lc3 = {
	.id = SPA_BLUETOOTH_AUDIO_CODEC_LC3,
	.kind = MEDIA_CODEC_BAP,
	.name = "lc3",
	.codec_id = BAP_CODEC_LC3,
	.description = "LC3",
	.fill_caps = codec_fill_caps,
	.select_config = codec_select_config,
	.enum_config = codec_enum_config,
	.validate_config = codec_validate_config,
	.get_qos = codec_get_qos,
	.get_metadata = codec_get_metadata,
	.free_config_data = codec_free_config_data,
	.caps_preference_cmp = codec_caps_preference_cmp,
	.init = codec_init,
	.deinit = codec_deinit,
	.get_block_size = codec_get_block_size,
	.get_interval = codec_get_interval,
	.abr_process = codec_abr_process,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.start_decode = codec_start_decode,
	.decode = codec_decode,
	.produce_plc = codec_produce_plc,
	.reduce_bitpool = codec_reduce_bitpool,
	.increase_bitpool = codec_increase_bitpool,
	.set_log = codec_set_log,
	.get_bis_config = codec_get_bis_config
};

MEDIA_CODEC_EXPORT_DEF(
	"lc3",
	&bap_codec_lc3
);
