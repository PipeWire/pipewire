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

#define RATE_MAX	(48000u*8u)
#define CHANNELS_MAX	(64u)

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
	[SAMPLE_ALAW] = { SPA_AUDIO_FORMAT_UNKNOWN, "aLaw", 1 },
	[SAMPLE_ULAW] = { SPA_AUDIO_FORMAT_UNKNOWN, "uLaw", 1 },
	[SAMPLE_S16LE] = { SPA_AUDIO_FORMAT_S16_LE, "s16le", 2 },
	[SAMPLE_S16BE] = { SPA_AUDIO_FORMAT_S16_BE, "s16be", 2 },
	[SAMPLE_FLOAT32LE] = { SPA_AUDIO_FORMAT_F32_LE, "float32le", 4 },
	[SAMPLE_FLOAT32BE] = { SPA_AUDIO_FORMAT_F32_BE, "float32be", 4 },
	[SAMPLE_S32LE] = { SPA_AUDIO_FORMAT_S32_LE, "s32le", 4 },
	[SAMPLE_S32BE] = { SPA_AUDIO_FORMAT_S32_BE, "s32be", 4 },
	[SAMPLE_S24LE] = { SPA_AUDIO_FORMAT_S24_LE, "s24le", 3 },
	[SAMPLE_S24BE] = { SPA_AUDIO_FORMAT_S24_BE, "s24be", 3 },
	[SAMPLE_S24_32LE] = { SPA_AUDIO_FORMAT_S24_32_LE, "s24-32le", 4 },
	[SAMPLE_S24_32BE] = { SPA_AUDIO_FORMAT_S24_32_BE, "s24-32be", 4 },
};

static inline uint32_t format_pa2id(enum sample_format format)
{
	if (format < 0 || (size_t)format >= SPA_N_ELEMENTS(audio_formats))
		return SPA_AUDIO_FORMAT_UNKNOWN;
	return audio_formats[format].format;
}

static inline enum sample_format format_name2pa(const char *name, size_t size)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(audio_formats); i++) {
		if (strncmp(name, audio_formats[i].name, size) == 0)
			return i;
	}
	return SAMPLE_INVALID;
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
#define SAMPLE_SPEC_INIT	(struct sample_spec) {			\
					.format = SAMPLE_FLOAT32LE,	\
					.rate = 44100,			\
					.channels = 2,			\
				};

static inline uint32_t sample_spec_frame_size(const struct sample_spec *ss)
{
	if (ss->format < 0 || (size_t)ss->format >= SPA_N_ELEMENTS(audio_formats))
		return 0;
	return audio_formats[ss->format].size * ss->channels;
}

static inline bool sample_spec_valid(const struct sample_spec *ss)
{
	return (ss->format < SAMPLE_MAX &&
	    ss->rate > 0 && ss->rate <= RATE_MAX &&
	    ss->channels > 0 && ss->channels <= CHANNELS_MAX);
}

enum channel_position {
	CHANNEL_POSITION_INVALID = -1,
	CHANNEL_POSITION_MONO = 0,
	CHANNEL_POSITION_FRONT_LEFT,
	CHANNEL_POSITION_FRONT_RIGHT,
	CHANNEL_POSITION_FRONT_CENTER,

	CHANNEL_POSITION_REAR_CENTER,
	CHANNEL_POSITION_REAR_LEFT,
	CHANNEL_POSITION_REAR_RIGHT,

	CHANNEL_POSITION_LFE,
	CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
	CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,

	CHANNEL_POSITION_SIDE_LEFT,
	CHANNEL_POSITION_SIDE_RIGHT,
	CHANNEL_POSITION_AUX0,
	CHANNEL_POSITION_AUX1,
	CHANNEL_POSITION_AUX2,
	CHANNEL_POSITION_AUX3,
	CHANNEL_POSITION_AUX4,
	CHANNEL_POSITION_AUX5,
	CHANNEL_POSITION_AUX6,
	CHANNEL_POSITION_AUX7,
	CHANNEL_POSITION_AUX8,
	CHANNEL_POSITION_AUX9,
	CHANNEL_POSITION_AUX10,
	CHANNEL_POSITION_AUX11,
	CHANNEL_POSITION_AUX12,
	CHANNEL_POSITION_AUX13,
	CHANNEL_POSITION_AUX14,
	CHANNEL_POSITION_AUX15,
	CHANNEL_POSITION_AUX16,
	CHANNEL_POSITION_AUX17,
	CHANNEL_POSITION_AUX18,
	CHANNEL_POSITION_AUX19,
	CHANNEL_POSITION_AUX20,
	CHANNEL_POSITION_AUX21,
	CHANNEL_POSITION_AUX22,
	CHANNEL_POSITION_AUX23,
	CHANNEL_POSITION_AUX24,
	CHANNEL_POSITION_AUX25,
	CHANNEL_POSITION_AUX26,
	CHANNEL_POSITION_AUX27,
	CHANNEL_POSITION_AUX28,
	CHANNEL_POSITION_AUX29,
	CHANNEL_POSITION_AUX30,
	CHANNEL_POSITION_AUX31,

