/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <stdio.h>
#include <string.h>
#include <math.h>

#include <pulse/volume.h>

#include "internal.h"
#include "sample-util.h"

SPA_EXPORT
int pa_cvolume_equal(const pa_cvolume *a, const pa_cvolume *b) {
    int i;
    pa_assert(a);
    pa_assert(b);

    pa_return_val_if_fail(pa_cvolume_valid(a), 0);

    if (PA_UNLIKELY(a == b))
        return 1;

    pa_return_val_if_fail(pa_cvolume_valid(b), 0);

    if (a->channels != b->channels)
        return 0;

    for (i = 0; i < a->channels; i++)
        if (a->values[i] != b->values[i])
            return 0;

    return 1;
}

SPA_EXPORT
pa_cvolume* pa_cvolume_init(pa_cvolume *a) {
    unsigned c;

    pa_assert(a);

    a->channels = 0;

    for (c = 0; c < PA_CHANNELS_MAX; c++)
        a->values[c] = PA_VOLUME_INVALID;

    return a;
}

SPA_EXPORT
pa_cvolume* pa_cvolume_set(pa_cvolume *a, unsigned channels, pa_volume_t v) {
    int i;

    pa_assert(a);
    pa_assert(pa_channels_valid(channels));

    a->channels = (uint8_t) channels;

    for (i = 0; i < a->channels; i++)
        /* Clamp in case there is stale data that exceeds the current
         * PA_VOLUME_MAX */
        a->values[i] = PA_CLAMP_VOLUME(v);

    return a;
}

SPA_EXPORT
pa_volume_t pa_cvolume_avg(const pa_cvolume *a) {
    uint64_t sum = 0;
    unsigned c;

    pa_assert(a);
    pa_return_val_if_fail(pa_cvolume_valid(a), PA_VOLUME_MUTED);

    for (c = 0; c < a->channels; c++)
        sum += a->values[c];

    sum /= a->channels;

    return (pa_volume_t) sum;
}

SPA_EXPORT
pa_volume_t pa_cvolume_avg_mask(const pa_cvolume *a, const pa_channel_map *cm, pa_channel_position_mask_t mask) {
    uint64_t sum = 0;
    unsigned c, n;

    pa_assert(a);

    if (!cm)
        return pa_cvolume_avg(a);

    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(a, cm), PA_VOLUME_MUTED);

    for (c = n = 0; c < a->channels; c++) {

        if (!(PA_CHANNEL_POSITION_MASK(cm->map[c]) & mask))
            continue;

        sum += a->values[c];
        n ++;
    }

    if (n > 0)
        sum /= n;

    return (pa_volume_t) sum;
}

SPA_EXPORT
pa_volume_t pa_cvolume_max(const pa_cvolume *a) {
    pa_volume_t m = PA_VOLUME_MUTED;
    unsigned c;

    pa_assert(a);
    pa_return_val_if_fail(pa_cvolume_valid(a), PA_VOLUME_MUTED);

    for (c = 0; c < a->channels; c++)
        if (a->values[c] > m)
            m = a->values[c];

    return m;
}

SPA_EXPORT
pa_volume_t pa_cvolume_min(const pa_cvolume *a) {
    pa_volume_t m = PA_VOLUME_MAX;
    unsigned c;

    pa_assert(a);
    pa_return_val_if_fail(pa_cvolume_valid(a), PA_VOLUME_MUTED);

    for (c = 0; c < a->channels; c++)
        if (a->values[c] < m)
            m = a->values[c];

    return m;
}

SPA_EXPORT
pa_volume_t pa_cvolume_max_mask(const pa_cvolume *a, const pa_channel_map *cm, pa_channel_position_mask_t mask) {
    pa_volume_t m = PA_VOLUME_MUTED;
    unsigned c;

    pa_assert(a);

    if (!cm)
        return pa_cvolume_max(a);

    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(a, cm), PA_VOLUME_MUTED);

    for (c = 0; c < a->channels; c++) {

        if (!(PA_CHANNEL_POSITION_MASK(cm->map[c]) & mask))
            continue;

        if (a->values[c] > m)
            m = a->values[c];
    }

    return m;
}

