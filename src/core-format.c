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
