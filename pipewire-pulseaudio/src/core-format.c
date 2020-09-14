/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "core-format.h"

#include <pulse/def.h>
#include <pulse/xmalloc.h>

#include "internal.h"

static const uint32_t audio_formats[] = {
	[PA_SAMPLE_U8] = SPA_AUDIO_FORMAT_U8,
	[PA_SAMPLE_ALAW] = SPA_AUDIO_FORMAT_UNKNOWN,
	[PA_SAMPLE_ULAW] = SPA_AUDIO_FORMAT_UNKNOWN,
	[PA_SAMPLE_S16NE] = SPA_AUDIO_FORMAT_S16,
	[PA_SAMPLE_S16RE] = SPA_AUDIO_FORMAT_S16_OE,
	[PA_SAMPLE_FLOAT32NE] = SPA_AUDIO_FORMAT_F32,
	[PA_SAMPLE_FLOAT32RE] = SPA_AUDIO_FORMAT_F32_OE,
	[PA_SAMPLE_S32NE] = SPA_AUDIO_FORMAT_S32,
	[PA_SAMPLE_S32RE] = SPA_AUDIO_FORMAT_S32_OE,
	[PA_SAMPLE_S24NE] = SPA_AUDIO_FORMAT_S24,
	[PA_SAMPLE_S24RE] = SPA_AUDIO_FORMAT_S24_OE,
	[PA_SAMPLE_S24_32NE] = SPA_AUDIO_FORMAT_S24_32,
	[PA_SAMPLE_S24_32RE] = SPA_AUDIO_FORMAT_S24_32_OE,
};

static inline uint32_t format_pa2id(pa_sample_format_t format)
{
	if (format < 0 || (size_t)format >= SPA_N_ELEMENTS(audio_formats))
		return SPA_AUDIO_FORMAT_UNKNOWN;
	return audio_formats[format];
}

static inline pa_sample_format_t format_id2pa(uint32_t id)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(audio_formats); i++) {
		if (id == audio_formats[i])
			return i;
	}
	return PA_SAMPLE_INVALID;
}