SPA_EXPORT
pa_volume_t pa_cvolume_min_mask(const pa_cvolume *a, const pa_channel_map *cm, pa_channel_position_mask_t mask) {
    pa_volume_t m = PA_VOLUME_MAX;
    unsigned c;

    pa_assert(a);

    if (!cm)
        return pa_cvolume_min(a);

    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(a, cm), PA_VOLUME_MUTED);

    for (c = 0; c < a->channels; c++) {

        if (!(PA_CHANNEL_POSITION_MASK(cm->map[c]) & mask))
            continue;

        if (a->values[c] < m)
            m = a->values[c];
    }

    return m;
}

SPA_EXPORT
pa_volume_t pa_sw_volume_multiply(pa_volume_t a, pa_volume_t b) {
    uint64_t result;

    pa_return_val_if_fail(PA_VOLUME_IS_VALID(a), PA_VOLUME_INVALID);
    pa_return_val_if_fail(PA_VOLUME_IS_VALID(b), PA_VOLUME_INVALID);

    /* cbrt((a/PA_VOLUME_NORM)^3*(b/PA_VOLUME_NORM)^3)*PA_VOLUME_NORM = a*b/PA_VOLUME_NORM */

    result = ((uint64_t) a * (uint64_t) b + (uint64_t) PA_VOLUME_NORM / 2ULL) / (uint64_t) PA_VOLUME_NORM;

    if (result > (uint64_t)PA_VOLUME_MAX)
        pa_log_warn("pa_sw_volume_multiply: Volume exceeds maximum allowed value and will be clipped. Please check your volume settings.");

    return (pa_volume_t) PA_CLAMP_VOLUME(result);
}

SPA_EXPORT
pa_volume_t pa_sw_volume_divide(pa_volume_t a, pa_volume_t b) {
    uint64_t result;

    pa_return_val_if_fail(PA_VOLUME_IS_VALID(a), PA_VOLUME_INVALID);
    pa_return_val_if_fail(PA_VOLUME_IS_VALID(b), PA_VOLUME_INVALID);

    if (b <= PA_VOLUME_MUTED)
        return 0;

    result = ((uint64_t) a * (uint64_t) PA_VOLUME_NORM + (uint64_t) b / 2ULL) / (uint64_t) b;

    if (result > (uint64_t)PA_VOLUME_MAX)
        pa_log_warn("pa_sw_volume_divide: Volume exceeds maximum allowed value and will be clipped. Please check your volume settings.");

    return (pa_volume_t) PA_CLAMP_VOLUME(result);
}

/* Amplitude, not power */
static double linear_to_dB(double v) {
    return 20.0 * log10(v);
}

static double dB_to_linear(double v) {
    return pow(10.0, v / 20.0);
}

SPA_EXPORT
pa_volume_t pa_sw_volume_from_dB(double dB) {
    if (isinf(dB) < 0 || dB <= PA_DECIBEL_MININFTY)
        return PA_VOLUME_MUTED;

    return pa_sw_volume_from_linear(dB_to_linear(dB));
}

SPA_EXPORT
double pa_sw_volume_to_dB(pa_volume_t v) {

    pa_return_val_if_fail(PA_VOLUME_IS_VALID(v), PA_DECIBEL_MININFTY);

    if (v <= PA_VOLUME_MUTED)
        return PA_DECIBEL_MININFTY;

    return linear_to_dB(pa_sw_volume_to_linear(v));
}

SPA_EXPORT
pa_volume_t pa_sw_volume_from_linear(double v) {

    if (v <= 0.0)
        return PA_VOLUME_MUTED;

    /*
     * We use a cubic mapping here, as suggested and discussed here:
     *
     * http://www.robotplanet.dk/audio/audio_gui_design/
     * http://lists.linuxaudio.org/pipermail/linux-audio-dev/2009-May/thread.html#23151
     *
     * We make sure that the conversion to linear and back yields the
     * same volume value! That's why we need the lround() below!
     */

    return (pa_volume_t) PA_CLAMP_VOLUME((uint64_t) lround(cbrt(v) * PA_VOLUME_NORM));
}

