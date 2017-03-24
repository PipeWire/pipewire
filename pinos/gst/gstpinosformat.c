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

#define _GNU_SOURCE
#include <stdio.h>

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>

#include <spa/lib/mapper.h>
#include <spa/include/spa/format-builder.h>
#include <spa/include/spa/video/format-utils.h>
#include <spa/include/spa/audio/format-utils.h>

#include "gstpinosformat.h"

typedef struct {
  const char *name;
  uint32_t *media_type;
  uint32_t *media_subtype;
} MediaType;

static struct {
  uint32_t format;
  SpaTypeMediaType media_type;
  SpaTypeMediaSubtype media_subtype;
  SpaTypeMediaSubtypeVideo media_subtype_video;
  SpaTypeMediaSubtypeAudio media_subtype_audio;
  SpaTypeFormatVideo format_video;
  SpaTypeFormatAudio format_audio;
  SpaTypeVideoFormat video_format;
  SpaTypeAudioFormat audio_format;
} type = { 0, };

static void
ensure_types (void)
{
  SpaTypeMap *map = spa_type_map_get_default ();

  type.format = spa_type_map_get_id (map, SPA_TYPE__Format);
  spa_type_media_type_map (map, &type.media_type);
  spa_type_media_subtype_map (map, &type.media_subtype);
  spa_type_media_subtype_video_map (map, &type.media_subtype_video);
  spa_type_media_subtype_audio_map (map, &type.media_subtype_audio);
  spa_type_format_video_map (map, &type.format_video);
  spa_type_format_audio_map (map, &type.format_audio);
  spa_type_video_format_map (map, &type.video_format);
  spa_type_audio_format_map (map, &type.audio_format);
}

static const MediaType media_type_map[] = {
  { "video/x-raw", &type.media_type.video, &type.media_subtype.raw },
  { "audio/x-raw", &type.media_type.audio, &type.media_subtype.raw },
  { "image/jpeg", &type.media_type.video, &type.media_subtype_video.mjpg },
  { "video/x-h264", &type.media_type.video, &type.media_subtype_video.h264 },
  { NULL, }
};

static const uint32_t *video_format_map[] = {
  &type.video_format.UNKNOWN,
  &type.video_format.ENCODED,
  &type.video_format.I420,
  &type.video_format.YV12,
  &type.video_format.YUY2,
  &type.video_format.UYVY,
  &type.video_format.AYUV,
  &type.video_format.RGBx,
  &type.video_format.BGRx,
  &type.video_format.xRGB,
  &type.video_format.xBGR,
  &type.video_format.RGBA,
  &type.video_format.BGRA,
  &type.video_format.ARGB,
  &type.video_format.ABGR,
  &type.video_format.RGB,
  &type.video_format.BGR,
  &type.video_format.Y41B,
  &type.video_format.Y42B,
  &type.video_format.YVYU,
  &type.video_format.Y444,
  &type.video_format.v210,
  &type.video_format.v216,
  &type.video_format.NV12,
  &type.video_format.NV21,
  &type.video_format.GRAY8,
  &type.video_format.GRAY16_BE,
  &type.video_format.GRAY16_LE,
  &type.video_format.v308,
  &type.video_format.RGB16,
  &type.video_format.BGR16,
  &type.video_format.RGB15,
  &type.video_format.BGR15,
  &type.video_format.UYVP,
  &type.video_format.A420,
  &type.video_format.RGB8P,
  &type.video_format.YUV9,
  &type.video_format.YVU9,
  &type.video_format.IYU1,
  &type.video_format.ARGB64,
  &type.video_format.AYUV64,
  &type.video_format.r210,
  &type.video_format.I420_10BE,
  &type.video_format.I420_10LE,
  &type.video_format.I422_10BE,
  &type.video_format.I422_10LE,
  &type.video_format.Y444_10BE,
  &type.video_format.Y444_10LE,
  &type.video_format.GBR,
  &type.video_format.GBR_10BE,
  &type.video_format.GBR_10LE,
  &type.video_format.NV16,
  &type.video_format.NV24,
  &type.video_format.NV12_64Z32,
  &type.video_format.A420_10BE,
  &type.video_format.A420_10LE,
  &type.video_format.A422_10BE,
  &type.video_format.A422_10LE,
  &type.video_format.A444_10BE,
  &type.video_format.A444_10LE,
  &type.video_format.NV61,
  &type.video_format.P010_10BE,
  &type.video_format.P010_10LE,
  &type.video_format.IYU2,
  &type.video_format.VYUY,
};

