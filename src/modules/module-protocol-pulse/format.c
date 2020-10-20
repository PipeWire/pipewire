/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

enum sample_format {
	SAMPLE_U8,
	SAMPLE_ALAW,
	SAMPLE_ULAW,
	SAMPLE_S16LE,
	SAMPLE_S16BE,
	SAMPLE_FLOAT32LE,
	SAMPLE_FLOAT32BE,
	SAMPLE_S32LE,
	SAMPLE_S32BE,
	SAMPLE_S24LE,
	SAMPLE_S24BE,
	SAMPLE_S24_32LE,
	SAMPLE_S24_32BE,
	SAMPLE_MAX,
	SAMPLE_INVALID = -1
};

struct format {
	uint32_t format;
	const char *name;
	uint32_t size;
};

static const struct format audio_formats[] = {
	[SAMPLE_U8] = { SPA_AUDIO_FORMAT_U8, "u8", 1 },
	[SAMPLE_ALAW] = { SPA_AUDIO_FORMAT_UNKNOWN, "alaw", 1 },
	[SAMPLE_ULAW] = { SPA_AUDIO_FORMAT_UNKNOWN, "ulaw", 1 },
	[SAMPLE_S16LE] = { SPA_AUDIO_FORMAT_S16_LE, "s16le", 2 },
	[SAMPLE_S16BE] = { SPA_AUDIO_FORMAT_S16_BE, "s16be", 2 },
	[SAMPLE_FLOAT32LE] = { SPA_AUDIO_FORMAT_F32_LE, "f32le", 4 },
	[SAMPLE_FLOAT32BE] = { SPA_AUDIO_FORMAT_F32_BE, "f32be", 4 },
	[SAMPLE_S32LE] = { SPA_AUDIO_FORMAT_S32_LE, "s32le", 4 },
	[SAMPLE_S32BE] = { SPA_AUDIO_FORMAT_S32_BE, "s32be", 4 },
	[SAMPLE_S24LE] = { SPA_AUDIO_FORMAT_S24_LE, "s24le", 3 },
	[SAMPLE_S24BE] = { SPA_AUDIO_FORMAT_S24_BE, "s24be", 3 },
	[SAMPLE_S24_32LE] = { SPA_AUDIO_FORMAT_S24_32_LE, "s24_32le", 4 },
	[SAMPLE_S24_32BE] = { SPA_AUDIO_FORMAT_S24_32_BE, "s24_32be", 4 },
};

static inline uint32_t format_pa2id(enum sample_format format)
{
	if (format < 0 || (size_t)format >= SPA_N_ELEMENTS(audio_formats))
		return SPA_AUDIO_FORMAT_UNKNOWN;
	return audio_formats[format].format;
}

static inline enum sample_format format_id2pa(uint32_t id)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(audio_formats); i++) {
		if (id == audio_formats[i].format)
			return i;
	}
	return SAMPLE_INVALID;
}


struct sample_spec {
	enum sample_format format;
	uint32_t rate;
	uint8_t channels;
};

static inline uint32_t sample_spec_frame_size(const struct sample_spec *ss)
{
	if (ss->format < 0 || (size_t)ss->format >= SPA_N_ELEMENTS(audio_formats))
		return 0;
	return audio_formats[ss->format].size * ss->channels;
}

#define CHANNELS_MAX	64

struct channel_map {
	uint8_t channels;
	uint32_t map[CHANNELS_MAX];
};

struct cvolume {
	uint8_t channels;
	float values[CHANNELS_MAX];
};

enum encoding {
	ENCODING_ANY,
	ENCODING_PCM,
	ENCODING_AC3_IEC61937,
	ENCODING_EAC3_IEC61937,
	ENCODING_MPEG_IEC61937,
	ENCODING_DTS_IEC61937,
	ENCODING_MPEG2_AAC_IEC61937,
	ENCODING_TRUEHD_IEC61937,
	ENCODING_DTSHD_IEC61937,
	ENCODING_MAX,
	NCODING_INVALID = -1,
};

struct format_info {
	enum encoding encoding;
	struct pw_properties *props;
};

static int format_parse_param(const struct spa_pod *param, struct sample_spec *ss, struct channel_map *map)
{
	struct spa_audio_info info = { 0 };
//	uint32_t i;

        spa_format_parse(param, &info.media_type, &info.media_subtype);

	if (info.media_type != SPA_MEDIA_TYPE_audio ||
	    info.media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    spa_format_audio_raw_parse(param, &info.info.raw) < 0 ||
	    !SPA_AUDIO_FORMAT_IS_INTERLEAVED(info.info.raw.format)) {
                return -ENOTSUP;
        }

        ss->format = format_id2pa(info.info.raw.format);
        if (ss->format == SAMPLE_INVALID)
                return -ENOTSUP;

        ss->rate = info.info.raw.rate;
        ss->channels = info.info.raw.channels;

	map->channels = info.info.raw.channels;
//	for (i = 0; i < map->channels; i++)
//		map->map[i] = info.info.raw.position[i];

	return 0;
}
