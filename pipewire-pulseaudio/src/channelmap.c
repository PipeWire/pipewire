/***
  This file is part of PulseAudio.

  Copyright 2005-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <pulse/xmalloc.h>
#include <pulse/channelmap.h>

#include "internal.h"
#include "bitset.h"
#include "sample-util.h"

const char *const table[PA_CHANNEL_POSITION_MAX] = {
    [PA_CHANNEL_POSITION_MONO] = "mono",

    [PA_CHANNEL_POSITION_FRONT_CENTER] = "front-center",
    [PA_CHANNEL_POSITION_FRONT_LEFT] = "front-left",
    [PA_CHANNEL_POSITION_FRONT_RIGHT] = "front-right",

    [PA_CHANNEL_POSITION_REAR_CENTER] = "rear-center",
    [PA_CHANNEL_POSITION_REAR_LEFT] = "rear-left",
    [PA_CHANNEL_POSITION_REAR_RIGHT] = "rear-right",

    [PA_CHANNEL_POSITION_LFE] = "lfe",

    [PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER] = "front-left-of-center",
    [PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER] = "front-right-of-center",

    [PA_CHANNEL_POSITION_SIDE_LEFT] = "side-left",
    [PA_CHANNEL_POSITION_SIDE_RIGHT] = "side-right",

    [PA_CHANNEL_POSITION_AUX0] = "aux0",
    [PA_CHANNEL_POSITION_AUX1] = "aux1",
    [PA_CHANNEL_POSITION_AUX2] = "aux2",
    [PA_CHANNEL_POSITION_AUX3] = "aux3",
    [PA_CHANNEL_POSITION_AUX4] = "aux4",
    [PA_CHANNEL_POSITION_AUX5] = "aux5",
    [PA_CHANNEL_POSITION_AUX6] = "aux6",
    [PA_CHANNEL_POSITION_AUX7] = "aux7",
    [PA_CHANNEL_POSITION_AUX8] = "aux8",
    [PA_CHANNEL_POSITION_AUX9] = "aux9",
    [PA_CHANNEL_POSITION_AUX10] = "aux10",
    [PA_CHANNEL_POSITION_AUX11] = "aux11",
    [PA_CHANNEL_POSITION_AUX12] = "aux12",
    [PA_CHANNEL_POSITION_AUX13] = "aux13",
    [PA_CHANNEL_POSITION_AUX14] = "aux14",
    [PA_CHANNEL_POSITION_AUX15] = "aux15",
    [PA_CHANNEL_POSITION_AUX16] = "aux16",
    [PA_CHANNEL_POSITION_AUX17] = "aux17",
    [PA_CHANNEL_POSITION_AUX18] = "aux18",
    [PA_CHANNEL_POSITION_AUX19] = "aux19",
    [PA_CHANNEL_POSITION_AUX20] = "aux20",
    [PA_CHANNEL_POSITION_AUX21] = "aux21",
    [PA_CHANNEL_POSITION_AUX22] = "aux22",
    [PA_CHANNEL_POSITION_AUX23] = "aux23",
    [PA_CHANNEL_POSITION_AUX24] = "aux24",
    [PA_CHANNEL_POSITION_AUX25] = "aux25",
    [PA_CHANNEL_POSITION_AUX26] = "aux26",
    [PA_CHANNEL_POSITION_AUX27] = "aux27",
    [PA_CHANNEL_POSITION_AUX28] = "aux28",
    [PA_CHANNEL_POSITION_AUX29] = "aux29",
    [PA_CHANNEL_POSITION_AUX30] = "aux30",
    [PA_CHANNEL_POSITION_AUX31] = "aux31",

    [PA_CHANNEL_POSITION_TOP_CENTER] = "top-center",

    [PA_CHANNEL_POSITION_TOP_FRONT_CENTER] = "top-front-center",
    [PA_CHANNEL_POSITION_TOP_FRONT_LEFT] = "top-front-left",
    [PA_CHANNEL_POSITION_TOP_FRONT_RIGHT] = "top-front-right",

    [PA_CHANNEL_POSITION_TOP_REAR_CENTER] = "top-rear-center",
    [PA_CHANNEL_POSITION_TOP_REAR_LEFT] = "top-rear-left",
    [PA_CHANNEL_POSITION_TOP_REAR_RIGHT] = "top-rear-right"
};

const char *const pretty_table[PA_CHANNEL_POSITION_MAX] = {
    [PA_CHANNEL_POSITION_MONO] = N_("Mono"),

    [PA_CHANNEL_POSITION_FRONT_CENTER] = N_("Front Center"),
    [PA_CHANNEL_POSITION_FRONT_LEFT] = N_("Front Left"),
    [PA_CHANNEL_POSITION_FRONT_RIGHT] = N_("Front Right"),

    [PA_CHANNEL_POSITION_REAR_CENTER] = N_("Rear Center"),
    [PA_CHANNEL_POSITION_REAR_LEFT] = N_("Rear Left"),
    [PA_CHANNEL_POSITION_REAR_RIGHT] = N_("Rear Right"),

    [PA_CHANNEL_POSITION_LFE] = N_("Subwoofer"),

    [PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER] = N_("Front Left-of-center"),
    [PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER] = N_("Front Right-of-center"),

    [PA_CHANNEL_POSITION_SIDE_LEFT] = N_("Side Left"),
    [PA_CHANNEL_POSITION_SIDE_RIGHT] = N_("Side Right"),

    [PA_CHANNEL_POSITION_AUX0] = N_("Auxiliary 0"),
    [PA_CHANNEL_POSITION_AUX1] = N_("Auxiliary 1"),
    [PA_CHANNEL_POSITION_AUX2] = N_("Auxiliary 2"),
    [PA_CHANNEL_POSITION_AUX3] = N_("Auxiliary 3"),
    [PA_CHANNEL_POSITION_AUX4] = N_("Auxiliary 4"),
    [PA_CHANNEL_POSITION_AUX5] = N_("Auxiliary 5"),
    [PA_CHANNEL_POSITION_AUX6] = N_("Auxiliary 6"),
    [PA_CHANNEL_POSITION_AUX7] = N_("Auxiliary 7"),
    [PA_CHANNEL_POSITION_AUX8] = N_("Auxiliary 8"),
    [PA_CHANNEL_POSITION_AUX9] = N_("Auxiliary 9"),
    [PA_CHANNEL_POSITION_AUX10] = N_("Auxiliary 10"),
    [PA_CHANNEL_POSITION_AUX11] = N_("Auxiliary 11"),
    [PA_CHANNEL_POSITION_AUX12] = N_("Auxiliary 12"),
    [PA_CHANNEL_POSITION_AUX13] = N_("Auxiliary 13"),
    [PA_CHANNEL_POSITION_AUX14] = N_("Auxiliary 14"),
    [PA_CHANNEL_POSITION_AUX15] = N_("Auxiliary 15"),
    [PA_CHANNEL_POSITION_AUX16] = N_("Auxiliary 16"),
    [PA_CHANNEL_POSITION_AUX17] = N_("Auxiliary 17"),
    [PA_CHANNEL_POSITION_AUX18] = N_("Auxiliary 18"),
    [PA_CHANNEL_POSITION_AUX19] = N_("Auxiliary 19"),
    [PA_CHANNEL_POSITION_AUX20] = N_("Auxiliary 20"),
    [PA_CHANNEL_POSITION_AUX21] = N_("Auxiliary 21"),
    [PA_CHANNEL_POSITION_AUX22] = N_("Auxiliary 22"),
    [PA_CHANNEL_POSITION_AUX23] = N_("Auxiliary 23"),
    [PA_CHANNEL_POSITION_AUX24] = N_("Auxiliary 24"),
    [PA_CHANNEL_POSITION_AUX25] = N_("Auxiliary 25"),
    [PA_CHANNEL_POSITION_AUX26] = N_("Auxiliary 26"),
    [PA_CHANNEL_POSITION_AUX27] = N_("Auxiliary 27"),
    [PA_CHANNEL_POSITION_AUX28] = N_("Auxiliary 28"),
    [PA_CHANNEL_POSITION_AUX29] = N_("Auxiliary 29"),
    [PA_CHANNEL_POSITION_AUX30] = N_("Auxiliary 30"),
    [PA_CHANNEL_POSITION_AUX31] = N_("Auxiliary 31"),

    [PA_CHANNEL_POSITION_TOP_CENTER] = N_("Top Center"),

    [PA_CHANNEL_POSITION_TOP_FRONT_CENTER] = N_("Top Front Center"),
    [PA_CHANNEL_POSITION_TOP_FRONT_LEFT] = N_("Top Front Left"),
    [PA_CHANNEL_POSITION_TOP_FRONT_RIGHT] = N_("Top Front Right"),

    [PA_CHANNEL_POSITION_TOP_REAR_CENTER] = N_("Top Rear Center"),
    [PA_CHANNEL_POSITION_TOP_REAR_LEFT] = N_("Top Rear Left"),
    [PA_CHANNEL_POSITION_TOP_REAR_RIGHT] = N_("Top Rear Right")
};

SPA_EXPORT
pa_channel_map* pa_channel_map_init(pa_channel_map *m) {
    unsigned c;
    pa_assert(m);

    m->channels = 0;

    for (c = 0; c < PA_CHANNELS_MAX; c++)
        m->map[c] = PA_CHANNEL_POSITION_INVALID;

    return m;
}

SPA_EXPORT
pa_channel_map* pa_channel_map_init_mono(pa_channel_map *m) {
    pa_assert(m);

    pa_channel_map_init(m);

    m->channels = 1;
    m->map[0] = PA_CHANNEL_POSITION_MONO;
    return m;
}

SPA_EXPORT
pa_channel_map* pa_channel_map_init_stereo(pa_channel_map *m) {
    pa_assert(m);

    pa_channel_map_init(m);

    m->channels = 2;
    m->map[0] = PA_CHANNEL_POSITION_LEFT;
    m->map[1] = PA_CHANNEL_POSITION_RIGHT;
    return m;
}

SPA_EXPORT
pa_channel_map* pa_channel_map_init_auto(pa_channel_map *m, unsigned channels, pa_channel_map_def_t def) {
    pa_assert(m);
    pa_assert(pa_channels_valid(channels));
    pa_assert(def < PA_CHANNEL_MAP_DEF_MAX);

    pa_channel_map_init(m);

    m->channels = (uint8_t) channels;

    switch (def) {
        case PA_CHANNEL_MAP_AIFF:

            /* This is somewhat compatible with RFC3551 */

            switch (channels) {
                case 1:
                    m->map[0] = PA_CHANNEL_POSITION_MONO;
                    return m;

                case 6:
                    m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
                    m->map[1] = PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER;
                    m->map[2] = PA_CHANNEL_POSITION_FRONT_CENTER;
                    m->map[3] = PA_CHANNEL_POSITION_FRONT_RIGHT;
                    m->map[4] = PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;
                    m->map[5] = PA_CHANNEL_POSITION_REAR_CENTER;
                    return m;

                case 5:
                    m->map[2] = PA_CHANNEL_POSITION_FRONT_CENTER;
                    m->map[3] = PA_CHANNEL_POSITION_REAR_LEFT;
                    m->map[4] = PA_CHANNEL_POSITION_REAR_RIGHT;
                    /* Fall through */

                case 2:
                    m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
                    m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
                    return m;

                case 3:
                    m->map[0] = PA_CHANNEL_POSITION_LEFT;
                    m->map[1] = PA_CHANNEL_POSITION_RIGHT;
                    m->map[2] = PA_CHANNEL_POSITION_CENTER;
                    return m;

                case 4:
                    m->map[0] = PA_CHANNEL_POSITION_LEFT;
                    m->map[1] = PA_CHANNEL_POSITION_CENTER;
                    m->map[2] = PA_CHANNEL_POSITION_RIGHT;
                    m->map[3] = PA_CHANNEL_POSITION_REAR_CENTER;
                    return m;

                default:
                    return NULL;
            }

        case PA_CHANNEL_MAP_ALSA:

            switch (channels) {
                case 1:
                    m->map[0] = PA_CHANNEL_POSITION_MONO;
                    return m;

                case 8:
                    m->map[6] = PA_CHANNEL_POSITION_SIDE_LEFT;
                    m->map[7] = PA_CHANNEL_POSITION_SIDE_RIGHT;
                    /* Fall through */

                case 6:
                    m->map[5] = PA_CHANNEL_POSITION_LFE;
                    /* Fall through */

                case 5:
                    m->map[4] = PA_CHANNEL_POSITION_FRONT_CENTER;
                    /* Fall through */

                case 4:
                    m->map[2] = PA_CHANNEL_POSITION_REAR_LEFT;
                    m->map[3] = PA_CHANNEL_POSITION_REAR_RIGHT;
                    /* Fall through */

                case 2:
                    m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
                    m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
                    return m;

                default:
                    return NULL;
            }

        case PA_CHANNEL_MAP_AUX: {
            unsigned i;

            for (i = 0; i < channels; i++)
                m->map[i] = PA_CHANNEL_POSITION_AUX0 + i;

            return m;
        }

        case PA_CHANNEL_MAP_WAVEEX:

            /* Following http://www.microsoft.com/whdc/device/audio/multichaud.mspx#EKLAC */

            switch (channels) {
                case 1:
                    m->map[0] = PA_CHANNEL_POSITION_MONO;
                    return m;

                case 18:
                    m->map[15] = PA_CHANNEL_POSITION_TOP_REAR_LEFT;
                    m->map[16] = PA_CHANNEL_POSITION_TOP_REAR_CENTER;
                    m->map[17] = PA_CHANNEL_POSITION_TOP_REAR_RIGHT;
                    /* Fall through */

                case 15:
                    m->map[12] = PA_CHANNEL_POSITION_TOP_FRONT_LEFT;
                    m->map[13] = PA_CHANNEL_POSITION_TOP_FRONT_CENTER;
                    m->map[14] = PA_CHANNEL_POSITION_TOP_FRONT_RIGHT;
                    /* Fall through */

                case 12:
                    m->map[11] = PA_CHANNEL_POSITION_TOP_CENTER;
                    /* Fall through */

                case 11:
                    m->map[9] = PA_CHANNEL_POSITION_SIDE_LEFT;
                    m->map[10] = PA_CHANNEL_POSITION_SIDE_RIGHT;
                    /* Fall through */

                case 9:
                    m->map[8] = PA_CHANNEL_POSITION_REAR_CENTER;
                    /* Fall through */

                case 8:
                    m->map[6] = PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER;
                    m->map[7] = PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;
                    /* Fall through */

                case 6:
                    m->map[4] = PA_CHANNEL_POSITION_REAR_LEFT;
                    m->map[5] = PA_CHANNEL_POSITION_REAR_RIGHT;
                    /* Fall through */

                case 4:
                    m->map[3] = PA_CHANNEL_POSITION_LFE;
                    /* Fall through */

                case 3:
                    m->map[2] = PA_CHANNEL_POSITION_FRONT_CENTER;
                    /* Fall through */

                case 2:
                    m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
                    m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
                    return m;

                default:
                    return NULL;
            }

        case PA_CHANNEL_MAP_OSS:

            switch (channels) {
                case 1:
                    m->map[0] = PA_CHANNEL_POSITION_MONO;
                    return m;

                case 8:
                    m->map[6] = PA_CHANNEL_POSITION_REAR_LEFT;
                    m->map[7] = PA_CHANNEL_POSITION_REAR_RIGHT;
                    /* Fall through */

                case 6:
                    m->map[4] = PA_CHANNEL_POSITION_SIDE_LEFT;
                    m->map[5] = PA_CHANNEL_POSITION_SIDE_RIGHT;
                    /* Fall through */

                case 4:
                    m->map[3] = PA_CHANNEL_POSITION_LFE;
                    /* Fall through */

                case 3:
                    m->map[2] = PA_CHANNEL_POSITION_FRONT_CENTER;
                    /* Fall through */

                case 2:
                    m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
                    m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
                    return m;

                default:
                    return NULL;
            }

        default:
            pa_assert_not_reached();
    }
}