#if __BYTE_ORDER == __BIG_ENDIAN
#define _FORMAT_LE(fmt)  &type.audio_format. fmt ## _OE
#define _FORMAT_BE(fmt)  &type.audio_format. fmt
#elif __BYTE_ORDER == __LITTLE_ENDIAN
#define _FORMAT_LE(fmt)  &type.audio_format. fmt
#define _FORMAT_BE(fmt)  &type.audio_format. fmt ## _OE
#endif

static const uint32_t *audio_format_map[] = {
  &type.audio_format.UNKNOWN,
  &type.audio_format.ENCODED,
  &type.audio_format.S8,
  &type.audio_format.U8,
  _FORMAT_LE (S16),
  _FORMAT_BE (S16),
  _FORMAT_LE (U16),
  _FORMAT_BE (U16),
  _FORMAT_LE (S24_32),
  _FORMAT_BE (S24_32),
  _FORMAT_LE (U24_32),
  _FORMAT_BE (U24_32),
  _FORMAT_LE (S32),
  _FORMAT_BE (S32),
  _FORMAT_LE (U32),
  _FORMAT_BE (U32),
  _FORMAT_LE (S24),
  _FORMAT_BE (S24),
  _FORMAT_LE (U24),
  _FORMAT_BE (U24),
  _FORMAT_LE (S20),
  _FORMAT_BE (S20),
  _FORMAT_LE (U20),
  _FORMAT_BE (U20),
  _FORMAT_LE (S18),
  _FORMAT_BE (S18),
  _FORMAT_LE (U18),
  _FORMAT_BE (U18),
  _FORMAT_LE (F32),
  _FORMAT_BE (F32),
  _FORMAT_LE (F64),
  _FORMAT_BE (F64),
};

typedef struct {
  SpaPODBuilder b;
  const MediaType *type;
  const GstCapsFeatures *cf;
  const GstStructure *cs;
} ConvertData;

static const MediaType *
find_media_types (const char *name)
{
  int i;
  for (i = 0; media_type_map[i].name; i++) {
    if (!strcmp (media_type_map[i].name, name))
      return &media_type_map[i];
  }
  return NULL;
}

static const char *
get_nth_string (const GValue *val, int idx)
{
  const GValue *v = NULL;
  GType type = G_VALUE_TYPE (val);

  if (type == G_TYPE_STRING && idx == 0)
    v = val;
  else if (type == GST_TYPE_LIST) {
    GArray *array = g_value_peek_pointer (val);
    if (idx < array->len + 1) {
      v = &g_array_index (array, GValue, SPA_MAX (idx - 1, 0));
    }
  }
  if (v)
    return g_value_get_string (v);

  return NULL;
}

static bool
get_nth_int (const GValue *val, int idx, int *res)
{
  const GValue *v = NULL;
  GType type = G_VALUE_TYPE (val);

  if (type == G_TYPE_INT && idx == 0) {
    v = val;
  } else if (type == GST_TYPE_INT_RANGE) {
    if (idx == 0 || idx == 1) {
      *res = gst_value_get_int_range_min (val);
      return true;
    } else if (idx == 2) {
      *res = gst_value_get_int_range_max (val);
      return true;
    }
  } else if (type == GST_TYPE_LIST) {
    GArray *array = g_value_peek_pointer (val);
    if (idx < array->len + 1) {
      v = &g_array_index (array, GValue, SPA_MAX (idx - 1, 0));
    }
  }
  if (v) {
    *res = g_value_get_int (v);
    return true;
  }
  return false;
}

static gboolean
get_nth_fraction (const GValue *val, int idx, SpaFraction *f)
{
  const GValue *v = NULL;
  GType type = G_VALUE_TYPE (val);

  if (type == GST_TYPE_FRACTION && idx == 0) {
    v = val;
  } else if (type == GST_TYPE_FRACTION_RANGE) {
    if (idx == 0 || idx == 1) {
      v = gst_value_get_fraction_range_min (val);
    } else if (idx == 2) {
      v = gst_value_get_fraction_range_max (val);
    }
  } else if (type == GST_TYPE_LIST) {
    GArray *array = g_value_peek_pointer (val);
    if (idx < array->len + 1) {
      v = &g_array_index (array, GValue, SPA_MAX (idx-1, 0));
    }
  }
  if (v) {
    f->num = gst_value_get_fraction_numerator (v);
    f->denom = gst_value_get_fraction_denominator (v);
    return true;
  }
  return false;
}