SPA_EXPORT
double pa_sw_volume_to_linear(pa_volume_t v) {
    double f;

    pa_return_val_if_fail(PA_VOLUME_IS_VALID(v), 0.0);

    if (v <= PA_VOLUME_MUTED)
        return 0.0;

    if (v == PA_VOLUME_NORM)
        return 1.0;

    f = ((double) v / PA_VOLUME_NORM);

    return f*f*f;
}

SPA_EXPORT
char *pa_cvolume_snprint(char *s, size_t l, const pa_cvolume *c) {
    unsigned channel;
    bool first = true;
    char *e;

    pa_assert(s);
    pa_assert(l > 0);
    pa_assert(c);

    pa_init_i18n();

    if (!pa_cvolume_valid(c)) {
        pa_snprintf(s, l, _("(invalid)"));
        return s;
    }

    *(e = s) = 0;

    for (channel = 0; channel < c->channels && l > 1; channel++) {
        l -= pa_snprintf(e, l, "%s%u: %3u%%",
                      first ? "" : " ",
                      channel,
                      (unsigned)(((uint64_t)c->values[channel] * 100 + (uint64_t)PA_VOLUME_NORM / 2) / (uint64_t)PA_VOLUME_NORM));

        e = strchr(e, 0);
        first = false;
    }

    return s;
}

SPA_EXPORT
char *pa_volume_snprint(char *s, size_t l, pa_volume_t v) {
    pa_assert(s);
    pa_assert(l > 0);

    pa_init_i18n();

    if (!PA_VOLUME_IS_VALID(v)) {
        pa_snprintf(s, l, _("(invalid)"));
        return s;
    }

    pa_snprintf(s, l, "%3u%%", (unsigned)(((uint64_t)v * 100 + (uint64_t)PA_VOLUME_NORM / 2) / (uint64_t)PA_VOLUME_NORM));
    return s;
}

SPA_EXPORT
char *pa_sw_cvolume_snprint_dB(char *s, size_t l, const pa_cvolume *c) {
    unsigned channel;
    bool first = true;
    char *e;

    pa_assert(s);
    pa_assert(l > 0);
    pa_assert(c);

    pa_init_i18n();

    if (!pa_cvolume_valid(c)) {
        pa_snprintf(s, l, _("(invalid)"));
        return s;
    }

    *(e = s) = 0;

    for (channel = 0; channel < c->channels && l > 1; channel++) {
        double f = pa_sw_volume_to_dB(c->values[channel]);

        l -= pa_snprintf(e, l, "%s%u: %0.2f dB",
                         first ? "" : " ",
                         channel,
                         isinf(f) < 0 || f <= PA_DECIBEL_MININFTY ? -INFINITY : f);

        e = strchr(e, 0);
        first = false;
    }

    return s;
}

SPA_EXPORT
char *pa_cvolume_snprint_verbose(char *s, size_t l, const pa_cvolume *c, const pa_channel_map *map, int print_dB) {
    char *current = s;
    bool first = true;

    pa_assert(s);
    pa_assert(l > 0);
    pa_assert(c);

    pa_init_i18n();

    if (!pa_cvolume_valid(c)) {
        pa_snprintf(s, l, _("(invalid)"));
        return s;
    }

    pa_assert(!map || (map->channels == c->channels));
    pa_assert(!map || pa_channel_map_valid(map));

    current[0] = 0;

    for (unsigned channel = 0; channel < c->channels && l > 1; channel++) {
        char channel_position[32];
        size_t bytes_printed;
        char buf[PA_VOLUME_SNPRINT_VERBOSE_MAX];

        if (map)
            pa_snprintf(channel_position, sizeof(channel_position), "%s", pa_channel_position_to_string(map->map[channel]));
        else
            pa_snprintf(channel_position, sizeof(channel_position), "%u", channel);

        bytes_printed = pa_snprintf(current, l, "%s%s: %s",
                                    first ? "" : ",   ",
                                    channel_position,
                                    pa_volume_snprint_verbose(buf, sizeof(buf), c->values[channel], print_dB));
        l -= bytes_printed;
        current += bytes_printed;
        first = false;
    }

    return s;
}