	CHANNEL_POSITION_TOP_CENTER,

	CHANNEL_POSITION_TOP_FRONT_LEFT,
	CHANNEL_POSITION_TOP_FRONT_RIGHT,
	CHANNEL_POSITION_TOP_FRONT_CENTER,

	CHANNEL_POSITION_TOP_REAR_LEFT,
	CHANNEL_POSITION_TOP_REAR_RIGHT,
	CHANNEL_POSITION_TOP_REAR_CENTER,

	CHANNEL_POSITION_MAX
};

struct channel {
	uint32_t channel;
	const char *name;
};

static const struct channel audio_channels[] = {
	[CHANNEL_POSITION_MONO] = { SPA_AUDIO_CHANNEL_MONO, "mono", },

	[CHANNEL_POSITION_FRONT_LEFT] = { SPA_AUDIO_CHANNEL_FL, "front-left", },
	[CHANNEL_POSITION_FRONT_RIGHT] = { SPA_AUDIO_CHANNEL_FR, "front-right", },
	[CHANNEL_POSITION_FRONT_CENTER] = { SPA_AUDIO_CHANNEL_FC, "front-center", },

	[CHANNEL_POSITION_REAR_CENTER] = { SPA_AUDIO_CHANNEL_RC, "rear-center", },
	[CHANNEL_POSITION_REAR_LEFT] = { SPA_AUDIO_CHANNEL_RL, "rear-left", },
	[CHANNEL_POSITION_REAR_RIGHT] = { SPA_AUDIO_CHANNEL_RR, "rear-right", },

	[CHANNEL_POSITION_LFE] = { SPA_AUDIO_CHANNEL_LFE, "lfe", },
	[CHANNEL_POSITION_FRONT_LEFT_OF_CENTER] = { SPA_AUDIO_CHANNEL_FLC, "front-left-of-center", },
	[CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER] = { SPA_AUDIO_CHANNEL_FRC, "front-right-of-center", },

	[CHANNEL_POSITION_SIDE_LEFT] = { SPA_AUDIO_CHANNEL_SL, "side-left", },
	[CHANNEL_POSITION_SIDE_RIGHT] = { SPA_AUDIO_CHANNEL_SR, "side-right", },

	[CHANNEL_POSITION_AUX0] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 1, "aux0", },
	[CHANNEL_POSITION_AUX1] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 2, "aux1", },
	[CHANNEL_POSITION_AUX2] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 3, "aux2", },
	[CHANNEL_POSITION_AUX3] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 4, "aux3", },
	[CHANNEL_POSITION_AUX4] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 5, "aux4", },
	[CHANNEL_POSITION_AUX5] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 6, "aux5", },
	[CHANNEL_POSITION_AUX6] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 7, "aux6", },
	[CHANNEL_POSITION_AUX7] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 8, "aux7", },
	[CHANNEL_POSITION_AUX8] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 9, "aux8", },
	[CHANNEL_POSITION_AUX9] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 10, "aux9", },
	[CHANNEL_POSITION_AUX10] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 11, "aux10", },
	[CHANNEL_POSITION_AUX11] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 12, "aux11", },
	[CHANNEL_POSITION_AUX12] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 13, "aux12", },
	[CHANNEL_POSITION_AUX13] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 14, "aux13", },
	[CHANNEL_POSITION_AUX14] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 15, "aux14", },
	[CHANNEL_POSITION_AUX15] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 16, "aux15", },
	[CHANNEL_POSITION_AUX16] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 17, "aux16", },
	[CHANNEL_POSITION_AUX17] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 18, "aux17", },
	[CHANNEL_POSITION_AUX18] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 19, "aux18", },
	[CHANNEL_POSITION_AUX19] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 20, "aux19", },
	[CHANNEL_POSITION_AUX20] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 21, "aux20", },
	[CHANNEL_POSITION_AUX21] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 22, "aux21", },
	[CHANNEL_POSITION_AUX22] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 23, "aux22", },
	[CHANNEL_POSITION_AUX23] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 24, "aux23", },
	[CHANNEL_POSITION_AUX24] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 25, "aux24", },
	[CHANNEL_POSITION_AUX25] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 26, "aux25", },
	[CHANNEL_POSITION_AUX26] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 27, "aux26", },
	[CHANNEL_POSITION_AUX27] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 28, "aux27", },
	[CHANNEL_POSITION_AUX28] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 29, "aux28", },
	[CHANNEL_POSITION_AUX29] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 30, "aux29", },
	[CHANNEL_POSITION_AUX30] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 31, "aux30", },
	[CHANNEL_POSITION_AUX31] = { SPA_AUDIO_CHANNEL_CUSTOM_START + 32, "aux31", },

	[CHANNEL_POSITION_TOP_CENTER] = { SPA_AUDIO_CHANNEL_TC, "top-center", },

	[CHANNEL_POSITION_TOP_FRONT_LEFT] = { SPA_AUDIO_CHANNEL_TFL, "top-front-left", },
	[CHANNEL_POSITION_TOP_FRONT_RIGHT] = { SPA_AUDIO_CHANNEL_TFR, "top-front-right", },
	[CHANNEL_POSITION_TOP_FRONT_CENTER] = { SPA_AUDIO_CHANNEL_TFC, "top-front-center", },

	[CHANNEL_POSITION_TOP_REAR_LEFT] = { SPA_AUDIO_CHANNEL_TRL, "top-rear-left", },
	[CHANNEL_POSITION_TOP_REAR_RIGHT] = { SPA_AUDIO_CHANNEL_TRR, "top-rear-right", },
	[CHANNEL_POSITION_TOP_REAR_CENTER] = { SPA_AUDIO_CHANNEL_TRC, "top-rear-center", },
};