static const uint32_t audio_channels[] = {
	[PA_CHANNEL_POSITION_MONO] = SPA_AUDIO_CHANNEL_MONO,

	[PA_CHANNEL_POSITION_FRONT_LEFT] = SPA_AUDIO_CHANNEL_FL,
	[PA_CHANNEL_POSITION_FRONT_RIGHT] = SPA_AUDIO_CHANNEL_FR,
	[PA_CHANNEL_POSITION_FRONT_CENTER] = SPA_AUDIO_CHANNEL_FC,

	[PA_CHANNEL_POSITION_REAR_CENTER] = SPA_AUDIO_CHANNEL_RC,
	[PA_CHANNEL_POSITION_REAR_LEFT] = SPA_AUDIO_CHANNEL_RL,
	[PA_CHANNEL_POSITION_REAR_RIGHT] = SPA_AUDIO_CHANNEL_RR,

	[PA_CHANNEL_POSITION_LFE] = SPA_AUDIO_CHANNEL_LFE,
	[PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER] = SPA_AUDIO_CHANNEL_FLC,
	[PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER] = SPA_AUDIO_CHANNEL_FRC,

	[PA_CHANNEL_POSITION_SIDE_LEFT] = SPA_AUDIO_CHANNEL_SL,
	[PA_CHANNEL_POSITION_SIDE_RIGHT] = SPA_AUDIO_CHANNEL_SR,

	[PA_CHANNEL_POSITION_AUX0] = SPA_AUDIO_CHANNEL_CUSTOM_START + 1,
	[PA_CHANNEL_POSITION_AUX1] = SPA_AUDIO_CHANNEL_CUSTOM_START + 2,
	[PA_CHANNEL_POSITION_AUX2] = SPA_AUDIO_CHANNEL_CUSTOM_START + 3,
	[PA_CHANNEL_POSITION_AUX3] = SPA_AUDIO_CHANNEL_CUSTOM_START + 4,
	[PA_CHANNEL_POSITION_AUX4] = SPA_AUDIO_CHANNEL_CUSTOM_START + 5,
	[PA_CHANNEL_POSITION_AUX5] = SPA_AUDIO_CHANNEL_CUSTOM_START + 6,
	[PA_CHANNEL_POSITION_AUX6] = SPA_AUDIO_CHANNEL_CUSTOM_START + 7,
	[PA_CHANNEL_POSITION_AUX7] = SPA_AUDIO_CHANNEL_CUSTOM_START + 8,
	[PA_CHANNEL_POSITION_AUX8] = SPA_AUDIO_CHANNEL_CUSTOM_START + 9,
	[PA_CHANNEL_POSITION_AUX9] = SPA_AUDIO_CHANNEL_CUSTOM_START + 10,
	[PA_CHANNEL_POSITION_AUX10] = SPA_AUDIO_CHANNEL_CUSTOM_START + 11,
	[PA_CHANNEL_POSITION_AUX11] = SPA_AUDIO_CHANNEL_CUSTOM_START + 12,
	[PA_CHANNEL_POSITION_AUX12] = SPA_AUDIO_CHANNEL_CUSTOM_START + 13,
	[PA_CHANNEL_POSITION_AUX13] = SPA_AUDIO_CHANNEL_CUSTOM_START + 14,
	[PA_CHANNEL_POSITION_AUX14] = SPA_AUDIO_CHANNEL_CUSTOM_START + 15,
	[PA_CHANNEL_POSITION_AUX15] = SPA_AUDIO_CHANNEL_CUSTOM_START + 16,
	[PA_CHANNEL_POSITION_AUX16] = SPA_AUDIO_CHANNEL_CUSTOM_START + 17,
	[PA_CHANNEL_POSITION_AUX17] = SPA_AUDIO_CHANNEL_CUSTOM_START + 18,
	[PA_CHANNEL_POSITION_AUX18] = SPA_AUDIO_CHANNEL_CUSTOM_START + 19,
	[PA_CHANNEL_POSITION_AUX19] = SPA_AUDIO_CHANNEL_CUSTOM_START + 20,
	[PA_CHANNEL_POSITION_AUX20] = SPA_AUDIO_CHANNEL_CUSTOM_START + 21,
	[PA_CHANNEL_POSITION_AUX21] = SPA_AUDIO_CHANNEL_CUSTOM_START + 22,
	[PA_CHANNEL_POSITION_AUX22] = SPA_AUDIO_CHANNEL_CUSTOM_START + 23,
	[PA_CHANNEL_POSITION_AUX23] = SPA_AUDIO_CHANNEL_CUSTOM_START + 24,
	[PA_CHANNEL_POSITION_AUX24] = SPA_AUDIO_CHANNEL_CUSTOM_START + 25,
	[PA_CHANNEL_POSITION_AUX25] = SPA_AUDIO_CHANNEL_CUSTOM_START + 26,
	[PA_CHANNEL_POSITION_AUX26] = SPA_AUDIO_CHANNEL_CUSTOM_START + 27,
	[PA_CHANNEL_POSITION_AUX27] = SPA_AUDIO_CHANNEL_CUSTOM_START + 28,
	[PA_CHANNEL_POSITION_AUX28] = SPA_AUDIO_CHANNEL_CUSTOM_START + 29,
	[PA_CHANNEL_POSITION_AUX29] = SPA_AUDIO_CHANNEL_CUSTOM_START + 30,
	[PA_CHANNEL_POSITION_AUX30] = SPA_AUDIO_CHANNEL_CUSTOM_START + 31,
	[PA_CHANNEL_POSITION_AUX31] = SPA_AUDIO_CHANNEL_CUSTOM_START + 32,

	[PA_CHANNEL_POSITION_TOP_CENTER] = SPA_AUDIO_CHANNEL_TC,

	[PA_CHANNEL_POSITION_TOP_FRONT_LEFT] = SPA_AUDIO_CHANNEL_TFL,
	[PA_CHANNEL_POSITION_TOP_FRONT_RIGHT] = SPA_AUDIO_CHANNEL_TFR,
	[PA_CHANNEL_POSITION_TOP_FRONT_CENTER] = SPA_AUDIO_CHANNEL_TFC,

	[PA_CHANNEL_POSITION_TOP_REAR_LEFT] = SPA_AUDIO_CHANNEL_TRL,
	[PA_CHANNEL_POSITION_TOP_REAR_RIGHT] = SPA_AUDIO_CHANNEL_TRR,
	[PA_CHANNEL_POSITION_TOP_REAR_CENTER] = SPA_AUDIO_CHANNEL_TRC,
};