SPA_EXPORT
char *pa_sw_volume_snprint_dB(char *s, size_t l, pa_volume_t v) {
    double f;

    pa_assert(s);
    pa_assert(l > 0);

    pa_init_i18n();

    if (!PA_VOLUME_IS_VALID(v)) {
        pa_snprintf(s, l, _("(invalid)"));
        return s;
    }

    f = pa_sw_volume_to_dB(v);
    pa_snprintf(s, l, "%0.2f dB", isinf(f) < 0 || f <= PA_DECIBEL_MININFTY ? -INFINITY : f);

    return s;
}

SPA_EXPORT
char *pa_volume_snprint_verbose(char *s, size_t l, pa_volume_t v, int print_dB) {
    char dB[PA_SW_VOLUME_SNPRINT_DB_MAX];

    pa_assert(s);
    pa_assert(l > 0);

    pa_init_i18n();

    if (!PA_VOLUME_IS_VALID(v)) {
        pa_snprintf(s, l, _("(invalid)"));
        return s;
    }

    pa_snprintf(s, l, "%" PRIu32 " / %3u%%%s%s",
                v,
                (unsigned)(((uint64_t)v * 100 + (uint64_t)PA_VOLUME_NORM / 2) / (uint64_t)PA_VOLUME_NORM),
                print_dB ? " / " : "",
                print_dB ? pa_sw_volume_snprint_dB(dB, sizeof(dB), v) : "");

    return s;
}

SPA_EXPORT
int pa_cvolume_channels_equal_to(const pa_cvolume *a, pa_volume_t v) {
    unsigned c;
    pa_assert(a);

    pa_return_val_if_fail(pa_cvolume_valid(a), 0);
    pa_return_val_if_fail(PA_VOLUME_IS_VALID(v), 0);

    for (c = 0; c < a->channels; c++)
        if (a->values[c] != v)
            return 0;

    return 1;
}

SPA_EXPORT
pa_cvolume *pa_sw_cvolume_multiply(pa_cvolume *dest, const pa_cvolume *a, const pa_cvolume *b) {
    unsigned i;

    pa_assert(dest);
    pa_assert(a);
    pa_assert(b);

    pa_return_val_if_fail(pa_cvolume_valid(a), NULL);
    pa_return_val_if_fail(pa_cvolume_valid(b), NULL);

    dest->channels = PA_MIN(a->channels, b->channels);

    for (i = 0; i < dest->channels; i++)
        dest->values[i] = pa_sw_volume_multiply(a->values[i], b->values[i]);

    return dest;
}

SPA_EXPORT
pa_cvolume *pa_sw_cvolume_multiply_scalar(pa_cvolume *dest, const pa_cvolume *a, pa_volume_t b) {
    unsigned i;

    pa_assert(dest);
    pa_assert(a);

    pa_return_val_if_fail(pa_cvolume_valid(a), NULL);
    pa_return_val_if_fail(PA_VOLUME_IS_VALID(b), NULL);

    for (i = 0; i < a->channels; i++)
        dest->values[i] = pa_sw_volume_multiply(a->values[i], b);

    dest->channels = (uint8_t) i;

    return dest;
}

SPA_EXPORT
pa_cvolume *pa_sw_cvolume_divide(pa_cvolume *dest, const pa_cvolume *a, const pa_cvolume *b) {
    unsigned i;

    pa_assert(dest);
    pa_assert(a);
    pa_assert(b);

    pa_return_val_if_fail(pa_cvolume_valid(a), NULL);
    pa_return_val_if_fail(pa_cvolume_valid(b), NULL);

    dest->channels = PA_MIN(a->channels, b->channels);

    for (i = 0; i < dest->channels; i++)
        dest->values[i] = pa_sw_volume_divide(a->values[i], b->values[i]);

    return dest;
}