struct channel_map {
	uint8_t channels;
	enum channel_position map[CHANNELS_MAX];
};

#define CHANNEL_MAP_INIT	(struct channel_map) {				\
					.channels = 2,				\
					.map[0] = CHANNEL_POSITION_FRONT_LEFT,	\
					.map[1] = CHANNEL_POSITION_FRONT_RIGHT,	\
				}

static inline uint32_t channel_pa2id(enum channel_position channel)
{
        if (channel < 0 || (size_t)channel >= SPA_N_ELEMENTS(audio_channels))
                return SPA_AUDIO_CHANNEL_UNKNOWN;
        return audio_channels[channel].channel;
}

static inline enum channel_position channel_id2pa(uint32_t id, uint32_t *aux)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(audio_channels); i++) {
		if (id == audio_channels[i].channel)
			return i;
	}
	return CHANNEL_POSITION_AUX0 + (*aux)++;
}


static inline enum channel_position channel_name2pa(const char *name, size_t size)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(audio_channels); i++) {
		if (strncmp(name, audio_channels[i].name, size) == 0)
			return i;
	}
	return CHANNEL_POSITION_INVALID;
}


static void channel_map_to_positions(const struct channel_map *map, uint32_t *pos)
{
	int i;
	for (i = 0; i < map->channels; i++)
		pos[i] = channel_pa2id(map->map[i]);
}

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
	uint32_t i, aux = 0;

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
	for (i = 0; i < map->channels; i++)
		map->map[i] = channel_id2pa(info.info.raw.position[i], &aux);

	return 0;
}

static const struct spa_pod *format_build_param(struct spa_pod_builder *b,
		uint32_t id, struct sample_spec *spec, struct channel_map *map)
{
	struct spa_audio_info_raw info;

	info = SPA_AUDIO_INFO_RAW_INIT(
			.format = format_pa2id(spec->format),
			.channels = spec->channels,
			.rate = spec->rate);
	if (map)
		channel_map_to_positions(map, info.position);

	return spa_format_audio_raw_build(b, id, &info);
}

static const struct spa_pod *format_info_build_param(struct spa_pod_builder *b,
		uint32_t id, struct format_info *info)
{
	const char *str;
	struct sample_spec ss;
	struct channel_map map, *pmap = NULL;
	size_t size;

	spa_zero(ss);
	spa_zero(map);

	if ((str = pw_properties_get(info->props, "format.sample_format")) == NULL)
		return NULL;
	if (str[0] != '\"')
		return NULL;
	size = strcspn(++str, "\"");
	ss.format = format_name2pa(str, size);
	if (ss.format == SAMPLE_INVALID)
		return NULL;

	if ((str = pw_properties_get(info->props, "format.rate")) == NULL)
		return NULL;
	ss.rate = atoi(str);

	if ((str = pw_properties_get(info->props, "format.channels")) == NULL)
		return NULL;
	ss.channels = atoi(str);

	if ((str = pw_properties_get(info->props, "format.channel_map")) != NULL) {
		while ((*str == '\"' || *str == ',') &&
		    (size = strcspn(++str, "\",")) > 0) {
			map.map[map.channels++] = channel_name2pa(str, size);
			str += size;
		}
		if (map.channels == ss.channels)
			pmap = &map;
	}
	return format_build_param(b, id, &ss, pmap);
}
