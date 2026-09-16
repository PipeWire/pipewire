/* SPDX-License-Identifier: MIT */
/* Patched-source functional checks only; no Bluetooth or audio I/O. */
#include <stdio.h>
#ifndef CODEC_PLUGIN
#define CODEC_PLUGIN 1
#endif
#include "bap-codec-lc3.c"

static const uint8_t valid_caps[] = {
	3, LC3_TYPE_FREQ, 0x80, 0x00,
	2, LC3_TYPE_DUR, 0x03,
	2, LC3_TYPE_CHAN, 0x01,
	5, LC3_TYPE_FRAMELEN, 75, 0, 155, 0,
};

static void check_debug_defaults(void)
{
	struct settings settings;
	struct spa_debug_log_ctx debug_ctx = { 0 };

	parse_settings(&settings, NULL, &debug_ctx);
	spa_assert_se(debug_ctx.ctx.log == spa_debug_log_log);
	spa_assert_se(debug_ctx.log == NULL);
	spa_assert_se(debug_ctx.level == SPA_LOG_LEVEL_TRACE);
	spa_assert_se(settings.retransmission == -1);
	spa_assert_se(settings.latency == -1);
	spa_assert_se(settings.delay == -1);
	spa_assert_se(settings.framing == -1);
	spa_assert_se(settings.qos_name == NULL);
	spa_assert_se(settings.force_target_latency == NULL);
	/* Exercise the initialized no-log context rather than only inspecting it. */
	spa_debugc(&debug_ctx.ctx, "optional settings absent");
	puts("PASS NULL settings initialize defaults and a safe debug context");
}

static void check_null_empty_equivalence(void)
{
	const struct media_codec *codec = &bap_codec_lc3;
	struct media_codec_audio_info info = { .rate = 48000, .channels = 1 };
	struct spa_dict empty = SPA_DICT_INIT(NULL, 0);
	const struct spa_dict *settings[] = { NULL, &empty };
	uint8_t config[2][A2DP_MAX_CAPS_SIZE], metadata[2][8];
	struct spa_audio_info audio[2];
	struct bap_endpoint_qos endpoint = { 0 };
	struct bap_codec_qos qos[2] = { 0 };
	void *data[2] = { NULL, NULL };
	int sizes[2], metadata_sizes[2];

	for (size_t i = 0; i < SPA_N_ELEMENTS(settings); i++) {
		sizes[i] = codec->select_config(codec, 0, valid_caps,
				sizeof(valid_caps), &info, settings[i], config[i], &data[i]);
		spa_assert_se(sizes[i] > 0);
		spa_assert_se(data[i] != NULL);
		spa_assert_se(codec->validate_config(codec, 0, config[i], sizes[i],
				&audio[i]) == 0);
		spa_assert_se(audio[i].info.raw.rate == 48000);
		spa_assert_se(audio[i].info.raw.channels == 1);
		spa_assert_se(codec->get_qos(codec, &endpoint, data[i], &qos[i]) == 0);
		metadata_sizes[i] = codec->get_metadata(codec, data[i],
				metadata[i], sizeof(metadata[i]));
		spa_assert_se(metadata_sizes[i] > 0);
		/* Exercise the capability-check entry point from the crash stack. */
		spa_assert_se(media_codec_check_caps(codec, codec->codec_id, valid_caps,
				sizeof(valid_caps), &info, settings[i]));
	}
	spa_assert_se(sizes[0] == sizes[1]);
	spa_assert_se(!memcmp(config[0], config[1], sizes[0]));
	spa_assert_se(qos[0].interval == qos[1].interval);
	spa_assert_se(qos[0].sdu == qos[1].sdu);
	spa_assert_se(qos[0].latency == qos[1].latency);
	spa_assert_se(qos[0].retransmission == qos[1].retransmission);
	spa_assert_se(metadata_sizes[0] == metadata_sizes[1]);
	spa_assert_se(!memcmp(metadata[0], metadata[1], metadata_sizes[0]));
	for (size_t i = 0; i < SPA_N_ELEMENTS(data); i++)
		codec->free_config_data(codec, data[i]);
	puts("PASS NULL and empty settings select equivalent valid config, QoS and metadata");
}

static void check_rejected_caps(void)
{
	/* Incomplete LTV: ordinary parser error-path fixture, never transmitted. */
	static const uint8_t incomplete[] = { 2 };
	const struct media_codec *codec = &bap_codec_lc3;
	struct media_codec_audio_info info = { .rate = 48000, .channels = 1 };
	struct spa_dict empty = SPA_DICT_INIT(NULL, 0);
	const struct spa_dict *settings[] = { NULL, &empty };
	uint8_t config[A2DP_MAX_CAPS_SIZE];

	for (size_t i = 0; i < SPA_N_ELEMENTS(settings); i++) {
		void *data = NULL;
		int res = codec->select_config(codec, 0, incomplete,
				sizeof(incomplete), &info, settings[i], config, &data);
		spa_assert_se(res == -EINVAL);
		spa_assert_se(data == NULL);
		spa_assert_se(!media_codec_check_caps(codec, codec->codec_id, incomplete,
				sizeof(incomplete), &info, settings[i]));
		res = codec->select_config(codec, 0, NULL, 0, &info,
				settings[i], config, &data);
		spa_assert_se(res == -EINVAL);
		spa_assert_se(data == NULL);
	}
	puts("PASS rejected PACs return errors with NULL and empty settings");
}

int main(void)
{
	check_debug_defaults();
	check_null_empty_equivalence();
	check_rejected_caps();
	return 0;
}