SPA_EXPORT
pa_cvolume *pa_sw_cvolume_divide_scalar(pa_cvolume *dest, const pa_cvolume *a, pa_volume_t b) {
    unsigned i;

    pa_assert(dest);
    pa_assert(a);

    pa_return_val_if_fail(pa_cvolume_valid(a), NULL);
    pa_return_val_if_fail(PA_VOLUME_IS_VALID(b), NULL);

    for (i = 0; i < a->channels; i++)
        dest->values[i] = pa_sw_volume_divide(a->values[i], b);

    dest->channels = (uint8_t) i;

    return dest;
}

SPA_EXPORT
int pa_cvolume_valid(const pa_cvolume *v) {
    unsigned c;

    pa_assert(v);

    if (!pa_channels_valid(v->channels))
        return 0;

    for (c = 0; c < v->channels; c++)
        if (!PA_VOLUME_IS_VALID(v->values[c]))
            return 0;

    return 1;
}

static bool on_left(pa_channel_position_t p) {
    return !!(PA_CHANNEL_POSITION_MASK(p) & PA_CHANNEL_POSITION_MASK_LEFT);
}

static bool on_right(pa_channel_position_t p) {
    return !!(PA_CHANNEL_POSITION_MASK(p) & PA_CHANNEL_POSITION_MASK_RIGHT);
}

static bool on_center(pa_channel_position_t p) {
    return !!(PA_CHANNEL_POSITION_MASK(p) & PA_CHANNEL_POSITION_MASK_CENTER);
}

static bool on_hfe(pa_channel_position_t p) {
    return !!(PA_CHANNEL_POSITION_MASK(p) & PA_CHANNEL_POSITION_MASK_HFE);
}

static bool on_lfe(pa_channel_position_t p) {
    return !!(PA_CHANNEL_POSITION_MASK(p) & PA_CHANNEL_POSITION_MASK_LFE);
}

static bool on_front(pa_channel_position_t p) {
    return !!(PA_CHANNEL_POSITION_MASK(p) & PA_CHANNEL_POSITION_MASK_FRONT);
}

static bool on_rear(pa_channel_position_t p) {
    return !!(PA_CHANNEL_POSITION_MASK(p) & PA_CHANNEL_POSITION_MASK_REAR);
}

SPA_EXPORT
pa_cvolume *pa_cvolume_remap(pa_cvolume *v, const pa_channel_map *from, const pa_channel_map *to) {
    int a, b;
    pa_cvolume result;

    pa_assert(v);
    pa_assert(from);
    pa_assert(to);

    pa_return_val_if_fail(pa_channel_map_valid(to), NULL);
    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(v, from), NULL);

    if (pa_channel_map_equal(from, to))
        return v;

    result.channels = to->channels;

    for (b = 0; b < to->channels; b++) {
        pa_volume_t k = 0;
        int n = 0;

        for (a = 0; a < from->channels; a++)
            if (from->map[a] == to->map[b]) {
                k += v->values[a];
                n ++;
            }

        if (n <= 0) {
            for (a = 0; a < from->channels; a++)
                if ((on_left(from->map[a]) && on_left(to->map[b])) ||
                    (on_right(from->map[a]) && on_right(to->map[b])) ||
                    (on_center(from->map[a]) && on_center(to->map[b])) ||
                    (on_lfe(from->map[a]) && on_lfe(to->map[b]))) {

                    k += v->values[a];
                    n ++;
                }
        }

        if (n <= 0)
            k = pa_cvolume_avg(v);
        else
            k /= n;

        result.values[b] = k;
    }

    *v = result;
    return v;
}