SPA_EXPORT
pa_channel_map* pa_channel_map_init_extend(pa_channel_map *m, unsigned channels, pa_channel_map_def_t def) {
    unsigned c;

    pa_assert(m);
    pa_assert(pa_channels_valid(channels));
    pa_assert(def < PA_CHANNEL_MAP_DEF_MAX);

    pa_channel_map_init(m);

    for (c = channels; c > 0; c--) {

        if (pa_channel_map_init_auto(m, c, def)) {
            unsigned i = 0;

            for (; c < channels; c++) {

                m->map[c] = PA_CHANNEL_POSITION_AUX0 + i;
                i++;
            }

            m->channels = (uint8_t) channels;

            return m;
        }
    }

    return NULL;
}

SPA_EXPORT
const char* pa_channel_position_to_string(pa_channel_position_t pos) {

    if (pos < 0 || pos >= PA_CHANNEL_POSITION_MAX)
        return NULL;

    return table[pos];
}

SPA_EXPORT
const char* pa_channel_position_to_pretty_string(pa_channel_position_t pos) {

    if (pos < 0 || pos >= PA_CHANNEL_POSITION_MAX)
        return NULL;

    pa_init_i18n();

    return _(pretty_table[pos]);
}

SPA_EXPORT
int pa_channel_map_equal(const pa_channel_map *a, const pa_channel_map *b) {
    unsigned c;

    pa_assert(a);
    pa_assert(b);

    pa_return_val_if_fail(pa_channel_map_valid(a), 0);

    if (PA_UNLIKELY(a == b))
        return 1;

    pa_return_val_if_fail(pa_channel_map_valid(b), 0);

    if (a->channels != b->channels)
        return 0;

    for (c = 0; c < a->channels; c++)
        if (a->map[c] != b->map[c])
            return 0;

    return 1;
}

