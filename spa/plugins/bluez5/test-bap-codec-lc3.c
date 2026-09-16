/* SPDX-License-Identifier: MIT */

#include <spa/utils/defs.h>

#include "media-codecs.h"

extern const struct media_codec bap_codec_lc3;

static void test_pac_metadata(void)
{
	/* Two ordinary mono PAC records, separated as in BlueZ's capabilities:
	 * 16 kHz and 48 kHz, both with 7.5 ms and 10 ms frames. */
	static const uint8_t caps[] = {
		3, LC3_TYPE_FREQ, 0x04, 0x00,
		2, LC3_TYPE_DUR, 0x03,
		2, LC3_TYPE_CHAN, 0x01,
		5, LC3_TYPE_FRAMELEN, 30, 0, 40, 0,
		0,
		3, LC3_TYPE_FREQ, 0x80, 0x00,
		2, LC3_TYPE_DUR, 0x03,
		2, LC3_TYPE_CHAN, 0x01,
		5, LC3_TYPE_FRAMELEN, 75, 0, 155, 0,
	};
	static const uint8_t metadata[] = {
		3, BAP_META_TYPE_PREFERRED_CONTEXT, BAP_CONTEXT_MEDIA, 0,
		0,
		3, BAP_META_TYPE_PREFERRED_CONTEXT, BAP_CONTEXT_CONVERSATIONAL, 0,
	};
	static const struct {
		const char *sink;
		bool metadata;
		bool length;
		uint32_t rate;
	} cases[] = {
		{ "false", false, false, 48000 },
		{ "false", true, false, 48000 },
		{ "false", true, true, 16000 },
		{ "true", true, true, 48000 },
	};
	const struct media_codec *codec = &bap_codec_lc3;
	struct media_codec_audio_info info = { .rate = 48000, .channels = 1 };
	uint8_t config[A2DP_MAX_CAPS_SIZE];
	struct spa_audio_info audio;
	void *config_data;
	int size;

	SPA_FOR_EACH_ELEMENT_VAR(cases, c) {
		struct spa_dict_item items[4];
		uint32_t n = 0;
		items[n++] = SPA_DICT_ITEM_INIT("bluez5.bap.sink", c->sink);
		items[n++] = SPA_DICT_ITEM_INIT("bluez5.bap.available-context", "6");
		if (c->metadata)
			items[n++] = SPA_DICT_ITEM_INIT("bluez5.bap.metadata", (const char *)metadata);
		if (c->length)
			items[n++] = SPA_DICT_ITEM_INIT("bluez5.bap.metadata-len", "9");
		struct spa_dict settings = SPA_DICT_INIT(items, n);

		config_data = NULL;
		size = codec->select_config(codec, 0, caps, sizeof(caps), &info,
				&settings, config, &config_data);
		spa_assert_se(size > 0);
		spa_assert_se(config_data != NULL);
		spa_assert_se(codec->validate_config(codec, 0, config, size, &audio) == 0);
		spa_assert_se(audio.info.raw.rate == c->rate);
		codec->free_config_data(codec, config_data);
	}
}

int main(void)
{
	test_pac_metadata();
	return 0;
}