SPA_EXPORT
int pa_cvolume_compatible(const pa_cvolume *v, const pa_sample_spec *ss) {

    pa_assert(v);
    pa_assert(ss);

    pa_return_val_if_fail(pa_cvolume_valid(v), 0);
    pa_return_val_if_fail(pa_sample_spec_valid(ss), 0);

    return v->channels == ss->channels;
}

SPA_EXPORT
int pa_cvolume_compatible_with_channel_map(const pa_cvolume *v, const pa_channel_map *cm) {
    pa_assert(v);
    pa_assert(cm);

    pa_return_val_if_fail(pa_cvolume_valid(v), 0);
    pa_return_val_if_fail(pa_channel_map_valid(cm), 0);

    return v->channels == cm->channels;
}

/*
 * Returns the average volume of l and r, where l and r are two disjoint sets of channels
 * (e g left and right, or front and rear).
 */
static void get_avg(const pa_channel_map *map, const pa_cvolume *v, pa_volume_t *l, pa_volume_t *r,
                    bool (*on_l)(pa_channel_position_t), bool (*on_r)(pa_channel_position_t)) {
    int c;
    pa_volume_t left = 0, right = 0;
    unsigned n_left = 0, n_right = 0;

    pa_assert(v);
    pa_assert(map);
    pa_assert(map->channels == v->channels);
    pa_assert(l);
    pa_assert(r);

    for (c = 0; c < map->channels; c++) {
        if (on_l(map->map[c])) {
            left += v->values[c];
            n_left++;
        } else if (on_r(map->map[c])) {
            right += v->values[c];
            n_right++;
        }
    }

    if (n_left <= 0)
        *l = PA_VOLUME_NORM;
    else
        *l = left / n_left;

    if (n_right <= 0)
        *r = PA_VOLUME_NORM;
    else
        *r = right / n_right;
}

SPA_EXPORT
float pa_cvolume_get_balance(const pa_cvolume *v, const pa_channel_map *map) {
    pa_volume_t left, right;

    pa_assert(v);
    pa_assert(map);

    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(v, map), 0.0f);

    if (!pa_channel_map_can_balance(map))
        return 0.0f;

    get_avg(map, v, &left, &right, on_left, on_right);

    if (left == right)
        return 0.0f;

    /*   1.0,  0.0  =>  -1.0
         0.0,  1.0  =>   1.0
         0.0,  0.0  =>   0.0
         0.5,  0.5  =>   0.0
         1.0,  0.5  =>  -0.5
         1.0,  0.25 => -0.75
         0.75, 0.25 => -0.66
         0.5,  0.25 => -0.5   */

    if (left > right)
        return -1.0f + ((float) right / (float) left);
    else
        return 1.0f - ((float) left / (float) right);
}

static pa_cvolume* set_balance(pa_cvolume *v, const pa_channel_map *map, float new_balance,
                               bool (*on_l)(pa_channel_position_t), bool (*on_r)(pa_channel_position_t)) {

    pa_volume_t left, nleft, right, nright, m;
    unsigned c;

    get_avg(map, v, &left, &right, on_l, on_r);

    m = PA_MAX(left, right);

    if (new_balance <= 0) {
        nright = (new_balance + 1.0f) * m;
        nleft = m;
    } else {
        nleft = (1.0f - new_balance) * m;
        nright = m;
    }

    for (c = 0; c < map->channels; c++) {
        if (on_l(map->map[c])) {
            if (left == 0)
                v->values[c] = nleft;
            else
                v->values[c] = (pa_volume_t) PA_CLAMP_VOLUME(((uint64_t) v->values[c] * (uint64_t) nleft) / (uint64_t) left);
        } else if (on_r(map->map[c])) {
            if (right == 0)
                v->values[c] = nright;
            else
                v->values[c] = (pa_volume_t) PA_CLAMP_VOLUME(((uint64_t) v->values[c] * (uint64_t) nright) / (uint64_t) right);
        }
    }

    return v;
}