SPA_EXPORT
char* pa_channel_map_snprint(char *s, size_t l, const pa_channel_map *map) {
    unsigned channel;
    char *e;

    pa_assert(s);
    pa_assert(l > 0);
    pa_assert(map);

    pa_init_i18n();

    if (!pa_channel_map_valid(map)) {
        pa_snprintf(s, l, _("(invalid)"));
        return s;
    }

    *(e = s) = 0;

    for (channel = 0; channel < map->channels && l > 1; channel++) {
        l -= pa_snprintf(e, l, "%s%s",
                      channel == 0 ? "" : ",",
                      pa_channel_position_to_string(map->map[channel]));

        e = strchr(e, 0);
    }

    return s;
}

SPA_EXPORT
pa_channel_position_t pa_channel_position_from_string(const char *p) {
    pa_channel_position_t i;
    pa_assert(p);

    /* Some special aliases */
    if (pa_streq(p, "left"))
        return PA_CHANNEL_POSITION_LEFT;
    else if (pa_streq(p, "right"))
        return PA_CHANNEL_POSITION_RIGHT;
    else if (pa_streq(p, "center"))
        return PA_CHANNEL_POSITION_CENTER;
    else if (pa_streq(p, "subwoofer"))
        return PA_CHANNEL_POSITION_SUBWOOFER;

    for (i = 0; i < PA_CHANNEL_POSITION_MAX; i++)
        if (pa_streq(p, table[i]))
            return i;

    return PA_CHANNEL_POSITION_INVALID;
}

