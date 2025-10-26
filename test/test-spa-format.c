/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2025 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/format.h>

#include "pwtest.h"

PWTEST(audio_format_sizes)
{
	union {
		uint8_t buf[1024];
		struct spa_audio_info align;
	} data;
	struct spa_audio_info info;
	size_t i;

	memset(&info, 0xf3, sizeof(info));
	info.media_type = SPA_MEDIA_TYPE_audio;
	info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info.info.raw.channels = 5;
	info.info.raw.format = SPA_AUDIO_FORMAT_F32P;
	info.info.raw.rate = 12345;
	info.info.raw.flags = 0;
	info.info.raw.position[0] = 1;
	info.info.raw.position[1] = 2;
	info.info.raw.position[2] = 3;
	info.info.raw.position[3] = 4;
	info.info.raw.position[4] = 5;

	for (i = 0; i < sizeof(data.buf); ++i) {
		struct spa_pod *pod;
		uint8_t buf[4096];
		struct spa_pod_builder b;

		spa_pod_builder_init(&b, buf, sizeof(buf));
		memcpy(data.buf, &info, sizeof(info));

		pod = spa_format_audio_ext_build(&b, 123, (void *)data.buf, i);
		if (i < offsetof(struct spa_audio_info, info.raw)
				+ offsetof(struct spa_audio_info_raw, position))
			pwtest_bool_true(!pod);
		else
			pwtest_bool_true(pod);
	}

	for (i = 0; i < sizeof(data.buf); ++i) {
		struct spa_pod *pod;
		uint8_t buf[4096];
		struct spa_pod_builder b;
		int ret;

		spa_pod_builder_init(&b, buf, sizeof(buf));
		pod = spa_format_audio_ext_build(&b, 123, &info, sizeof(info));
		pwtest_bool_true(pod);

		memset(data.buf, 0xf3, sizeof(data.buf));

		ret = spa_format_audio_ext_parse(pod, (void *)data.buf, i);
		if (i < offsetof(struct spa_audio_info, info.raw)
				+ offsetof(struct spa_audio_info_raw, position)
				+ info.info.raw.channels*sizeof(uint32_t)) {
			for (size_t j = i; j < sizeof(data.buf); ++j)
				pwtest_int_eq(data.buf[j], 0xf3);
			pwtest_int_lt(ret, 0);
		} else {
			pwtest_int_ge(ret, 0);
			pwtest_bool_true(memcmp(data.buf, &info, SPA_MIN(i, sizeof(info))) == 0);
		}
	}

	memset(&info, 0xf3, sizeof(info));
	info.media_type = SPA_MEDIA_TYPE_audio;
	info.media_subtype = SPA_MEDIA_SUBTYPE_aac;
	info.info.aac.rate = 12345;
	info.info.aac.channels = 6;
	info.info.aac.bitrate = 54321;
	info.info.aac.stream_format = SPA_AUDIO_AAC_STREAM_FORMAT_MP4LATM;

	for (i = 0; i < sizeof(data.buf); ++i) {
		struct spa_pod *pod;
		uint8_t buf[4096];
		struct spa_pod_builder b;

		spa_pod_builder_init(&b, buf, sizeof(buf));
		memcpy(data.buf, &info, sizeof(info));

		pod = spa_format_audio_ext_build(&b, 123, (void *)data.buf, i);
		if (i < offsetof(struct spa_audio_info, info.raw)
				+ sizeof(struct spa_audio_info_aac))
			pwtest_bool_true(!pod);
		else
			pwtest_bool_true(pod);
	}

	for (i = 0; i < sizeof(data.buf); ++i) {
		struct spa_pod *pod;
		uint8_t buf[4096];
		struct spa_pod_builder b;
		int ret;

		spa_pod_builder_init(&b, buf, sizeof(buf));
		pod = spa_format_audio_ext_build(&b, 123, &info, sizeof(info));
		pwtest_bool_true(pod);

		memset(data.buf, 0xf3, sizeof(data.buf));

		ret = spa_format_audio_ext_parse(pod, (void *)data.buf, i);
		if (i < offsetof(struct spa_audio_info, info.raw)
				+ sizeof(struct spa_audio_info_aac)) {
			for (size_t j = i; j < sizeof(data.buf); ++j)
				pwtest_int_eq(data.buf[j], 0xf3);
			pwtest_int_lt(ret, 0);
		} else {
			pwtest_int_ge(ret, 0);
			pwtest_bool_true(memcmp(data.buf, &info, SPA_MIN(i, sizeof(info))) == 0);
		}
	}

	return PWTEST_PASS;
}

PWTEST_SUITE(spa_format)
{
	pwtest_add(audio_format_sizes, PWTEST_NOARG);

	return PWTEST_PASS;
}