static gboolean
get_nth_rectangle (const GValue *width, const GValue *height, int idx, SpaRectangle *r)
{
  const GValue *w = NULL, *h = NULL;
  GType wt = G_VALUE_TYPE (width);
  GType ht = G_VALUE_TYPE (height);

  if (wt == G_TYPE_INT && ht == G_TYPE_INT && idx == 0) {
    w = width;
    h = height;
  } else if (wt == GST_TYPE_INT_RANGE && ht == GST_TYPE_INT_RANGE) {
    if (idx == 0 || idx == 1) {
      r->width = gst_value_get_int_range_min (width);
      r->height = gst_value_get_int_range_min (height);
      return true;
    } else if (idx == 2) {
      r->width = gst_value_get_int_range_max (width);
      r->height = gst_value_get_int_range_max (height);
      return true;
    }
  } else if (wt == GST_TYPE_LIST && ht == GST_TYPE_LIST) {
    GArray *wa = g_value_peek_pointer (width);
    GArray *ha = g_value_peek_pointer (height);
    if (idx < wa->len + 1)
      w = &g_array_index (wa, GValue, SPA_MAX (idx-1, 0));
    if (idx < ha->len + 1)
      h = &g_array_index (ha, GValue, SPA_MAX (idx-1, 0));
  }
  if (w && h) {
    r->width = g_value_get_int (w);
    r->height = g_value_get_int (h);
    return true;
  }
  return false;
}

static const uint32_t
get_range_type (const GValue *val)
{
  GType type = G_VALUE_TYPE (val);

  if (type == GST_TYPE_LIST)
    return SPA_POD_PROP_RANGE_ENUM;
  if (type == GST_TYPE_DOUBLE_RANGE || type == GST_TYPE_FRACTION_RANGE)
    return SPA_POD_PROP_RANGE_MIN_MAX;
  if (type == GST_TYPE_INT_RANGE) {
    if (gst_value_get_int_range_step (val) == 1)
      return SPA_POD_PROP_RANGE_MIN_MAX;
    else
      return SPA_POD_PROP_RANGE_STEP;
  }
  if (type == GST_TYPE_INT64_RANGE) {
    if (gst_value_get_int64_range_step (val) == 1)
      return SPA_POD_PROP_RANGE_MIN_MAX;
    else
      return SPA_POD_PROP_RANGE_STEP;
  }
  return SPA_POD_PROP_RANGE_NONE;
}

static const uint32_t
get_range_type2 (const GValue *v1, const GValue *v2)
{
  uint32_t r1, r2;

  r1 = get_range_type (v1);
  r2 = get_range_type (v2);

  if (r1 == r2)
    return r1;
  if (r1 == SPA_POD_PROP_RANGE_STEP || r2 == SPA_POD_PROP_RANGE_STEP)
    return SPA_POD_PROP_RANGE_STEP;
  if (r1 == SPA_POD_PROP_RANGE_MIN_MAX || r2 == SPA_POD_PROP_RANGE_MIN_MAX)
    return SPA_POD_PROP_RANGE_MIN_MAX;
  return SPA_POD_PROP_RANGE_MIN_MAX;
}

