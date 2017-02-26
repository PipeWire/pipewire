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
#include <spa/include/spa/format-builder.h>

#include "gstpinosformat.h"

typedef struct {
  const char *name;
  uint32_t media_type;
  uint32_t media_subtype;
} MediaType;

static const MediaType media_types[] = {
  { "video/x-raw", SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { "audio/x-raw", SPA_MEDIA_TYPE_AUDIO, SPA_MEDIA_SUBTYPE_RAW },
  { "image/jpeg", SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_MJPG },
  { "video/x-h264", SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_H264 },
  { NULL, }
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
  for (i = 0; media_types[i].name; i++) {
    if (!strcmp (media_types[i].name, name))
      return &media_types[i];
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
                                   SPA_PROP_ID_VIDEO_FORMAT,
                                   get_range_type (value) | SPA_POD_PROP_FLAG_READWRITE);

      spa_pod_builder_int (&d->b, gst_video_format_from_string (v));
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
                                   SPA_PROP_ID_VIDEO_SIZE,
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
                                   SPA_PROP_ID_VIDEO_FRAMERATE,
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
                                   SPA_PROP_ID_AUDIO_FORMAT,
                                   get_range_type (value) | SPA_POD_PROP_FLAG_READWRITE);

      spa_pod_builder_int (&d->b, gst_audio_format_from_string (v));
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
                                   SPA_PROP_ID_AUDIO_LAYOUT,
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
                                   SPA_PROP_ID_AUDIO_RATE,
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
                                   SPA_PROP_ID_AUDIO_CHANNELS,
                                   get_range_type (value) | SPA_POD_PROP_FLAG_READWRITE);

      spa_pod_builder_int (&d->b, v);
    }
    if (i > 1)
      SPA_POD_BUILDER_DEREF (&d->b, f.ref, SpaPODProp)->body.flags |= SPA_POD_PROP_FLAG_UNSET;
    spa_pod_builder_pop (&d->b, &f);
  }
  return TRUE;
}

static off_t
write_pod (SpaPODBuilder *b, off_t ref, const void *data, size_t size)
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

  spa_pod_builder_push_format (&d.b, &f,
                               d.type->media_type,
                               d.type->media_subtype);

  if (d.type->media_type == SPA_MEDIA_TYPE_VIDEO)
    handle_video_fields (&d);
  else if (d.type->media_type == SPA_MEDIA_TYPE_AUDIO)
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