SPA_EXPORT
pa_channel_map *pa_channel_map_parse(pa_channel_map *rmap, const char *s) {
    pa_channel_map map;
    char **tokens;
    int i, n_tokens;

    pa_assert(rmap);
    pa_assert(s);

    pa_channel_map_init(&map);

    /* We don't need to match against the well known channel mapping
     * "mono" here explicitly, because that can be understood as
     * listing with one channel called "mono". */

    if (pa_streq(s, "stereo")) {
        map.channels = 2;
        map.map[0] = PA_CHANNEL_POSITION_LEFT;
        map.map[1] = PA_CHANNEL_POSITION_RIGHT;
        goto finish;
    } else if (pa_streq(s, "surround-21")) {
        map.channels = 3;
        map.map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
        map.map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
        map.map[2] = PA_CHANNEL_POSITION_LFE;
        goto finish;
    } else if (pa_streq(s, "surround-40")) {
        map.channels = 4;
        map.map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
        map.map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
        map.map[2] = PA_CHANNEL_POSITION_REAR_LEFT;
        map.map[3] = PA_CHANNEL_POSITION_REAR_RIGHT;
        goto finish;
    } else if (pa_streq(s, "surround-41")) {
        map.channels = 5;
        map.map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
        map.map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
        map.map[2] = PA_CHANNEL_POSITION_REAR_LEFT;
        map.map[3] = PA_CHANNEL_POSITION_REAR_RIGHT;
        map.map[4] = PA_CHANNEL_POSITION_LFE;
        goto finish;
    } else if (pa_streq(s, "surround-50")) {
        map.channels = 5;
        map.map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
        map.map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
        map.map[2] = PA_CHANNEL_POSITION_REAR_LEFT;
        map.map[3] = PA_CHANNEL_POSITION_REAR_RIGHT;
        map.map[4] = PA_CHANNEL_POSITION_FRONT_CENTER;
        goto finish;
    } else if (pa_streq(s, "surround-51")) {
        map.channels = 6;
        map.map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
        map.map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
        map.map[2] = PA_CHANNEL_POSITION_REAR_LEFT;
        map.map[3] = PA_CHANNEL_POSITION_REAR_RIGHT;
        map.map[4] = PA_CHANNEL_POSITION_FRONT_CENTER;
        map.map[5] = PA_CHANNEL_POSITION_LFE;
        goto finish;
    } else if (pa_streq(s, "surround-71")) {
        map.channels = 8;
        map.map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
        map.map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
        map.map[2] = PA_CHANNEL_POSITION_REAR_LEFT;
        map.map[3] = PA_CHANNEL_POSITION_REAR_RIGHT;
        map.map[4] = PA_CHANNEL_POSITION_FRONT_CENTER;
        map.map[5] = PA_CHANNEL_POSITION_LFE;
        map.map[6] = PA_CHANNEL_POSITION_SIDE_LEFT;
        map.map[7] = PA_CHANNEL_POSITION_SIDE_RIGHT;
        goto finish;
    }

    map.channels = 0;

    tokens = pw_split_strv(s, ",", INT_MAX, &n_tokens);

    for (i = 0; i < n_tokens; i++) {
        pa_channel_position_t f;

        if (map.channels >= PA_CHANNELS_MAX) {
            pw_free_strv(tokens);
            return NULL;
        }

        if ((f = pa_channel_position_from_string(tokens[i])) == PA_CHANNEL_POSITION_INVALID) {
            pw_free_strv(tokens);
            return NULL;
        }
        map.map[map.channels++] = f;
    }
    pw_free_strv(tokens);

finish:

    if (!pa_channel_map_valid(&map))
        return NULL;

    *rmap = map;
    return rmap;
}