static gboolean
handle_video_fields (ConvertData *d)
{
  SpaPODFrame f;
  const GValue *value, *value2;
  int i;

  value = gst_structure_get_value (d->cs, "format");
  if (value) {
    const char *v;
    for (i = 0; (v = get_nth_string (value, i)); i++) {
      if (i == 0)
        spa_pod_builder_push_prop (&d->b, &f,
                                   type.format_video.format,
                                   get_range_type (value) | SPA_POD_PROP_FLAG_READWRITE);

      spa_pod_builder_id (&d->b, *video_format_map[gst_video_format_from_string (v)]);
    }
    if (i > 1)
      SPA_POD_BUILDER_DEREF (&d->b, f.ref, SpaPODProp)->body.flags |= SPA_POD_PROP_FLAG_UNSET;
    spa_pod_builder_pop (&d->b, &f);
  }
  value = gst_structure_get_value (d->cs, "width");
  value2 = gst_structure_get_value (d->cs, "height");
  if (value || value2) {
    SpaRectangle v;
    for (i = 0; get_nth_rectangle (value, value2, i, &v); i++) {
      if (i == 0)
        spa_pod_builder_push_prop (&d->b, &f,
                                   type.format_video.size,
                                   get_range_type2 (value, value2) | SPA_POD_PROP_FLAG_READWRITE);

      spa_pod_builder_rectangle (&d->b, v.width, v.height);
    }
    if (i > 1)
      SPA_POD_BUILDER_DEREF (&d->b, f.ref, SpaPODProp)->body.flags |= SPA_POD_PROP_FLAG_UNSET;
    spa_pod_builder_pop (&d->b, &f);
  }

  value = gst_structure_get_value (d->cs, "framerate");
  if (value) {
    SpaFraction v;
    for (i = 0; get_nth_fraction (value, i, &v); i++) {
      if (i == 0)
        spa_pod_builder_push_prop (&d->b, &f,
                                   type.format_video.framerate,
                                   get_range_type (value) | SPA_POD_PROP_FLAG_READWRITE);

      spa_pod_builder_fraction (&d->b, v.num, v.denom);
    }
    if (i > 1)
      SPA_POD_BUILDER_DEREF (&d->b, f.ref, SpaPODProp)->body.flags |= SPA_POD_PROP_FLAG_UNSET;
    spa_pod_builder_pop (&d->b, &f);
  }
  return TRUE;
}

static gboolean
handle_audio_fields (ConvertData *d)
{
  SpaPODFrame f;
  const GValue *value;
  int i = 0;

  value = gst_structure_get_value (d->cs, "format");
  if (value) {
    const char *v;
    for (i = 0; (v = get_nth_string (value, i)); i++) {
      if (i == 0)
        spa_pod_builder_push_prop (&d->b, &f,
                                   type.format_audio.format,
                                   get_range_type (value) | SPA_POD_PROP_FLAG_READWRITE);

      spa_pod_builder_id (&d->b, *audio_format_map[gst_audio_format_from_string (v)]);
    }
    if (i > 1)
      SPA_POD_BUILDER_DEREF (&d->b, f.ref, SpaPODProp)->body.flags |= SPA_POD_PROP_FLAG_UNSET;
    spa_pod_builder_pop (&d->b, &f);
  }

  value = gst_structure_get_value (d->cs, "layout");
  if (value) {
    const char *v;
    for (i = 0; (v = get_nth_string (value, i)); i++) {
      SpaAudioLayout layout;

      if (!strcmp (v, "interleaved"))
        layout = SPA_AUDIO_LAYOUT_INTERLEAVED;
      else if (!strcmp (v, "non-interleaved"))
        layout = SPA_AUDIO_LAYOUT_NON_INTERLEAVED;
      else
        break;

      if (i == 0)
        spa_pod_builder_push_prop (&d->b, &f,
                                   type.format_audio.layout,
                                   get_range_type (value) | SPA_POD_PROP_FLAG_READWRITE);

      spa_pod_builder_int (&d->b, layout);
    }
    if (i > 1)
      SPA_POD_BUILDER_DEREF (&d->b, f.ref, SpaPODProp)->body.flags |= SPA_POD_PROP_FLAG_UNSET;
    spa_pod_builder_pop (&d->b, &f);
  }
  value = gst_structure_get_value (d->cs, "rate");
  if (value) {
    int v;
    for (i = 0; get_nth_int (value, i, &v); i++) {
      if (i == 0)
        spa_pod_builder_push_prop (&d->b, &f,
                                   type.format_audio.rate,
                                   get_range_type (value) | SPA_POD_PROP_FLAG_READWRITE);

      spa_pod_builder_int (&d->b, v);
    }
    if (i > 1)
      SPA_POD_BUILDER_DEREF (&d->b, f.ref, SpaPODProp)->body.flags |= SPA_POD_PROP_FLAG_UNSET;
    spa_pod_builder_pop (&d->b, &f);
  }
  value = gst_structure_get_value (d->cs, "channels");
  if (value) {
    int v;
    for (i = 0; get_nth_int (value, i, &v); i++) {
      if (i == 0)
        spa_pod_builder_push_prop (&d->b, &f,
                                   type.format_audio.channels,
                                   get_range_type (value) | SPA_POD_PROP_FLAG_READWRITE);

      spa_pod_builder_int (&d->b, v);
    }
    if (i > 1)
      SPA_POD_BUILDER_DEREF (&d->b, f.ref, SpaPODProp)->body.flags |= SPA_POD_PROP_FLAG_UNSET;
    spa_pod_builder_pop (&d->b, &f);
  }
  return TRUE;
}

