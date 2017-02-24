/* GStreamer
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stdio.h>

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>

#include <spa/include/spa/video/format.h>
#include <spa/include/spa/audio/format.h>
#include <spa/include/spa/format.h>

#include "gstpinosformat.h"

#if 0
static guint
calc_range_size (const GValue *val)
{
  guint res = 0;

  if (G_VALUE_TYPE (val) == GST_TYPE_LIST)
    res += gst_value_list_get_size (val);
  else if (G_VALUE_TYPE (val) == GST_TYPE_ARRAY)
    res += gst_value_array_get_size (val);
  else if (G_VALUE_TYPE (val) == GST_TYPE_INT_RANGE)
    res += 2;
  else if (G_VALUE_TYPE (val) == GST_TYPE_INT64_RANGE)
    res += 2;
  else if (G_VALUE_TYPE (val) == GST_TYPE_DOUBLE_RANGE)
    res += 2;
  else if (G_VALUE_TYPE (val) == GST_TYPE_FRACTION_RANGE)
    res += 2;

  return res;
}

static void
calc_size (GstCapsFeatures *cf, GstStructure *cs, guint *n_infos, guint *n_ranges, guint *n_datas)
{
  guint c;
  const GValue *val;

  if (gst_structure_has_name (cs, "video/x-raw")) {
    if ((val = gst_structure_get_value (cs, "format"))) {
      c = calc_range_size (val);
      n_infos[0] += 1;
      n_ranges[0] += c;
      n_datas[0] += (1 + c) * sizeof (SpaVideoFormat);
    }
    if ((val = gst_structure_get_value (cs, "width")) ||
        (val = gst_structure_get_value (cs, "height"))) {
      c = calc_range_size (val);
      n_infos[0]++;
      n_ranges[0] += c;
      n_datas[0] += (1 + c) * sizeof (SpaRectangle);
    }
    if ((val = gst_structure_get_value (cs, "framerate"))) {
      c = calc_range_size (val);
      n_infos[0]++;
      n_ranges[0] += c;
      n_datas[0] += (1 + c) * sizeof (SpaFraction);
    }
  } else if (gst_structure_has_name (cs, "audio/x-raw")) {
    if ((val = gst_structure_get_value (cs, "format"))) {
      c = calc_range_size (val);
      n_infos[0]++;
      n_ranges[0] += c;
      n_datas[0] += (1 + c) * sizeof (SpaAudioFormat);
    }
    if ((val = gst_structure_get_value (cs, "layout"))) {
      c = calc_range_size (val);
      n_infos[0]++;
      n_ranges[0] += c;
      n_datas[0] += (1 + c) * sizeof (SpaAudioLayout);
    }
    if ((val = gst_structure_get_value (cs, "rate"))) {
      c = calc_range_size (val);
      n_infos[0]++;
      n_ranges[0] += c;
      n_datas[0] += (1 + c) * sizeof (uint32_t);
    }
    if ((val = gst_structure_get_value (cs, "channels"))) {
      c = calc_range_size (val);
      n_infos[0]++;
      n_ranges[0] += c;
      n_datas[0] += (1 + c) * sizeof (uint32_t);
    }
  } else if (gst_structure_has_name (cs, "image/jpeg") ||
             gst_structure_has_name (cs, "video/x-h264")) {
    if ((val = gst_structure_get_value (cs, "width")) ||
        (val = gst_structure_get_value (cs, "height"))) {
      c = calc_range_size (val);
      n_infos[0]++;
      n_ranges[0] += c;
      n_datas[0] += (1 + c) * sizeof (SpaRectangle);
    }
    if ((val = gst_structure_get_value (cs, "framerate"))) {
      c = calc_range_size (val);
      n_infos[0]++;
      n_ranges[0] += c;
      n_datas[0] += (1 + c) * sizeof (SpaFraction);
    }
  }
}

typedef struct {
  GstCapsFeatures *cf;
  GstStructure *cs;
  SpaFormat *f;
  SpaPropInfo *bpi;
  SpaPropRangeInfo *bri;
  int pi, ri;
  void *p;
} ConvertData;

static void
handle_video_format (ConvertData *d)
{
  const GValue *val;

  val = gst_structure_get_value (d->cs, "format");
  if (val) {
    SpaVideoFormat *sv = d->p;

    spa_prop_info_fill_video (&d->bpi[d->pi],
                              SPA_PROP_ID_VIDEO_FORMAT,
                              SPA_PTRDIFF (d->p, d->f));

    if (G_VALUE_TYPE (val) == G_TYPE_STRING) {
      *sv = gst_video_format_from_string (g_value_get_string (val));
      d->p = ++sv;
    } else if (G_VALUE_TYPE (val) == GST_TYPE_LIST) {
      g_debug ("implement me");
      SPA_PROPS_INDEX_UNSET (&d->f->props, d->pi);
    } else if (G_VALUE_TYPE (val) == GST_TYPE_ARRAY) {
      g_debug ("implement me");
      SPA_PROPS_INDEX_UNSET (&d->f->props, d->pi);
    } else {
      g_debug ("implement me");
      SPA_PROPS_INDEX_UNSET (&d->f->props, d->pi);
    }
    d->pi++;
  }
}

static void
handle_video_size (ConvertData *d)
{
  const GValue *val, *val2;

  val = gst_structure_get_value (d->cs, "width");
  val2 = gst_structure_get_value (d->cs, "height");
  if (val || val2) {
    SpaRectangle *sv = d->p;

    spa_prop_info_fill_video (&d->bpi[d->pi],
                              SPA_PROP_ID_VIDEO_SIZE,
                              SPA_PTRDIFF (d->p, d->f));

    if (val && val2 && G_VALUE_TYPE (val) == G_VALUE_TYPE (val2)) {
      if (G_VALUE_TYPE (val) == G_TYPE_INT) {
        sv->width = g_value_get_int (val);
        sv->height = g_value_get_int (val2);
        d->p = ++sv;
        d->bpi[d->pi].range_type = SPA_PROP_RANGE_TYPE_NONE;
        d->bpi[d->pi].n_range_values = 0;
        d->bpi[d->pi].range_values = NULL;
      } else if (G_VALUE_TYPE (val) == GST_TYPE_INT_RANGE) {
        d->bpi[d->pi].range_type = SPA_PROP_RANGE_TYPE_MIN_MAX;
        d->bpi[d->pi].n_range_values = 2;
        d->bpi[d->pi].range_values = &d->bri[d->ri];

        d->bri[d->ri].name = NULL;
        d->bri[d->ri].val.size = sizeof (SpaRectangle);
        d->bri[d->ri].val.value = d->p;
        sv->width = gst_value_get_int_range_min (val);
        sv->height = gst_value_get_int_range_min (val2);
        d->p = ++sv;
        d->ri++;

        d->bri[d->ri].name = NULL;
        d->bri[d->ri].val.size = sizeof (SpaRectangle);
        d->bri[d->ri].val.value = d->p;
        sv->width = gst_value_get_int_range_max (val);
        sv->height = gst_value_get_int_range_max (val2);
        d->p = ++sv;
        d->ri++;

        SPA_PROPS_INDEX_UNSET (&d->f->props, d->pi);
      } else if (G_VALUE_TYPE (val) == GST_TYPE_LIST) {
        g_debug ("implement me");
        SPA_PROPS_INDEX_UNSET (&d->f->props, d->pi);
      } else if (G_VALUE_TYPE (val) == GST_TYPE_ARRAY) {
        g_debug ("implement me");
        SPA_PROPS_INDEX_UNSET (&d->f->props, d->pi);
      } else {
        g_debug ("implement me");
        SPA_PROPS_INDEX_UNSET (&d->f->props, d->pi);
      }
    }
    d->pi++;
  }
}

static void
handle_video_framerate (ConvertData *d)
{
  const GValue *val;

  val = gst_structure_get_value (d->cs, "framerate");
  if (val) {
    SpaFraction *sv = d->p;

    spa_prop_info_fill_video (&d->bpi[d->pi],
                              SPA_PROP_ID_VIDEO_FRAMERATE,
                              SPA_PTRDIFF (d->p, d->f));

    if (G_VALUE_TYPE (val) == GST_TYPE_FRACTION) {
      sv->num = gst_value_get_fraction_numerator (val);
      sv->denom = gst_value_get_fraction_denominator (val);
      d->p = ++sv;
      d->bpi[d->pi].range_type = SPA_PROP_RANGE_TYPE_NONE;
      d->bpi[d->pi].n_range_values = 0;
      d->bpi[d->pi].range_values = NULL;
    } else if (G_VALUE_TYPE (val) == GST_TYPE_FRACTION_RANGE) {
      const GValue *min, *max;

      min = gst_value_get_fraction_range_min (val);
      max = gst_value_get_fraction_range_max (val);

      d->bpi[d->pi].range_type = SPA_PROP_RANGE_TYPE_MIN_MAX;
      d->bpi[d->pi].n_range_values = 2;
      d->bpi[d->pi].range_values = &d->bri[d->ri];

      d->bri[d->ri].name = NULL;
      d->bri[d->ri].val.size = sizeof (SpaFraction);
      d->bri[d->ri].val.value = d->p;
      sv->num = gst_value_get_fraction_numerator (min);
      sv->denom = gst_value_get_fraction_denominator (min);
      d->p = ++sv;
      d->ri++;

      d->bri[d->ri].name = NULL;
      d->bri[d->ri].val.size = sizeof (SpaFraction);
      d->bri[d->ri].val.value = d->p;
      sv->num = gst_value_get_fraction_numerator (max);
      sv->denom = gst_value_get_fraction_denominator (max);
      d->p = ++sv;
      d->ri++;

      SPA_PROPS_INDEX_UNSET (&d->f->props, d->pi);
    } else if (G_VALUE_TYPE (val) == GST_TYPE_LIST) {
      g_debug ("implement me");
      SPA_PROPS_INDEX_UNSET (&d->f->props, d->pi);
    } else if (G_VALUE_TYPE (val) == GST_TYPE_ARRAY) {
      g_debug ("implement me");
      SPA_PROPS_INDEX_UNSET (&d->f->props, d->pi);
    } else {
      g_debug ("implement me");
      SPA_PROPS_INDEX_UNSET (&d->f->props, d->pi);
    }
    d->pi++;
  }
}
#endif

static SpaFormat *
convert_1 (GstCapsFeatures *cf, GstStructure *cs)
{
#if 0
  const GValue *val;
  guint size, n_infos = 0, n_ranges = 0, n_datas = 0;
  ConvertData d;

  d.cf = cf;
  d.cs = cs;

  calc_size (cf, cs, &n_infos, &n_ranges, &n_datas);

  size = sizeof (SpaFormat);
  size += n_infos * sizeof (SpaPropInfo);
  size += n_ranges * sizeof (SpaPropRangeInfo);
  size += n_datas;

  d.f = malloc (size);
  d.bpi = SPA_MEMBER (d.f, sizeof (SpaFormat), SpaPropInfo);
  d.bri = SPA_MEMBER (d.bpi, n_infos * sizeof (SpaPropInfo), SpaPropRangeInfo);
  d.p = SPA_MEMBER (d.bri, n_ranges * sizeof (SpaPropRangeInfo), void);

  d.pi = d.ri = 0;

  d.f->props.n_prop_info = n_infos;
  d.f->props.prop_info = d.bpi;
  d.f->props.unset_mask = 0;

  if (gst_structure_has_name (d.cs, "video/x-raw")) {
    d.f->media_type = SPA_MEDIA_TYPE_VIDEO;
    d.f->media_subtype = SPA_MEDIA_SUBTYPE_RAW;
    handle_video_format (&d);
    handle_video_size (&d);
    handle_video_framerate (&d);
  } else if (gst_structure_has_name (d.cs, "audio/x-raw")) {
    d.f->media_type = SPA_MEDIA_TYPE_AUDIO;
    d.f->media_subtype = SPA_MEDIA_SUBTYPE_RAW;

    val = gst_structure_get_value (d.cs, "format");
    if (val) {
      SpaAudioFormat *sv = d.p;

      spa_prop_info_fill_audio (&d.bpi[d.pi],
                                SPA_PROP_ID_AUDIO_FORMAT,
                                SPA_PTRDIFF (d.p, d.f));

      if (G_VALUE_TYPE (val) == G_TYPE_STRING) {
        *sv = gst_audio_format_from_string (g_value_get_string (val));
        d.p = ++sv;
      } else if (G_VALUE_TYPE (val) == GST_TYPE_LIST) {
        g_debug ("implement me");
        SPA_PROPS_INDEX_UNSET (&d.f->props, d.pi);
      } else if (G_VALUE_TYPE (val) == GST_TYPE_ARRAY) {
        g_debug ("implement me");
        SPA_PROPS_INDEX_UNSET (&d.f->props, d.pi);
      } else {
        g_debug ("implement me");
        SPA_PROPS_INDEX_UNSET (&d.f->props, d.pi);
      }
      d.pi++;
    }
    val = gst_structure_get_value (d.cs, "layout");
    if (val) {
      SpaAudioLayout *sv = d.p;

      spa_prop_info_fill_audio (&d.bpi[d.pi],
                                SPA_PROP_ID_AUDIO_LAYOUT,
                                SPA_PTRDIFF (d.p, d.f));

      if (G_VALUE_TYPE (val) == G_TYPE_STRING) {
        const gchar *s = g_value_get_string (val);
        if (!strcmp (s, "interleaved"))
          *sv = SPA_AUDIO_LAYOUT_INTERLEAVED;
        else if (!strcmp (s, "non-interleaved"))
          *sv = SPA_AUDIO_LAYOUT_NON_INTERLEAVED;
        d.p = ++sv;
      } else if (G_VALUE_TYPE (val) == GST_TYPE_LIST) {
        g_debug ("implement me");
        SPA_PROPS_INDEX_UNSET (&d.f->props, d.pi);
      } else if (G_VALUE_TYPE (val) == GST_TYPE_ARRAY) {
        g_debug ("implement me");
        SPA_PROPS_INDEX_UNSET (&d.f->props, d.pi);
      } else {
        g_debug ("implement me");
        SPA_PROPS_INDEX_UNSET (&d.f->props, d.pi);
      }
      d.pi++;
    }
    val = gst_structure_get_value (d.cs, "rate");
    if (val) {
      uint32_t *sv = d.p;

      spa_prop_info_fill_audio (&d.bpi[d.pi],
                                SPA_PROP_ID_AUDIO_RATE,
                                SPA_PTRDIFF (d.p, d.f));

      if (G_VALUE_TYPE (val) == G_TYPE_INT) {
        *sv = g_value_get_int (val);
        d.p = ++sv;
      } else if (G_VALUE_TYPE (val) == GST_TYPE_LIST) {
        g_debug ("implement me");
        SPA_PROPS_INDEX_UNSET (&d.f->props, d.pi);
      } else if (G_VALUE_TYPE (val) == GST_TYPE_ARRAY) {
        g_debug ("implement me");
        SPA_PROPS_INDEX_UNSET (&d.f->props, d.pi);
      } else {
        g_debug ("implement me");
        SPA_PROPS_INDEX_UNSET (&d.f->props, d.pi);
      }
      d.pi++;
    }
    val = gst_structure_get_value (cs, "channels");
    if (val) {
      uint32_t *sv = d.p;

      spa_prop_info_fill_audio (&d.bpi[d.pi],
                                SPA_PROP_ID_AUDIO_CHANNELS,
                                SPA_PTRDIFF (d.p, d.f));

      if (G_VALUE_TYPE (val) == G_TYPE_INT) {
        *sv = g_value_get_int (val);
        d.p = ++sv;
      } else if (G_VALUE_TYPE (val) == GST_TYPE_LIST) {
        g_debug ("implement me");
        SPA_PROPS_INDEX_UNSET (&d.f->props, d.pi);
      } else if (G_VALUE_TYPE (val) == GST_TYPE_ARRAY) {
        g_debug ("implement me");
        SPA_PROPS_INDEX_UNSET (&d.f->props, d.pi);
      } else {
        g_debug ("implement me");
        SPA_PROPS_INDEX_UNSET (&d.f->props, d.pi);
      }
      d.pi++;
    }
  } else if (gst_structure_has_name (cs, "image/jpeg")) {
    d.f->media_type = SPA_MEDIA_TYPE_VIDEO;
    d.f->media_subtype = SPA_MEDIA_SUBTYPE_MJPG;
    handle_video_size (&d);
    handle_video_framerate (&d);
  } else if (gst_structure_has_name (cs, "video/x-h264")) {
    d.f->media_type = SPA_MEDIA_TYPE_VIDEO;
    d.f->media_subtype = SPA_MEDIA_SUBTYPE_H264;
    handle_video_size (&d);
    handle_video_framerate (&d);
  }
  return d.f;
#endif
  return NULL;
}

SpaFormat *
gst_caps_to_format (GstCaps *caps, guint index)
{
  GstCapsFeatures *f;
  GstStructure *s;
  SpaFormat *res;

  g_return_val_if_fail (GST_IS_CAPS (caps), NULL);
  g_return_val_if_fail (gst_caps_is_fixed (caps), NULL);

  f = gst_caps_get_features (caps, index);
  s = gst_caps_get_structure (caps, index);

  res = convert_1 (f, s);

  return res;
}

static gboolean
foreach_func (GstCapsFeatures *features,
              GstStructure    *structure,
              GPtrArray       *array)
{
  SpaFormat *fmt;

  if ((fmt = convert_1 (features, structure)))
    g_ptr_array_insert (array, -1, fmt);

  return TRUE;
}


GPtrArray *
gst_caps_to_format_all (GstCaps *caps)
{
  GPtrArray *res;

  res = g_ptr_array_new_full (gst_caps_get_size (caps), (GDestroyNotify)g_free);
  gst_caps_foreach (caps, (GstCapsForeachFunc) foreach_func, res);

  return res;
}

GstCaps *
gst_caps_from_format (const SpaFormat *format)
{
  GstCaps *res = NULL;

  if (format->media_type == SPA_MEDIA_TYPE_VIDEO) {
    SpaVideoInfo f;

    if (spa_format_video_parse (format, &f) < 0)
      return NULL;

    if (format->media_subtype == SPA_MEDIA_SUBTYPE_RAW) {
      res = gst_caps_new_simple ("video/x-raw",
          "format", G_TYPE_STRING, gst_video_format_to_string (f.info.raw.format),
          "width", G_TYPE_INT, f.info.raw.size.width,
          "height", G_TYPE_INT, f.info.raw.size.height,
          "framerate", GST_TYPE_FRACTION, f.info.raw.framerate.num, f.info.raw.framerate.denom,
          NULL);
    }
    else if (format->media_subtype == SPA_MEDIA_SUBTYPE_MJPG) {
      res = gst_caps_new_simple ("image/jpeg",
          "width", G_TYPE_INT, f.info.mjpg.size.width,
          "height", G_TYPE_INT, f.info.mjpg.size.height,
          "framerate", GST_TYPE_FRACTION, f.info.mjpg.framerate.num, f.info.mjpg.framerate.denom,
          NULL);
    }
    else if (format->media_subtype == SPA_MEDIA_SUBTYPE_H264) {
      res = gst_caps_new_simple ("video/x-h264",
          "width", G_TYPE_INT, f.info.h264.size.width,
          "height", G_TYPE_INT, f.info.h264.size.height,
          "framerate", GST_TYPE_FRACTION, f.info.h264.framerate.num, f.info.h264.framerate.denom,
          "stream-format", G_TYPE_STRING, "byte-stream",
          "alignment", G_TYPE_STRING, "au",
          NULL);
    }
  } else if (format->media_type == SPA_MEDIA_TYPE_AUDIO) {
    SpaAudioInfo f;

    if (spa_format_audio_parse (format, &f) < 0)
      return NULL;

    if (format->media_subtype == SPA_MEDIA_SUBTYPE_RAW) {
      res = gst_caps_new_simple ("audio/x-raw",
          "format", G_TYPE_STRING, gst_audio_format_to_string (f.info.raw.format),
          "layout", G_TYPE_STRING, "interleaved",
          "rate", G_TYPE_INT, f.info.raw.rate,
          "channels", G_TYPE_INT, f.info.raw.channels,
          NULL);
    }
  }
  return res;
}