static inline uint32_t channel_pa2id(pa_channel_position_t channel)
{
	if (channel < 0 || (size_t)channel >= SPA_N_ELEMENTS(audio_channels))
		return SPA_AUDIO_CHANNEL_UNKNOWN;
	return audio_channels[channel];
}

static inline pa_channel_position_t channel_id2pa(uint32_t id, uint32_t *aux)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(audio_channels); i++) {
		if (id == audio_channels[i])
			return i;
	}
	return PA_CHANNEL_POSITION_AUX0 + (*aux)++;
}

SPA_EXPORT
int pa_format_info_get_sample_format(const pa_format_info *f, pa_sample_format_t *sf) {
    int r;
    char *sf_str;
    pa_sample_format_t sf_local;

    pa_assert(f);
    pa_assert(sf);

    r = pa_format_info_get_prop_string(f, PA_PROP_FORMAT_SAMPLE_FORMAT, &sf_str);
    if (r < 0)
        return r;

    sf_local = pa_parse_sample_format(sf_str);
    pa_xfree(sf_str);

    if (!pa_sample_format_valid(sf_local)) {
        pa_log_debug("Invalid sample format.");
        return -PA_ERR_INVALID;
    }

    *sf = sf_local;

    return 0;
}

SPA_EXPORT
int pa_format_info_get_rate(const pa_format_info *f, uint32_t *rate) {
    int r;
    int rate_local;

    pa_assert(f);
    pa_assert(rate);

    r = pa_format_info_get_prop_int(f, PA_PROP_FORMAT_RATE, &rate_local);
    if (r < 0)
        return r;

    if (!pa_sample_rate_valid(rate_local)) {
        pa_log_debug("Invalid sample rate: %i", rate_local);
        return -PA_ERR_INVALID;
    }

    *rate = rate_local;

    return 0;
}

SPA_EXPORT
int pa_format_info_get_channels(const pa_format_info *f, uint8_t *channels) {
    int r;
    int channels_local;

    pa_assert(f);
    pa_assert(channels);

    r = pa_format_info_get_prop_int(f, PA_PROP_FORMAT_CHANNELS, &channels_local);
    if (r < 0)
        return r;

    if (!pa_channels_valid(channels_local)) {
        pa_log_debug("Invalid channel count: %i", channels_local);
        return -PA_ERR_INVALID;
    }

    *channels = channels_local;

    return 0;
}

SPA_EXPORT
int pa_format_info_get_channel_map(const pa_format_info *f, pa_channel_map *map) {
    int r;
    char *map_str;

    pa_assert(f);
    pa_assert(map);

    r = pa_format_info_get_prop_string(f, PA_PROP_FORMAT_CHANNEL_MAP, &map_str);
    if (r < 0)
        return r;

    map = pa_channel_map_parse(map, map_str);
    pa_xfree(map_str);

    if (!map) {
        pa_log_debug("Failed to parse channel map.");
        return -PA_ERR_INVALID;
    }

    return 0;
}

SPA_EXPORT
pa_format_info *pa_format_info_from_sample_spec2(const pa_sample_spec *ss, const pa_channel_map *map, bool set_format,
                                                 bool set_rate, bool set_channels) {
    pa_format_info *format = NULL;

    pa_assert(ss);

    format = pa_format_info_new();
    format->encoding = PA_ENCODING_PCM;

    if (set_format)
        pa_format_info_set_sample_format(format, ss->format);

    if (set_rate)
        pa_format_info_set_rate(format, ss->rate);

    if (set_channels) {
        pa_format_info_set_channels(format, ss->channels);

        if (map) {
            if (map->channels != ss->channels) {
                pa_log_debug("Channel map is incompatible with the sample spec.");
                goto fail;
            }

            pa_format_info_set_channel_map(format, map);
        }
    }

    return format;

fail:
    if (format)
        pa_format_info_free(format);

    return NULL;
}