SPA_EXPORT
int pa_channel_map_valid(const pa_channel_map *map) {
    unsigned c;

    pa_assert(map);

    if (!pa_channels_valid(map->channels))
        return 0;

    for (c = 0; c < map->channels; c++)
        if (map->map[c] < 0 || map->map[c] >= PA_CHANNEL_POSITION_MAX)
            return 0;

    return 1;
}

SPA_EXPORT
int pa_channel_map_compatible(const pa_channel_map *map, const pa_sample_spec *ss) {
    pa_assert(map);
    pa_assert(ss);

    pa_return_val_if_fail(pa_channel_map_valid(map), 0);
    pa_return_val_if_fail(pa_sample_spec_valid(ss), 0);

    return map->channels == ss->channels;
}

SPA_EXPORT
int pa_channel_map_superset(const pa_channel_map *a, const pa_channel_map *b) {
    pa_channel_position_mask_t am, bm;

    pa_assert(a);
    pa_assert(b);

    pa_return_val_if_fail(pa_channel_map_valid(a), 0);

    if (PA_UNLIKELY(a == b))
        return 1;

    pa_return_val_if_fail(pa_channel_map_valid(b), 0);

    am = pa_channel_map_mask(a);
    bm = pa_channel_map_mask(b);

    return (bm & am) == bm;
}