SPA_EXPORT
pa_cvolume* pa_cvolume_set_balance(pa_cvolume *v, const pa_channel_map *map, float new_balance) {
    pa_assert(map);
    pa_assert(v);

    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(v, map), NULL);
    pa_return_val_if_fail(new_balance >= -1.0f, NULL);
    pa_return_val_if_fail(new_balance <= 1.0f, NULL);

    if (!pa_channel_map_can_balance(map))
        return v;

    return set_balance(v, map, new_balance, on_left, on_right);
}

SPA_EXPORT
pa_cvolume* pa_cvolume_scale(pa_cvolume *v, pa_volume_t max) {
    unsigned c;
    pa_volume_t t = 0;

    pa_assert(v);

    pa_return_val_if_fail(pa_cvolume_valid(v), NULL);
    pa_return_val_if_fail(PA_VOLUME_IS_VALID(max), NULL);

    t = pa_cvolume_max(v);

    if (t <= PA_VOLUME_MUTED)
        return pa_cvolume_set(v, v->channels, max);

    for (c = 0; c < v->channels; c++)
        v->values[c] = (pa_volume_t) PA_CLAMP_VOLUME(((uint64_t) v->values[c] * (uint64_t) max) / (uint64_t) t);

    return v;
}

SPA_EXPORT
#if PA_CHECK_VERSION(12, 0, 0)
pa_cvolume* pa_cvolume_scale_mask(pa_cvolume *v, pa_volume_t max, const pa_channel_map *cm, pa_channel_position_mask_t mask) {
#else
pa_cvolume* pa_cvolume_scale_mask(pa_cvolume *v, pa_volume_t max, pa_channel_map *cm, pa_channel_position_mask_t mask) {
#endif
    unsigned c;
    pa_volume_t t = 0;

    pa_assert(v);

    pa_return_val_if_fail(PA_VOLUME_IS_VALID(max), NULL);

    if (!cm)
        return pa_cvolume_scale(v, max);

    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(v, cm), NULL);

    t = pa_cvolume_max_mask(v, cm, mask);

    if (t <= PA_VOLUME_MUTED)
        return pa_cvolume_set(v, v->channels, max);

    for (c = 0; c < v->channels; c++)
        v->values[c] = (pa_volume_t) PA_CLAMP_VOLUME(((uint64_t) v->values[c] * (uint64_t) max) / (uint64_t) t);

    return v;
}

SPA_EXPORT
float pa_cvolume_get_fade(const pa_cvolume *v, const pa_channel_map *map) {
    pa_volume_t rear, front;

    pa_assert(v);
    pa_assert(map);

    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(v, map), 0.0f);

    if (!pa_channel_map_can_fade(map))
        return 0.0f;

    get_avg(map, v, &rear, &front, on_rear, on_front);

    if (front == rear)
        return 0.0f;

    if (rear > front)
        return -1.0f + ((float) front / (float) rear);
    else
        return 1.0f - ((float) rear / (float) front);
}

SPA_EXPORT
pa_cvolume* pa_cvolume_set_fade(pa_cvolume *v, const pa_channel_map *map, float new_fade) {
    pa_assert(map);
    pa_assert(v);

    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(v, map), NULL);
    pa_return_val_if_fail(new_fade >= -1.0f, NULL);
    pa_return_val_if_fail(new_fade <= 1.0f, NULL);

    if (!pa_channel_map_can_fade(map))
        return v;

    return set_balance(v, map, new_fade, on_rear, on_front);
}

SPA_EXPORT
float pa_cvolume_get_lfe_balance(const pa_cvolume *v, const pa_channel_map *map) {
    pa_volume_t hfe, lfe;

    pa_assert(v);
    pa_assert(map);

    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(v, map), 0.0f);

    if (!pa_channel_map_can_lfe_balance(map))
        return 0.0f;

    get_avg(map, v, &hfe, &lfe, on_hfe, on_lfe);

    if (hfe == lfe)
        return 0.0f;

    if (hfe > lfe)
        return -1.0f + ((float) lfe / (float) hfe);
    else
        return 1.0f - ((float) hfe / (float) lfe);
}