SPA_EXPORT
int pa_format_info_to_sample_spec2(const pa_format_info *f, pa_sample_spec *ss, pa_channel_map *map,
                                   const pa_sample_spec *fallback_ss, const pa_channel_map *fallback_map) {
    int r, r2;
    pa_sample_spec ss_local;
    pa_channel_map map_local;

    pa_assert(f);
    pa_assert(ss);
    pa_assert(map);
    pa_assert(fallback_ss);
    pa_assert(fallback_map);

    if (!pa_format_info_is_pcm(f))
        return pa_format_info_to_sample_spec_fake(f, ss, map);

    r = pa_format_info_get_sample_format(f, &ss_local.format);
    if (r == -PA_ERR_NOENTITY)
        ss_local.format = fallback_ss->format;
    else if (r < 0)
        return r;

    pa_assert(pa_sample_format_valid(ss_local.format));

    r = pa_format_info_get_rate(f, &ss_local.rate);
    if (r == -PA_ERR_NOENTITY)
        ss_local.rate = fallback_ss->rate;
    else if (r < 0)
        return r;

    pa_assert(pa_sample_rate_valid(ss_local.rate));

    r = pa_format_info_get_channels(f, &ss_local.channels);
    r2 = pa_format_info_get_channel_map(f, &map_local);
    if (r == -PA_ERR_NOENTITY && r2 >= 0)
        ss_local.channels = map_local.channels;
    else if (r == -PA_ERR_NOENTITY)
        ss_local.channels = fallback_ss->channels;
    else if (r < 0)
        return r;

    pa_assert(pa_channels_valid(ss_local.channels));

    if (r2 >= 0 && map_local.channels != ss_local.channels) {
        pa_log_debug("Channel map is not compatible with the sample spec.");
        return -PA_ERR_INVALID;
    }

    if (r2 == -PA_ERR_NOENTITY) {
        if (fallback_map->channels == ss_local.channels)
            map_local = *fallback_map;
        else
            pa_channel_map_init_extend(&map_local, ss_local.channels, PA_CHANNEL_MAP_DEFAULT);
    } else if (r2 < 0)
        return r2;

    pa_assert(pa_channel_map_valid(&map_local));
    pa_assert(ss_local.channels == map_local.channels);

    *ss = ss_local;
    *map = map_local;

    return 0;
}

SPA_EXPORT
int pa_format_info_to_sample_spec_fake(const pa_format_info *f, pa_sample_spec *ss, pa_channel_map *map) {
    int rate;

    pa_assert(f);
    pa_assert(ss);

    /* Note: When we add support for non-IEC61937 encapsulated compressed
     * formats, this function should return a non-zero values for these. */

    ss->format = PA_SAMPLE_S16LE;
    ss->channels = 2;

    if (map)
        pa_channel_map_init_stereo(map);

    pa_return_val_if_fail(pa_format_info_get_prop_int(f, PA_PROP_FORMAT_RATE, &rate) == 0, -PA_ERR_INVALID);
    ss->rate = (uint32_t) rate;

    if (f->encoding == PA_ENCODING_EAC3_IEC61937)
        ss->rate *= 4;

    return 0;
}

int pa_format_parse_param(const struct spa_pod *param, pa_sample_spec *spec, pa_channel_map *map)
{
	struct spa_audio_info info = { 0 };
	uint32_t i, aux;

	if (param == NULL)
		return -EINVAL;

	spa_format_parse(param, &info.media_type, &info.media_subtype);

	if (info.media_type != SPA_MEDIA_TYPE_audio ||
	    info.media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    spa_format_audio_raw_parse(param, &info.info.raw) < 0 ||
	    !SPA_AUDIO_FORMAT_IS_INTERLEAVED(info.info.raw.format)) {
		return -ENOTSUP;
	}

	spec->format = format_id2pa(info.info.raw.format);
	if (spec->format == PA_SAMPLE_INVALID)
		return -ENOTSUP;

	spec->rate = info.info.raw.rate;
	spec->channels = info.info.raw.channels;

	aux = 0;
	pa_channel_map_init(map);
	map->channels = info.info.raw.channels;
	for (i = 0; i < info.info.raw.channels; i++)
		map->map[i] = channel_id2pa(info.info.raw.position[i], &aux);

	if (!pa_channel_map_valid(map))
		pa_channel_map_init_extend(map, info.info.raw.channels, PA_CHANNEL_MAP_OSS);

	return 0;
}

const struct spa_pod *pa_format_build_param(struct spa_pod_builder *b,
		uint32_t id, pa_sample_spec *spec, pa_channel_map *map)
{
	struct spa_audio_info_raw info;

	info = SPA_AUDIO_INFO_RAW_INIT( .format = format_pa2id(spec->format),
		                .channels = spec->channels,
		                .rate = spec->rate);
	if (map) {
		int i;
		for (i = 0; i < map->channels; i++)
			info.position[i] = channel_pa2id(map->map[i]);
	}
	return spa_format_audio_raw_build(b, id, &info);
}