SPA_EXPORT
int pa_channel_map_can_balance(const pa_channel_map *map) {
    pa_channel_position_mask_t m;

    pa_assert(map);
    pa_return_val_if_fail(pa_channel_map_valid(map), 0);

    m = pa_channel_map_mask(map);

    return
        (PA_CHANNEL_POSITION_MASK_LEFT & m) &&
        (PA_CHANNEL_POSITION_MASK_RIGHT & m);
}

SPA_EXPORT
int pa_channel_map_can_fade(const pa_channel_map *map) {
    pa_channel_position_mask_t m;

    pa_assert(map);
    pa_return_val_if_fail(pa_channel_map_valid(map), 0);

    m = pa_channel_map_mask(map);

    return
        (PA_CHANNEL_POSITION_MASK_FRONT & m) &&
        (PA_CHANNEL_POSITION_MASK_REAR & m);
}

SPA_EXPORT
int pa_channel_map_can_lfe_balance(const pa_channel_map *map) {
    pa_channel_position_mask_t m;

    pa_assert(map);
    pa_return_val_if_fail(pa_channel_map_valid(map), 0);

    m = pa_channel_map_mask(map);

    return
        (PA_CHANNEL_POSITION_MASK_LFE & m) &&
        (PA_CHANNEL_POSITION_MASK_HFE & m);
}