static uint32_t
write_pod (SpaPODBuilder *b, uint32_t ref, const void *data, uint32_t size)
{
  if (ref == -1)
    ref = b->offset;

  if (b->size <= b->offset) {
    b->size = SPA_ROUND_UP_N (b->offset + size, 512);
    b->data = realloc (b->data, b->size);
  }
  memcpy (b->data + ref, data, size);
  return ref;
}

static SpaFormat *
convert_1 (GstCapsFeatures *cf, GstStructure *cs)
{
  ConvertData d;
  SpaPODFrame f;

  spa_zero (d);
  d.cf = cf;
  d.cs = cs;

  if (!(d.type = find_media_types (gst_structure_get_name (cs))))
    return NULL;

  d.b.write = write_pod;

  spa_pod_builder_push_format (&d.b, &f, type.format,
                               *d.type->media_type,
                               *d.type->media_subtype);

  if (*d.type->media_type == type.media_type.video)
    handle_video_fields (&d);
  else if (*d.type->media_type == type.media_type.audio)
    handle_audio_fields (&d);

  spa_pod_builder_pop (&d.b, &f);

  return SPA_MEMBER (d.b.data, 0, SpaFormat);
}

SpaFormat *
gst_caps_to_format (GstCaps *caps, guint index)
{
  GstCapsFeatures *f;
  GstStructure *s;
  SpaFormat *res;

  g_return_val_if_fail (GST_IS_CAPS (caps), NULL);
  g_return_val_if_fail (gst_caps_is_fixed (caps), NULL);

  ensure_types();

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

  ensure_types();

  res = g_ptr_array_new_full (gst_caps_get_size (caps), (GDestroyNotify)g_free);
  gst_caps_foreach (caps, (GstCapsForeachFunc) foreach_func, res);

  return res;
}

GstCaps *
gst_caps_from_format (const SpaFormat *format)
{
  GstCaps *res = NULL;
  uint32_t media_type, media_subtype;

  ensure_types();

  media_type = format->body.media_type.value;
  media_subtype = format->body.media_subtype.value;

  if (media_type == type.media_type.video) {
    SpaVideoInfo f;

    if (spa_format_video_parse (format, &f) < 0)
      return NULL;

    if (media_subtype == type.media_subtype.raw) {
      const char * str = spa_type_map_get_type (spa_type_map_get_default (), f.info.raw.format);

      res = gst_caps_new_simple ("video/x-raw",
          "format", G_TYPE_STRING, rindex (str, ':') + 1,
          "width", G_TYPE_INT, f.info.raw.size.width,
          "height", G_TYPE_INT, f.info.raw.size.height,
          "framerate", GST_TYPE_FRACTION, f.info.raw.framerate.num, f.info.raw.framerate.denom,
          NULL);
    }
    else if (media_subtype == type.media_subtype_video.mjpg) {
      res = gst_caps_new_simple ("image/jpeg",
          "width", G_TYPE_INT, f.info.mjpg.size.width,
          "height", G_TYPE_INT, f.info.mjpg.size.height,
          "framerate", GST_TYPE_FRACTION, f.info.mjpg.framerate.num, f.info.mjpg.framerate.denom,
          NULL);
    }
    else if (media_subtype == type.media_subtype_video.h264) {
      res = gst_caps_new_simple ("video/x-h264",
          "width", G_TYPE_INT, f.info.h264.size.width,
          "height", G_TYPE_INT, f.info.h264.size.height,
          "framerate", GST_TYPE_FRACTION, f.info.h264.framerate.num, f.info.h264.framerate.denom,
          "stream-format", G_TYPE_STRING, "byte-stream",
          "alignment", G_TYPE_STRING, "au",
          NULL);
    }
  } else if (media_type == type.media_type.audio) {
    SpaAudioInfo f;

    if (spa_format_audio_parse (format, &f) < 0)
      return NULL;

    if (media_subtype == type.media_subtype.raw) {
      const char * str = spa_type_map_get_type (spa_type_map_get_default (), f.info.raw.format);

      res = gst_caps_new_simple ("audio/x-raw",
          "format", G_TYPE_STRING, rindex (str, ':') + 1,
          "layout", G_TYPE_STRING, "interleaved",
          "rate", G_TYPE_INT, f.info.raw.rate,
          "channels", G_TYPE_INT, f.info.raw.channels,
          NULL);
    }
  }
  return res;
}