SPA_EXPORT
pa_cvolume* pa_cvolume_set_lfe_balance(pa_cvolume *v, const pa_channel_map *map, float new_balance) {
    pa_assert(map);
    pa_assert(v);

    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(v, map), NULL);
    pa_return_val_if_fail(new_balance >= -1.0f, NULL);
    pa_return_val_if_fail(new_balance <= 1.0f, NULL);

    if (!pa_channel_map_can_lfe_balance(map))
        return v;

    return set_balance(v, map, new_balance, on_hfe, on_lfe);
}

SPA_EXPORT
pa_cvolume* pa_cvolume_set_position(
        pa_cvolume *cv,
        const pa_channel_map *map,
        pa_channel_position_t t,
        pa_volume_t v) {

    unsigned c;
    bool good = false;

    pa_assert(cv);
    pa_assert(map);

    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(cv, map), NULL);
    pa_return_val_if_fail(t < PA_CHANNEL_POSITION_MAX, NULL);
    pa_return_val_if_fail(PA_VOLUME_IS_VALID(v), NULL);

    for (c = 0; c < map->channels; c++)
        if (map->map[c] == t) {
            cv->values[c] = v;
            good = true;
        }

    return good ? cv : NULL;
}

SPA_EXPORT
pa_volume_t pa_cvolume_get_position(
        PA_CONST pa_cvolume *cv,
        const pa_channel_map *map,
        pa_channel_position_t t) {

    unsigned c;
    pa_volume_t v = PA_VOLUME_MUTED;

    pa_assert(cv);
    pa_assert(map);

    pa_return_val_if_fail(pa_cvolume_compatible_with_channel_map(cv, map), PA_VOLUME_MUTED);
    pa_return_val_if_fail(t < PA_CHANNEL_POSITION_MAX, PA_VOLUME_MUTED);

    for (c = 0; c < map->channels; c++)
        if (map->map[c] == t)
            if (cv->values[c] > v)
                v = cv->values[c];

    return v;
}

SPA_EXPORT
pa_cvolume* pa_cvolume_merge(pa_cvolume *dest, const pa_cvolume *a, const pa_cvolume *b) {
    unsigned i;

    pa_assert(dest);
    pa_assert(a);
    pa_assert(b);

    pa_return_val_if_fail(pa_cvolume_valid(a), NULL);
    pa_return_val_if_fail(pa_cvolume_valid(b), NULL);

    dest->channels = PA_MIN(a->channels, b->channels);

    for (i = 0; i < dest->channels; i++)
        dest->values[i] = PA_MAX(a->values[i], b->values[i]);

    return dest;
}

SPA_EXPORT
pa_cvolume* pa_cvolume_inc_clamp(pa_cvolume *v, pa_volume_t inc, pa_volume_t limit) {
    pa_volume_t m;

    pa_assert(v);

    pa_return_val_if_fail(pa_cvolume_valid(v), NULL);
    pa_return_val_if_fail(PA_VOLUME_IS_VALID(inc), NULL);

    m = pa_cvolume_max(v);

    if (m >= limit - inc)
        m = limit;
    else
        m += inc;

    return pa_cvolume_scale(v, m);
}

SPA_EXPORT
pa_cvolume* pa_cvolume_inc(pa_cvolume *v, pa_volume_t inc) {
    return pa_cvolume_inc_clamp(v, inc, PA_VOLUME_MAX);
}

SPA_EXPORT
pa_cvolume* pa_cvolume_dec(pa_cvolume *v, pa_volume_t dec) {
    pa_volume_t m;

    pa_assert(v);

    pa_return_val_if_fail(pa_cvolume_valid(v), NULL);
    pa_return_val_if_fail(PA_VOLUME_IS_VALID(dec), NULL);

    m = pa_cvolume_max(v);

    if (m <= PA_VOLUME_MUTED + dec)
        m = PA_VOLUME_MUTED;
    else
        m -= dec;

    return pa_cvolume_scale(v, m);
}