SPA_EXPORT
const char* pa_channel_map_to_name(const pa_channel_map *map) {
    pa_bitset_t in_map[PA_BITSET_ELEMENTS(PA_CHANNEL_POSITION_MAX)];
    unsigned c;

    pa_assert(map);

    pa_return_val_if_fail(pa_channel_map_valid(map), NULL);

    memset(in_map, 0, sizeof(in_map));

    for (c = 0; c < map->channels; c++)
        pa_bitset_set(in_map, map->map[c], true);

    if (pa_bitset_equals(in_map, PA_CHANNEL_POSITION_MAX,
                         PA_CHANNEL_POSITION_MONO, -1))
        return "mono";

    if (pa_bitset_equals(in_map, PA_CHANNEL_POSITION_MAX,
                         PA_CHANNEL_POSITION_LEFT, PA_CHANNEL_POSITION_RIGHT, -1))
        return "stereo";

    if (pa_bitset_equals(in_map, PA_CHANNEL_POSITION_MAX,
                         PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
                         PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT, -1))
        return "surround-40";

    if (pa_bitset_equals(in_map, PA_CHANNEL_POSITION_MAX,
                         PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
                         PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
                         PA_CHANNEL_POSITION_LFE, -1))
        return "surround-41";

    if (pa_bitset_equals(in_map, PA_CHANNEL_POSITION_MAX,
                         PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
                         PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
                         PA_CHANNEL_POSITION_FRONT_CENTER, -1))
        return "surround-50";

    if (pa_bitset_equals(in_map, PA_CHANNEL_POSITION_MAX,
                         PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
                         PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
                         PA_CHANNEL_POSITION_FRONT_CENTER, PA_CHANNEL_POSITION_LFE, -1))
        return "surround-51";

    if (pa_bitset_equals(in_map, PA_CHANNEL_POSITION_MAX,
                         PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
                         PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
                         PA_CHANNEL_POSITION_FRONT_CENTER, PA_CHANNEL_POSITION_LFE,
                         PA_CHANNEL_POSITION_SIDE_LEFT, PA_CHANNEL_POSITION_SIDE_RIGHT, -1))
        return "surround-71";

    return NULL;
}

SPA_EXPORT
const char* pa_channel_map_to_pretty_name(const pa_channel_map *map) {
    pa_bitset_t in_map[PA_BITSET_ELEMENTS(PA_CHANNEL_POSITION_MAX)];
    unsigned c;

    pa_assert(map);

    pa_return_val_if_fail(pa_channel_map_valid(map), NULL);

    memset(in_map, 0, sizeof(in_map));

    for (c = 0; c < map->channels; c++)
        pa_bitset_set(in_map, map->map[c], true);

    pa_init_i18n();

    if (pa_bitset_equals(in_map, PA_CHANNEL_POSITION_MAX,
                         PA_CHANNEL_POSITION_MONO, -1))
        return _("Mono");

    if (pa_bitset_equals(in_map, PA_CHANNEL_POSITION_MAX,
                         PA_CHANNEL_POSITION_LEFT, PA_CHANNEL_POSITION_RIGHT, -1))
        return _("Stereo");

    if (pa_bitset_equals(in_map, PA_CHANNEL_POSITION_MAX,
                         PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
                         PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT, -1))
        return _("Surround 4.0");

    if (pa_bitset_equals(in_map, PA_CHANNEL_POSITION_MAX,
                         PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
                         PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
                         PA_CHANNEL_POSITION_LFE, -1))
        return _("Surround 4.1");

    if (pa_bitset_equals(in_map, PA_CHANNEL_POSITION_MAX,
                         PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
                         PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
                         PA_CHANNEL_POSITION_FRONT_CENTER, -1))
        return _("Surround 5.0");

    if (pa_bitset_equals(in_map, PA_CHANNEL_POSITION_MAX,
                         PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
                         PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
                         PA_CHANNEL_POSITION_FRONT_CENTER, PA_CHANNEL_POSITION_LFE, -1))
        return _("Surround 5.1");

    if (pa_bitset_equals(in_map, PA_CHANNEL_POSITION_MAX,
                         PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
                         PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
                         PA_CHANNEL_POSITION_FRONT_CENTER, PA_CHANNEL_POSITION_LFE,
                         PA_CHANNEL_POSITION_SIDE_LEFT, PA_CHANNEL_POSITION_SIDE_RIGHT, -1))
        return _("Surround 7.1");

    return NULL;
}

SPA_EXPORT
int pa_channel_map_has_position(const pa_channel_map *map, pa_channel_position_t p) {
    unsigned c;

    pa_return_val_if_fail(pa_channel_map_valid(map), 0);
    pa_return_val_if_fail(p < PA_CHANNEL_POSITION_MAX, 0);

    for (c = 0; c < map->channels; c++)
        if (map->map[c] == p)
            return 1;

    return 0;
}

SPA_EXPORT
pa_channel_position_mask_t pa_channel_map_mask(const pa_channel_map *map) {
    unsigned c;
    pa_channel_position_mask_t r = 0;

    pa_return_val_if_fail(pa_channel_map_valid(map), 0);

    for (c = 0; c < map->channels; c++)
        r |= PA_CHANNEL_POSITION_MASK(map->map[c]);

    return r;
}
