/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <stdio.h>

#include <gst/gst.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>

#include <spa/utils/string.h>
#include <spa/utils/type.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/dynamic.h>

#include "gstpipewireformat.h"

#ifndef DRM_FORMAT_INVALID
#define DRM_FORMAT_INVALID 0
#endif

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0
#endif

struct media_type {
  const char *name;
  uint32_t media_type;
  uint32_t media_subtype;
};

static const struct media_type media_type_map[] = {
  { "video/x-raw", SPA_MEDIA_TYPE_video, SPA_MEDIA_SUBTYPE_raw },
  { "audio/x-raw", SPA_MEDIA_TYPE_audio, SPA_MEDIA_SUBTYPE_raw },
  { "image/jpeg", SPA_MEDIA_TYPE_video, SPA_MEDIA_SUBTYPE_mjpg },
  { "video/x-jpeg", SPA_MEDIA_TYPE_video, SPA_MEDIA_SUBTYPE_mjpg },
  { "video/x-h264", SPA_MEDIA_TYPE_video, SPA_MEDIA_SUBTYPE_h264 },
  { "video/x-h265", SPA_MEDIA_TYPE_video, SPA_MEDIA_SUBTYPE_h265 },
  { "audio/x-mulaw", SPA_MEDIA_TYPE_audio, SPA_MEDIA_SUBTYPE_raw },
  { "audio/x-alaw", SPA_MEDIA_TYPE_audio, SPA_MEDIA_SUBTYPE_raw },
  { "audio/mpeg", SPA_MEDIA_TYPE_audio, SPA_MEDIA_SUBTYPE_mp3 },
  { "audio/x-flac", SPA_MEDIA_TYPE_audio, SPA_MEDIA_SUBTYPE_flac },
  { NULL, }
};

static const uint32_t video_format_map[] = {
  SPA_VIDEO_FORMAT_UNKNOWN,
  SPA_VIDEO_FORMAT_ENCODED,
  SPA_VIDEO_FORMAT_I420,
  SPA_VIDEO_FORMAT_YV12,
  SPA_VIDEO_FORMAT_YUY2,
  SPA_VIDEO_FORMAT_UYVY,
  SPA_VIDEO_FORMAT_AYUV,
  SPA_VIDEO_FORMAT_RGBx,
  SPA_VIDEO_FORMAT_BGRx,
  SPA_VIDEO_FORMAT_xRGB,
  SPA_VIDEO_FORMAT_xBGR,
  SPA_VIDEO_FORMAT_RGBA,
  SPA_VIDEO_FORMAT_BGRA,
  SPA_VIDEO_FORMAT_ARGB,
  SPA_VIDEO_FORMAT_ABGR,
  SPA_VIDEO_FORMAT_RGB,
  SPA_VIDEO_FORMAT_BGR,
  SPA_VIDEO_FORMAT_Y41B,
  SPA_VIDEO_FORMAT_Y42B,
  SPA_VIDEO_FORMAT_YVYU,
  SPA_VIDEO_FORMAT_Y444,
  SPA_VIDEO_FORMAT_v210,
  SPA_VIDEO_FORMAT_v216,
  SPA_VIDEO_FORMAT_NV12,
  SPA_VIDEO_FORMAT_NV21,
  SPA_VIDEO_FORMAT_GRAY8,
  SPA_VIDEO_FORMAT_GRAY16_BE,
  SPA_VIDEO_FORMAT_GRAY16_LE,
  SPA_VIDEO_FORMAT_v308,
  SPA_VIDEO_FORMAT_RGB16,
  SPA_VIDEO_FORMAT_BGR16,
  SPA_VIDEO_FORMAT_RGB15,
  SPA_VIDEO_FORMAT_BGR15,
  SPA_VIDEO_FORMAT_UYVP,
  SPA_VIDEO_FORMAT_A420,
  SPA_VIDEO_FORMAT_RGB8P,
  SPA_VIDEO_FORMAT_YUV9,
  SPA_VIDEO_FORMAT_YVU9,
  SPA_VIDEO_FORMAT_IYU1,
  SPA_VIDEO_FORMAT_ARGB64,
  SPA_VIDEO_FORMAT_AYUV64,
  SPA_VIDEO_FORMAT_r210,
  SPA_VIDEO_FORMAT_I420_10BE,
  SPA_VIDEO_FORMAT_I420_10LE,
  SPA_VIDEO_FORMAT_I422_10BE,
  SPA_VIDEO_FORMAT_I422_10LE,
  SPA_VIDEO_FORMAT_Y444_10BE,
  SPA_VIDEO_FORMAT_Y444_10LE,
  SPA_VIDEO_FORMAT_GBR,
  SPA_VIDEO_FORMAT_GBR_10BE,
  SPA_VIDEO_FORMAT_GBR_10LE,
  SPA_VIDEO_FORMAT_NV16,
  SPA_VIDEO_FORMAT_NV24,
  SPA_VIDEO_FORMAT_NV12_64Z32,
  SPA_VIDEO_FORMAT_A420_10BE,
  SPA_VIDEO_FORMAT_A420_10LE,
  SPA_VIDEO_FORMAT_A422_10BE,
  SPA_VIDEO_FORMAT_A422_10LE,
  SPA_VIDEO_FORMAT_A444_10BE,
  SPA_VIDEO_FORMAT_A444_10LE,
  SPA_VIDEO_FORMAT_NV61,
  SPA_VIDEO_FORMAT_P010_10BE,
  SPA_VIDEO_FORMAT_P010_10LE,
  SPA_VIDEO_FORMAT_IYU2,
  SPA_VIDEO_FORMAT_VYUY,
  SPA_VIDEO_FORMAT_GBRA,
  SPA_VIDEO_FORMAT_GBRA_10BE,
  SPA_VIDEO_FORMAT_GBRA_10LE,
  SPA_VIDEO_FORMAT_GBR_12BE,
  SPA_VIDEO_FORMAT_GBR_12LE,
  SPA_VIDEO_FORMAT_GBRA_12BE,
  SPA_VIDEO_FORMAT_GBRA_12LE,
  SPA_VIDEO_FORMAT_I420_12BE,
  SPA_VIDEO_FORMAT_I420_12LE,
  SPA_VIDEO_FORMAT_I422_12BE,
  SPA_VIDEO_FORMAT_I422_12LE,
  SPA_VIDEO_FORMAT_Y444_12BE,
  SPA_VIDEO_FORMAT_Y444_12LE,
};

static const uint32_t color_range_map[] = {
  SPA_VIDEO_COLOR_RANGE_UNKNOWN,
  SPA_VIDEO_COLOR_RANGE_0_255,
  SPA_VIDEO_COLOR_RANGE_16_235,
};

static const uint32_t color_matrix_map[] = {
  SPA_VIDEO_COLOR_MATRIX_UNKNOWN,
  SPA_VIDEO_COLOR_MATRIX_RGB,
  SPA_VIDEO_COLOR_MATRIX_FCC,
  SPA_VIDEO_COLOR_MATRIX_BT709,
  SPA_VIDEO_COLOR_MATRIX_BT601,
  SPA_VIDEO_COLOR_MATRIX_SMPTE240M,
  SPA_VIDEO_COLOR_MATRIX_BT2020,
};

static const uint32_t transfer_function_map[] = {
  SPA_VIDEO_TRANSFER_UNKNOWN,
  SPA_VIDEO_TRANSFER_GAMMA10,
  SPA_VIDEO_TRANSFER_GAMMA18,
  SPA_VIDEO_TRANSFER_GAMMA20,
  SPA_VIDEO_TRANSFER_GAMMA22,
  SPA_VIDEO_TRANSFER_BT709,
  SPA_VIDEO_TRANSFER_SMPTE240M,
  SPA_VIDEO_TRANSFER_SRGB,
  SPA_VIDEO_TRANSFER_GAMMA28,
  SPA_VIDEO_TRANSFER_LOG100,
  SPA_VIDEO_TRANSFER_LOG316,
  SPA_VIDEO_TRANSFER_BT2020_12,
  SPA_VIDEO_TRANSFER_ADOBERGB,
  SPA_VIDEO_TRANSFER_BT2020_10,
  SPA_VIDEO_TRANSFER_SMPTE2084,
  SPA_VIDEO_TRANSFER_ARIB_STD_B67,
  SPA_VIDEO_TRANSFER_BT601,
};

static const uint32_t color_primaries_map[] = {
  SPA_VIDEO_COLOR_PRIMARIES_UNKNOWN,
  SPA_VIDEO_COLOR_PRIMARIES_BT709,
  SPA_VIDEO_COLOR_PRIMARIES_BT470M,
  SPA_VIDEO_COLOR_PRIMARIES_BT470BG,
  SPA_VIDEO_COLOR_PRIMARIES_SMPTE170M,
  SPA_VIDEO_COLOR_PRIMARIES_SMPTE240M,
  SPA_VIDEO_COLOR_PRIMARIES_FILM,
  SPA_VIDEO_COLOR_PRIMARIES_BT2020,
  SPA_VIDEO_COLOR_PRIMARIES_ADOBERGB,
  SPA_VIDEO_COLOR_PRIMARIES_SMPTEST428,
  SPA_VIDEO_COLOR_PRIMARIES_SMPTERP431,
  SPA_VIDEO_COLOR_PRIMARIES_SMPTEEG432,
  SPA_VIDEO_COLOR_PRIMARIES_EBU3213,
};

static const uint32_t interlace_mode_map[] = {
  SPA_VIDEO_INTERLACE_MODE_PROGRESSIVE,
  SPA_VIDEO_INTERLACE_MODE_INTERLEAVED,
  SPA_VIDEO_INTERLACE_MODE_MIXED,
  SPA_VIDEO_INTERLACE_MODE_FIELDS,
};

#if __BYTE_ORDER == __BIG_ENDIAN
#define _FORMAT_LE(fmt)  SPA_AUDIO_FORMAT_ ## fmt ## _OE
#define _FORMAT_BE(fmt)  SPA_AUDIO_FORMAT_ ## fmt
#elif __BYTE_ORDER == __LITTLE_ENDIAN
#define _FORMAT_LE(fmt)  SPA_AUDIO_FORMAT_ ## fmt
#define _FORMAT_BE(fmt)  SPA_AUDIO_FORMAT_ ## fmt ## _OE
#endif

static const uint32_t audio_format_map[] = {
  SPA_AUDIO_FORMAT_UNKNOWN,
  SPA_AUDIO_FORMAT_ENCODED,
  SPA_AUDIO_FORMAT_S8,
  SPA_AUDIO_FORMAT_U8,
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
  const struct media_type *type;
  const GstCapsFeatures *cf;
  const GstStructure *cs;
  GPtrArray *array;
} ConvertData;

static const struct media_type *
find_media_types (const char *name)
{
  int i;
  for (i = 0; media_type_map[i].name; i++) {
    if (spa_streq(media_type_map[i].name, name))
      return &media_type_map[i];
  }
  return NULL;
}

static int find_index(const uint32_t *items, int n_items, uint32_t id)
{
  int i;
  for (i = 0; i < n_items; i++)
    if (items[i] == id)
      return i;
  return -1;
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
    if (idx < (int)(array->len + 1)) {
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
    if (idx < (int)(array->len + 1)) {
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
get_nth_fraction (const GValue *val, int idx, struct spa_fraction *f)
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
    if (idx < (int)(array->len + 1)) {
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
get_nth_rectangle (const GValue *width, const GValue *height, int idx, struct spa_rectangle *r)
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
    } else if (idx == 3) {
      r->width = gst_value_get_int_range_step (width);
      r->height = gst_value_get_int_range_step (height);
      if (r->width > 1 || r->height > 1)
        return true;
      else
        return false;
    }
  } else if (wt == GST_TYPE_LIST && ht == GST_TYPE_LIST) {
    GArray *wa = g_value_peek_pointer (width);
    GArray *ha = g_value_peek_pointer (height);
    if (idx < (int)(wa->len + 1))
      w = &g_array_index (wa, GValue, SPA_MAX (idx-1, 0));
    if (idx < (int)(ha->len + 1))
      h = &g_array_index (ha, GValue, SPA_MAX (idx-1, 0));
  }
  if (w && h) {
    r->width = g_value_get_int (w);
    r->height = g_value_get_int (h);
    return true;
  }
  return false;
}

static uint32_t
get_range_type (const GValue *val)
{
  GType type = G_VALUE_TYPE (val);

  if (type == GST_TYPE_LIST)
    return SPA_CHOICE_Enum;
  if (type == GST_TYPE_DOUBLE_RANGE || type == GST_TYPE_FRACTION_RANGE)
    return SPA_CHOICE_Range;
  if (type == GST_TYPE_INT_RANGE) {
    if (gst_value_get_int_range_step (val) == 1)
      return SPA_CHOICE_Range;
    else
      return SPA_CHOICE_Step;
  }
  if (type == GST_TYPE_INT64_RANGE) {
    if (gst_value_get_int64_range_step (val) == 1)
      return SPA_CHOICE_Range;
    else
      return SPA_CHOICE_Step;
  }
  return SPA_CHOICE_None;
}

static uint32_t
get_range_type2 (const GValue *v1, const GValue *v2)
{
  uint32_t r1, r2;

  r1 = get_range_type (v1);
  r2 = get_range_type (v2);

  if (r1 == r2)
    return r1;
  if (r1 == SPA_CHOICE_Step || r2 == SPA_CHOICE_Step)
    return SPA_CHOICE_Step;
  if (r1 == SPA_CHOICE_Range || r2 == SPA_CHOICE_Range)
    return SPA_CHOICE_Range;
  return SPA_CHOICE_Range;
}

static void
add_limits (struct spa_pod_dynamic_builder *b, ConvertData *d)
{
  struct spa_pod_choice *choice;
  struct spa_pod_frame f;
  const GValue *value, *value2;
  int i;

  value = gst_structure_get_value (d->cs, "width");
  value2 = gst_structure_get_value (d->cs, "height");
  if (value && value2) {
    struct spa_rectangle v;
    for (i = 0; get_nth_rectangle (value, value2, i, &v); i++) {
      if (i == 0) {
        spa_pod_builder_prop (&b->b, SPA_FORMAT_VIDEO_size, 0);
        spa_pod_builder_push_choice(&b->b, &f, get_range_type2 (value, value2), 0);
      }

      spa_pod_builder_rectangle (&b->b, v.width, v.height);
    }
    if (i > 0) {
      choice = spa_pod_builder_pop(&b->b, &f);
      if (i == 1)
        choice->body.type = SPA_CHOICE_None;
    }
  }

  value = gst_structure_get_value (d->cs, "framerate");
  if (value) {
    struct spa_fraction v;
    for (i = 0; get_nth_fraction (value, i, &v); i++) {
      if (i == 0) {
        spa_pod_builder_prop (&b->b, SPA_FORMAT_VIDEO_framerate, 0);
        spa_pod_builder_push_choice(&b->b, &f, get_range_type (value), 0);
      }

      spa_pod_builder_fraction (&b->b, v.num, v.denom);
    }
    if (i > 0) {
      choice = spa_pod_builder_pop(&b->b, &f);
      if (i == 1)
        choice->body.type = SPA_CHOICE_None;
    }
  }

  value = gst_structure_get_value (d->cs, "max-framerate");
  if (value) {
    struct spa_fraction v;
    for (i = 0; get_nth_fraction (value, i, &v); i++) {
      if (i == 0) {
        spa_pod_builder_prop (&b->b, SPA_FORMAT_VIDEO_maxFramerate, 0);
        spa_pod_builder_push_choice(&b->b, &f, get_range_type (value), 0);
      }

      spa_pod_builder_fraction (&b->b, v.num, v.denom);
    }
    if (i > 0) {
      choice = spa_pod_builder_pop(&b->b, &f);
      if (i == 1)
        choice->body.type = SPA_CHOICE_None;
    }
  }
}

static void
add_video_format (gpointer format_ptr,
                  gpointer modifiers_ptr,
                  gpointer user_data)
{
  uint32_t format = GPOINTER_TO_UINT (format_ptr);
  GHashTable *modifiers = modifiers_ptr;
  ConvertData *d = user_data;
  struct spa_pod_dynamic_builder b;
  struct spa_pod_frame f;
  int n_mods;

  spa_pod_dynamic_builder_init (&b, NULL, 0, 1024);

  spa_pod_builder_push_object (&b.b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);

  spa_pod_builder_prop (&b.b, SPA_FORMAT_mediaType, 0);
  spa_pod_builder_id(&b.b, d->type->media_type);

  spa_pod_builder_prop (&b.b, SPA_FORMAT_mediaSubtype, 0);
  spa_pod_builder_id(&b.b, d->type->media_subtype);

  spa_pod_builder_prop (&b.b, SPA_FORMAT_VIDEO_format, 0);
  spa_pod_builder_id (&b.b, format);

  n_mods = g_hash_table_size (modifiers);
  if (n_mods > 0) {
    struct spa_pod_frame f2;
    GHashTableIter iter;
    gpointer key, value;
    uint32_t flags, choice_type;

    flags = SPA_POD_PROP_FLAG_MANDATORY;
    if (n_mods > 1) {
      choice_type = SPA_CHOICE_Enum;
      flags |= SPA_POD_PROP_FLAG_DONT_FIXATE;
    } else {
      choice_type = SPA_CHOICE_None;
    }

    spa_pod_builder_prop (&b.b, SPA_FORMAT_VIDEO_modifier, flags);
    spa_pod_builder_push_choice (&b.b, &f2, choice_type, 0);

    g_hash_table_iter_init (&iter, modifiers);
    g_hash_table_iter_next (&iter, &key, &value);
    spa_pod_builder_long (&b.b, (uint64_t) key);

    if (n_mods > 1) {
      do {
        spa_pod_builder_long (&b.b, (uint64_t) key);
      } while (g_hash_table_iter_next (&iter, &key, &value));
    }

    spa_pod_builder_pop (&b.b, &f2);
  }

  add_limits (&b, d);

  g_ptr_array_add (d->array, spa_pod_builder_pop (&b.b, &f));
}

static void
handle_video_fields (ConvertData *d)
{
  g_autoptr (GHashTable) formats = NULL;
  const GValue *value;
  gboolean dmabuf_caps;
  int i;

  formats = g_hash_table_new_full (NULL, NULL, NULL,
                                   (GDestroyNotify) g_hash_table_unref);
  dmabuf_caps = (d->cf &&
                 gst_caps_features_contains (d->cf,
                                             GST_CAPS_FEATURE_MEMORY_DMABUF));

  value = gst_structure_get_value (d->cs, "format");
  if (value) {
    const char *v;

    for (i = 0; (v = get_nth_string (value, i)); i++) {
      int idx;

      idx = gst_video_format_from_string (v);
#ifdef HAVE_GSTREAMER_DMA_DRM
      if (dmabuf_caps && idx == GST_VIDEO_FORMAT_DMA_DRM) {
        const GValue *value2;

        value2 = gst_structure_get_value (d->cs, "drm-format");
        if (value2) {
          const char *v2;
          int j;

          for (j = 0; (v2 = get_nth_string (value2, j)); j++) {
            uint32_t fourcc;
            uint64_t mod;
            int idx2;

            fourcc = gst_video_dma_drm_fourcc_from_string (v2, &mod);
            idx2 = gst_video_dma_drm_fourcc_to_format (fourcc);

            if (idx2 != GST_VIDEO_FORMAT_UNKNOWN &&
                idx2 < (int)SPA_N_ELEMENTS (video_format_map)) {
              GHashTable *modifiers =
                  g_hash_table_lookup (formats,
                                       GINT_TO_POINTER (video_format_map[idx2]));
              if (!modifiers) {
                modifiers = g_hash_table_new (NULL, NULL);
                g_hash_table_insert (formats,
                                     GINT_TO_POINTER (video_format_map[idx2]),
                                     modifiers);
              }

              g_hash_table_add (modifiers, GINT_TO_POINTER (mod));
            }
          }
        }
      } else
#endif
      if (idx != GST_VIDEO_FORMAT_UNKNOWN &&
          idx < (int)SPA_N_ELEMENTS (video_format_map)) {
          GHashTable *modifiers =
              g_hash_table_lookup (formats,
                                   GINT_TO_POINTER (video_format_map[idx]));
          if (!modifiers) {
            modifiers = g_hash_table_new (NULL, NULL);
            g_hash_table_insert (formats,
                                 GINT_TO_POINTER (video_format_map[idx]),
                                 modifiers);
          }

          if (dmabuf_caps) {
            g_hash_table_add (modifiers, GINT_TO_POINTER (DRM_FORMAT_MOD_LINEAR));
            g_hash_table_add (modifiers, GINT_TO_POINTER (DRM_FORMAT_MOD_INVALID));
          }
      }
    }
  }

  if (g_hash_table_size (formats) > 0) {
    g_hash_table_foreach (formats, add_video_format, d);
  } else if (!dmabuf_caps) {
    struct spa_pod_dynamic_builder b;
    struct spa_pod_frame f;

    spa_pod_dynamic_builder_init (&b, NULL, 0, 1024);

    spa_pod_builder_push_object (&b.b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);

    spa_pod_builder_prop (&b.b, SPA_FORMAT_mediaType, 0);
    spa_pod_builder_id(&b.b, d->type->media_type);

    spa_pod_builder_prop (&b.b, SPA_FORMAT_mediaSubtype, 0);
    spa_pod_builder_id(&b.b, d->type->media_subtype);

    add_limits (&b, d);

    g_ptr_array_add (d->array, spa_pod_builder_pop (&b.b, &f));
  }
}

static void
set_default_channels (struct spa_pod_builder *b, uint32_t channels)
{
  uint32_t position[8] = {0};
  gboolean ok = TRUE;

  switch (channels) {
  case 8:
    position[6] = SPA_AUDIO_CHANNEL_SL;
    position[7] = SPA_AUDIO_CHANNEL_SR;
    SPA_FALLTHROUGH
  case 6:
    position[5] = SPA_AUDIO_CHANNEL_LFE;
    SPA_FALLTHROUGH
  case 5:
    position[4] = SPA_AUDIO_CHANNEL_FC;
    SPA_FALLTHROUGH
  case 4:
    position[2] = SPA_AUDIO_CHANNEL_RL;
    position[3] = SPA_AUDIO_CHANNEL_RR;
    SPA_FALLTHROUGH
  case 2:
    position[0] = SPA_AUDIO_CHANNEL_FL;
    position[1] = SPA_AUDIO_CHANNEL_FR;
    break;
  case 1:
    position[0] = SPA_AUDIO_CHANNEL_MONO;
    break;
  default:
    ok = FALSE;
    break;
  }

  if (ok)
    spa_pod_builder_add (b, SPA_FORMAT_AUDIO_position,
        SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Id, channels, position), 0);
}

static void
handle_audio_fields (ConvertData *d)
{
  const GValue *value;
  struct spa_pod_dynamic_builder b;
  struct spa_pod_choice *choice;
  struct spa_pod_frame f, f0;
  int i = 0;

  spa_pod_dynamic_builder_init (&b, NULL, 0, 1024);

  spa_pod_builder_push_object (&b.b, &f0, SPA_TYPE_OBJECT_Format,
                               SPA_PARAM_EnumFormat);

  spa_pod_builder_prop (&b.b, SPA_FORMAT_mediaType, 0);
  spa_pod_builder_id(&b.b, d->type->media_type);

  spa_pod_builder_prop (&b.b, SPA_FORMAT_mediaSubtype, 0);
  spa_pod_builder_id(&b.b, d->type->media_subtype);

  value = gst_structure_get_value (d->cs, "format");
  if (value) {
    const char *v;
    int idx;
    for (i = 0; (v = get_nth_string (value, i)); i++) {
      if (i == 0) {
        spa_pod_builder_prop (&b.b, SPA_FORMAT_AUDIO_format, 0);
        spa_pod_builder_push_choice(&b.b, &f, get_range_type (value), 0);
      }

      idx = gst_audio_format_from_string (v);
      if (idx < (int)SPA_N_ELEMENTS (audio_format_map))
        spa_pod_builder_id (&b.b, audio_format_map[idx]);
    }
    if (i > 0) {
      choice = spa_pod_builder_pop(&b.b, &f);
      if (i == 1)
        choice->body.type = SPA_CHOICE_None;
    }
  } else if (strcmp(d->type->name, "audio/x-mulaw") == 0) {
        spa_pod_builder_prop (&b.b, SPA_FORMAT_AUDIO_format, 0);
        spa_pod_builder_id (&b.b, SPA_AUDIO_FORMAT_ULAW);
  } else if (strcmp(d->type->name, "audio/x-alaw") == 0) {
        spa_pod_builder_prop (&b.b, SPA_FORMAT_AUDIO_format, 0);
        spa_pod_builder_id (&b.b, SPA_AUDIO_FORMAT_ALAW);
  } else if (strcmp(d->type->name, "audio/mpeg") == 0) {
        spa_pod_builder_prop (&b.b, SPA_FORMAT_AUDIO_format, 0);
        spa_pod_builder_id (&b.b, SPA_AUDIO_FORMAT_ENCODED);
  } else if (strcmp(d->type->name, "audio/x-flac") == 0) {
        spa_pod_builder_prop (&b.b, SPA_FORMAT_AUDIO_format, 0);
        spa_pod_builder_id (&b.b, SPA_AUDIO_FORMAT_ENCODED);
  }

#if 0
  value = gst_structure_get_value (d->cs, "layout");
  if (value) {
    const char *v;
    for (i = 0; (v = get_nth_string (value, i)); i++) {
      enum spa_audio_layout layout;

      if (spa_streq(v, "interleaved"))
        layout = SPA_AUDIO_LAYOUT_INTERLEAVED;
      else if (spa_streq(v, "non-interleaved"))
        layout = SPA_AUDIO_LAYOUT_NON_INTERLEAVED;
      else
        break;

      if (i == 0) {
        spa_pod_builder_prop (&b.b, SPA_FORMAT_AUDIO_layout, 0);
        spa_pod_builder_push_choice(&b.b, &f, get_range_type (value), 0);
      }

      spa_pod_builder_id (&b.b, layout);
    }
    if (i > 0) {
      choice = spa_pod_builder_pop(&b.b, &f);
      if (i == 1)
        choice->body.type = SPA_CHOICE_None;
    }
  }
#endif
  value = gst_structure_get_value (d->cs, "rate");
  if (value) {
    int v;
    for (i = 0; get_nth_int (value, i, &v); i++) {
      if (i == 0) {
        spa_pod_builder_prop (&b.b, SPA_FORMAT_AUDIO_rate, 0);
        spa_pod_builder_push_choice(&b.b, &f, get_range_type (value), 0);
      }

      spa_pod_builder_int (&b.b, v);
    }
    if (i > 0) {
      choice = spa_pod_builder_pop(&b.b, &f);
      if (i == 1)
        choice->body.type = SPA_CHOICE_None;
    }
  }
  value = gst_structure_get_value (d->cs, "channels");
  if (value) {
    int v;
    for (i = 0; get_nth_int (value, i, &v); i++) {
      if (i == 0) {
        spa_pod_builder_prop (&b.b, SPA_FORMAT_AUDIO_channels, 0);
        spa_pod_builder_push_choice(&b.b, &f, get_range_type (value), 0);
      }

      spa_pod_builder_int (&b.b, v);
    }
    if (i > 0) {
      choice = spa_pod_builder_pop(&b.b, &f);
      if (i == 1) {
        choice->body.type = SPA_CHOICE_None;
        set_default_channels (&b.b, v);
      }
    }
  }

  g_ptr_array_add (d->array, spa_pod_builder_pop (&b.b, &f0));
}

static void
handle_fields (ConvertData *d)
{
  if (!(d->type = find_media_types (gst_structure_get_name (d->cs))))
    return;

  if (d->type->media_type == SPA_MEDIA_TYPE_video)
    handle_video_fields (d);
  else if (d->type->media_type == SPA_MEDIA_TYPE_audio)
    handle_audio_fields (d);
}

static gboolean
foreach_func_dmabuf (GstCapsFeatures *features,
                     GstStructure    *structure,
                     ConvertData     *d)
{
  if (!features || !gst_caps_features_contains (features, GST_CAPS_FEATURE_MEMORY_DMABUF))
    return TRUE;

  d->cf = features;
  d->cs = structure;

  handle_fields (d);

  return TRUE;
}

static gboolean
foreach_func_no_dmabuf (GstCapsFeatures *features,
                        GstStructure    *structure,
                        ConvertData     *d)
{
  if (features && gst_caps_features_contains (features, GST_CAPS_FEATURE_MEMORY_DMABUF))
    return TRUE;

  d->cf = features;
  d->cs = structure;

  handle_fields (d);

  return TRUE;
}


GPtrArray *
gst_caps_to_format_all (GstCaps *caps)
{
  ConvertData d;

  d.array = g_ptr_array_new_full (gst_caps_get_size (caps), (GDestroyNotify)g_free);

  gst_caps_foreach (caps, (GstCapsForeachFunc) foreach_func_dmabuf, &d);
  gst_caps_foreach (caps, (GstCapsForeachFunc) foreach_func_no_dmabuf, &d);

  return d.array;
}

typedef const char *(*id_to_string_func)(uint32_t id);

static const char *video_id_to_string(uint32_t id)
{
  int idx;
  if ((idx = find_index(video_format_map, SPA_N_ELEMENTS(video_format_map), id)) == -1)
    return NULL;
  return gst_video_format_to_string(idx);
}

#ifdef HAVE_GSTREAMER_DMA_DRM
static char *video_id_to_dma_drm_fourcc(uint32_t id, uint64_t mod)
{
  int idx;
  guint32 fourcc;
  if ((idx = find_index(video_format_map, SPA_N_ELEMENTS(video_format_map), id)) == -1)
    return NULL;
  fourcc = gst_video_dma_drm_fourcc_from_format(idx);
  if (fourcc == DRM_FORMAT_INVALID)
    return NULL;
  return gst_video_dma_drm_fourcc_to_string(fourcc, mod);
}
#endif

static const char *interlace_mode_id_to_string(uint32_t id)
{
  int idx;
  if ((idx = find_index(interlace_mode_map, SPA_N_ELEMENTS(interlace_mode_map), id)) == -1)
    return NULL;
  return gst_video_interlace_mode_to_string(idx);
}

static const char *audio_id_to_string(uint32_t id)
{
  int idx;
  if ((idx = find_index(audio_format_map, SPA_N_ELEMENTS(audio_format_map), id)) == -1)
    return NULL;
  return gst_audio_format_to_string(idx);
}

static GstVideoColorRange color_range_to_gst(uint32_t id)
{
  int idx;
  if ((idx = find_index(color_range_map, SPA_N_ELEMENTS(color_range_map), id)) == -1)
    return GST_VIDEO_COLOR_RANGE_UNKNOWN;
  return idx;
}

static GstVideoColorMatrix color_matrix_to_gst(uint32_t id)
{
  int idx;
  if ((idx = find_index(color_matrix_map, SPA_N_ELEMENTS(color_matrix_map), id)) == -1)
    return GST_VIDEO_COLOR_MATRIX_UNKNOWN;
  return idx;
}

static GstVideoTransferFunction transfer_function_to_gst(uint32_t id)
{
  int idx;
  if ((idx = find_index(transfer_function_map, SPA_N_ELEMENTS(transfer_function_map), id)) == -1)
    return GST_VIDEO_TRANSFER_UNKNOWN;
  return idx;
}

static GstVideoColorPrimaries color_primaries_to_gst(uint32_t id)
{
  int idx;
  if ((idx = find_index(color_primaries_map, SPA_N_ELEMENTS(color_primaries_map), id)) == -1)
    return GST_VIDEO_COLOR_PRIMARIES_UNKNOWN;
  return idx;
}

static void colorimetry_to_gst_colorimetry(struct spa_video_colorimetry *colorimetry, GstVideoColorimetry *gst_colorimetry)
{
  gst_colorimetry->range = color_range_to_gst(colorimetry->range);
  gst_colorimetry->matrix = color_matrix_to_gst(colorimetry->matrix);
  gst_colorimetry->transfer = transfer_function_to_gst(colorimetry->transfer);
  gst_colorimetry->primaries = color_primaries_to_gst(colorimetry->primaries);
}

static void
handle_id_prop (const struct spa_pod_prop *prop, const char *key, id_to_string_func func, GstCaps *res)
{
  const char * str;
  struct spa_pod *val;
  uint32_t *id;
  uint32_t i, n_items, choice;

  val = spa_pod_get_values(&prop->value, &n_items, &choice);
  if (val->type != SPA_TYPE_Id || n_items == 0)
          return;

  id = SPA_POD_BODY(val);

  switch (choice) {
    case SPA_CHOICE_None:
      if (!(str = func(id[0])))
        return;
      gst_caps_set_simple (res, key, G_TYPE_STRING, str, NULL);
      break;
    case SPA_CHOICE_Enum:
    {
      GValue list = { 0 }, v = { 0 };

      g_value_init (&list, GST_TYPE_LIST);
      for (i = 1; i < n_items; i++) {
        if (!(str = func(id[i])))
          continue;

        g_value_init (&v, G_TYPE_STRING);
        g_value_set_string (&v, str);
        gst_value_list_append_and_take_value (&list, &v);
      }
      gst_caps_set_value (res, key, &list);
      g_value_unset (&list);
      break;
    }
    default:
      break;
  }
}

static void
handle_dmabuf_prop (const struct spa_pod_prop *prop,
    const struct spa_pod_prop *prop_modifier, GstCaps *res)
{
  g_autoptr (GPtrArray) fmt_array = NULL;
  g_autoptr (GPtrArray) drm_fmt_array = NULL;
  const struct spa_pod *pod_modifier;
  struct spa_pod *val;
  uint32_t *id, n_fmts, n_mods, choice, i, j;
  uint64_t *mods;

  val = spa_pod_get_values (&prop->value, &n_fmts, &choice);
  if (val->type != SPA_TYPE_Id || n_fmts == 0)
    return;
  if (choice != SPA_CHOICE_None && choice != SPA_CHOICE_Enum)
    return;

  id = SPA_POD_BODY (val);
  if (n_fmts > 1) {
    n_fmts--;
    id++;
  }

  pod_modifier = spa_pod_get_values (&prop_modifier->value, &n_mods, &choice);
  if (pod_modifier->type != SPA_TYPE_Long || n_mods == 0)
    return;
  if (choice != SPA_CHOICE_None && choice != SPA_CHOICE_Enum)
    return;

  mods = SPA_POD_BODY (pod_modifier);
  if (n_mods > 1) {
    n_mods--;
    mods++;
  }

  fmt_array = g_ptr_array_new_with_free_func (g_free);
  drm_fmt_array = g_ptr_array_new_with_free_func (g_free);

  for (i = 0; i < n_fmts; i++) {
    for (j = 0; j < n_mods; j++) {
      gboolean as_drm = FALSE;
      const char *fmt_str;

#ifdef HAVE_GSTREAMER_DMA_DRM
      if (mods[j] != DRM_FORMAT_MOD_INVALID) {
        char *drm_str;

        if ((drm_str = video_id_to_dma_drm_fourcc(id[i], mods[j]))) {
          g_ptr_array_add(drm_fmt_array, drm_str);
          as_drm = TRUE;
        }
      }
#endif

      if (!as_drm &&
          (mods[j] == DRM_FORMAT_MOD_LINEAR ||
           mods[j] == DRM_FORMAT_MOD_INVALID) &&
          (fmt_str = video_id_to_string(id[i])))
        g_ptr_array_add(fmt_array, g_strdup_printf ("%s", fmt_str));
    }
  }

#ifdef HAVE_GSTREAMER_DMA_DRM
  if (drm_fmt_array->len > 0) {
    g_ptr_array_add (fmt_array, g_strdup_printf ("DMA_DRM"));

    if (drm_fmt_array->len == 1) {
      gst_caps_set_simple (res, "drm-format", G_TYPE_STRING,
          g_ptr_array_index (drm_fmt_array, 0), NULL);
    } else {
      GValue list = { 0 };

      g_value_init (&list, GST_TYPE_LIST);
      for (i = 0; i < drm_fmt_array->len; i++) {
        GValue v = { 0 };

        g_value_init (&v, G_TYPE_STRING);
        g_value_set_string (&v, g_ptr_array_index (drm_fmt_array, i));
        gst_value_list_append_and_take_value (&list, &v);
      }

      gst_caps_set_value (res, "drm-format", &list);
      g_value_unset (&list);
    }
  }
#endif

  if (fmt_array->len > 0) {
    gst_caps_set_features_simple (res,
        gst_caps_features_from_string (GST_CAPS_FEATURE_MEMORY_DMABUF));

    if (fmt_array->len == 1) {
      gst_caps_set_simple (res, "format", G_TYPE_STRING,
          g_ptr_array_index (fmt_array, 0), NULL);
    } else {
      GValue list = { 0 };

      g_value_init (&list, GST_TYPE_LIST);
      for (i = 0; i < fmt_array->len; i++) {
        GValue v = { 0 };

        g_value_init (&v, G_TYPE_STRING);
        g_value_set_string (&v, g_ptr_array_index (fmt_array, i));
        gst_value_list_append_and_take_value (&list, &v);
      }

      gst_caps_set_value (res, "format", &list);
      g_value_unset (&list);
    }
  }
}

static void
handle_int_prop (const struct spa_pod_prop *prop, const char *key, GstCaps *res)
{
  struct spa_pod *val;
  uint32_t *ints;
  uint32_t i, n_items, choice;

  val = spa_pod_get_values(&prop->value, &n_items, &choice);
  if (val->type != SPA_TYPE_Int || n_items == 0)
          return;

  ints = SPA_POD_BODY(val);

  switch (choice) {
    case SPA_CHOICE_None:
      gst_caps_set_simple (res, key, G_TYPE_INT, ints[0], NULL);
      break;
    case SPA_CHOICE_Range:
    case SPA_CHOICE_Step:
    {
      if (n_items < 3)
        return;
      gst_caps_set_simple (res, key, GST_TYPE_INT_RANGE, ints[1], ints[2], NULL);
      break;
    }
    case SPA_CHOICE_Enum:
    {
      GValue list = { 0 }, v = { 0 };

      g_value_init (&list, GST_TYPE_LIST);
      for (i = 1; i < n_items; i++) {
        g_value_init (&v, G_TYPE_INT);
        g_value_set_int (&v, ints[i]);
        gst_value_list_append_and_take_value (&list, &v);
      }
      gst_caps_set_value (res, key, &list);
      g_value_unset (&list);
      break;
    }
    default:
      break;
  }
}

static void
handle_rect_prop (const struct spa_pod_prop *prop, const char *width, const char *height, GstCaps *res)
{
  struct spa_pod *val;
  struct spa_rectangle *rect;
  uint32_t i, n_items, choice;

  val = spa_pod_get_values(&prop->value, &n_items, &choice);
  if (val->type != SPA_TYPE_Rectangle || n_items == 0)
          return;

  rect = SPA_POD_BODY(val);

  switch (choice) {
    case SPA_CHOICE_None:
      gst_caps_set_simple (res, width, G_TYPE_INT, rect[0].width,
                                height, G_TYPE_INT, rect[0].height, NULL);
      break;
    case SPA_CHOICE_Range:
    case SPA_CHOICE_Step:
    {
      if (n_items < 3)
        return;

      if (rect[1].width == rect[2].width &&
          rect[1].height == rect[2].height) {
        gst_caps_set_simple (res,
            width, G_TYPE_INT, rect[1].width,
            height, G_TYPE_INT, rect[1].height,
            NULL);
      } else {
        gst_caps_set_simple (res,
            width, GST_TYPE_INT_RANGE, rect[1].width, rect[2].width,
            height, GST_TYPE_INT_RANGE, rect[1].height, rect[2].height,
            NULL);
      }
      break;
    }
    case SPA_CHOICE_Enum:
    {
      GValue l1 = { 0 }, l2 = { 0 }, v1 = { 0 }, v2 = { 0 };

      g_value_init (&l1, GST_TYPE_LIST);
      g_value_init (&l2, GST_TYPE_LIST);
      for (i = 1; i < n_items; i++) {
        g_value_init (&v1, G_TYPE_INT);
        g_value_set_int (&v1, rect[i].width);
        gst_value_list_append_and_take_value (&l1, &v1);

        g_value_init (&v2, G_TYPE_INT);
        g_value_set_int (&v2, rect[i].height);
        gst_value_list_append_and_take_value (&l2, &v2);
      }
      gst_caps_set_value (res, width, &l1);
      gst_caps_set_value (res, height, &l2);
      g_value_unset (&l1);
      g_value_unset (&l2);
      break;
    }
    default:
      break;
  }
}

static void
handle_fraction_prop (const struct spa_pod_prop *prop, const char *key, GstCaps *res)
{
  struct spa_pod *val;
  struct spa_fraction *fract;
  uint32_t i, n_items, choice;

  val = spa_pod_get_values(&prop->value, &n_items, &choice);
  if (val->type != SPA_TYPE_Fraction || n_items == 0)
          return;

  fract = SPA_POD_BODY(val);

  switch (choice) {
    case SPA_CHOICE_None:
      gst_caps_set_simple (res, key, GST_TYPE_FRACTION, fract[0].num, fract[0].denom, NULL);
      break;
    case SPA_CHOICE_Range:
    case SPA_CHOICE_Step:
    {
      if (n_items < 3)
        return;

      if (fract[1].num == fract[2].num &&
          fract[1].denom == fract[2].denom) {
        gst_caps_set_simple (res, key, GST_TYPE_FRACTION,
            fract[1].num, fract[1].denom, NULL);
      } else {
        gst_caps_set_simple (res, key, GST_TYPE_FRACTION_RANGE,
            fract[1].num, fract[1].denom,
            fract[2].num, fract[2].denom,
            NULL);
      }
      break;
    }
    case SPA_CHOICE_Enum:
    {
      GValue l1 = { 0 }, v1 = { 0 };

      g_value_init (&l1, GST_TYPE_LIST);
      for (i = 1; i < n_items; i++) {
        g_value_init (&v1, GST_TYPE_FRACTION);
        gst_value_set_fraction (&v1, fract[i].num, fract[i].denom);
        gst_value_list_append_and_take_value (&l1, &v1);
      }
      gst_caps_set_value (res, key, &l1);
      g_value_unset (&l1);
      break;
    }
    default:
      break;
  }
}
GstCaps *
gst_caps_from_format (const struct spa_pod *format)
{
  GstCaps *res = NULL;
  uint32_t media_type, media_subtype;
  struct spa_video_colorimetry colorimetry = { 0 };
  const struct spa_pod_prop *prop = NULL;
  const struct spa_pod_object *obj = (const struct spa_pod_object *) format;

  if (spa_format_parse(format, &media_type, &media_subtype) < 0)
    return res;

  if (media_type == SPA_MEDIA_TYPE_video) {
    if (media_subtype == SPA_MEDIA_SUBTYPE_raw) {
      const struct spa_pod_prop *prop_modifier;

      res = gst_caps_new_empty_simple ("video/x-raw");

      if ((prop = spa_pod_object_find_prop (obj, prop, SPA_FORMAT_VIDEO_format)) &&
          (prop_modifier = spa_pod_object_find_prop (obj, NULL, SPA_FORMAT_VIDEO_modifier))) {
        handle_dmabuf_prop (prop, prop_modifier, res);
      } else {
        if ((prop = spa_pod_object_find_prop (obj, prop, SPA_FORMAT_VIDEO_format))) {
          handle_id_prop (prop, "format", video_id_to_string, res);
        }
      }
      if ((prop = spa_pod_object_find_prop (obj, prop, SPA_FORMAT_VIDEO_interlaceMode))) {
        handle_id_prop (prop, "interlace-mode", interlace_mode_id_to_string, res);
      } else {
        gst_caps_set_simple(res, "interlace-mode", G_TYPE_STRING, "progressive", NULL);
      }
    }
    else if (media_subtype == SPA_MEDIA_SUBTYPE_mjpg) {
      res = gst_caps_new_empty_simple ("image/jpeg");
    }
    else if (media_subtype == SPA_MEDIA_SUBTYPE_h264) {
      res = gst_caps_new_simple ("video/x-h264",
          "stream-format", G_TYPE_STRING, "byte-stream",
          "alignment", G_TYPE_STRING, "au",
          NULL);
    }
    else if (media_subtype == SPA_MEDIA_SUBTYPE_h265) {
      res = gst_caps_new_simple ("video/x-h265",
          "stream-format", G_TYPE_STRING, "byte-stream",
          "alignment", G_TYPE_STRING, "au",
          NULL);
    } else {
      return NULL;
    }
    if ((prop = spa_pod_object_find_prop (obj, prop, SPA_FORMAT_VIDEO_size))) {
      handle_rect_prop (prop, "width", "height", res);
    }
    if ((prop = spa_pod_object_find_prop (obj, prop, SPA_FORMAT_VIDEO_framerate))) {
      handle_fraction_prop (prop, "framerate", res);
    }
    if ((prop = spa_pod_object_find_prop (obj, prop, SPA_FORMAT_VIDEO_maxFramerate))) {
      handle_fraction_prop (prop, "max-framerate", res);
    }
    if (spa_pod_parse_object(format,
      SPA_TYPE_OBJECT_Format, NULL,
      SPA_FORMAT_VIDEO_colorRange, SPA_POD_OPT_Id(&colorimetry.range),
      SPA_FORMAT_VIDEO_colorMatrix, SPA_POD_OPT_Id(&colorimetry.matrix),
      SPA_FORMAT_VIDEO_transferFunction, SPA_POD_OPT_Id(&colorimetry.transfer),
      SPA_FORMAT_VIDEO_colorPrimaries, SPA_POD_OPT_Id(&colorimetry.primaries)) > 0) {
        GstVideoColorimetry gst_colorimetry;
        char *color;
        colorimetry_to_gst_colorimetry(&colorimetry, &gst_colorimetry);
        color = gst_video_colorimetry_to_string(&gst_colorimetry);
        gst_caps_set_simple(res, "colorimetry", G_TYPE_STRING, color, NULL);
        g_free(color);

    }
  } else if (media_type == SPA_MEDIA_TYPE_audio) {
    if (media_subtype == SPA_MEDIA_SUBTYPE_raw) {
      res = gst_caps_new_simple ("audio/x-raw",
          "layout", G_TYPE_STRING, "interleaved",
          NULL);
      if ((prop = spa_pod_object_find_prop (obj, prop, SPA_FORMAT_AUDIO_format))) {
        handle_id_prop (prop, "format", audio_id_to_string, res);
      }
      if ((prop = spa_pod_object_find_prop (obj, prop, SPA_FORMAT_AUDIO_rate))) {
        handle_int_prop (prop, "rate", res);
      }
      if ((prop = spa_pod_object_find_prop (obj, prop, SPA_FORMAT_AUDIO_channels))) {
        handle_int_prop (prop, "channels", res);
      }
    }
  }
  return res;
}

static gboolean
filter_dmabuf_caps (GstCapsFeatures *features,
                    GstStructure    *structure,
                    gpointer         user_data)
{
  const GValue *value;
  const char *v;

  if (!gst_caps_features_contains (features, GST_CAPS_FEATURE_MEMORY_DMABUF))
    return TRUE;

  if (!(value = gst_structure_get_value (structure, "format")) ||
      !(v = get_nth_string (value, 0)))
    return FALSE;

#ifdef HAVE_GSTREAMER_DMA_DRM
  {
    int idx;

    idx = gst_video_format_from_string (v);
    if (idx == GST_VIDEO_FORMAT_UNKNOWN)
      return FALSE;

    if (idx == GST_VIDEO_FORMAT_DMA_DRM &&
        !gst_structure_get_value (structure, "drm-format"))
      return FALSE;
  }
#endif

  return TRUE;
}

void
gst_caps_sanitize (GstCaps **caps)
{
  g_return_if_fail (GST_IS_CAPS (*caps));

  *caps = gst_caps_make_writable (*caps);
  gst_caps_filter_and_map_in_place (*caps, filter_dmabuf_caps, NULL);
}

void
gst_caps_maybe_fixate_dma_format (GstCaps *caps)
{
#ifdef HAVE_GSTREAMER_DMA_DRM
  GstCapsFeatures *features;
  GstStructure *structure;
  const GValue *format_value;
  const GValue *drm_format_value;
  const char *format_string;
  const char *drm_format_string;
  uint32_t fourcc;
  uint64_t mod;
  int drm_idx;
  int i;

  g_return_if_fail (GST_IS_CAPS (caps));

  if (gst_caps_is_fixed (caps) || gst_caps_get_size(caps) != 1)
    return;

  features = gst_caps_get_features (caps, 0);
  if (!gst_caps_features_contains (features, GST_CAPS_FEATURE_MEMORY_DMABUF))
    return;

  structure = gst_caps_get_structure (caps, 0);
  if (!gst_structure_has_field (structure, "format") ||
      !gst_structure_has_field (structure, "drm-format"))
    return;

  format_value = gst_structure_get_value (structure, "format");
  drm_format_value = gst_structure_get_value (structure, "drm-format");
  if (G_VALUE_TYPE (format_value) != GST_TYPE_LIST ||
      ((GArray *) g_value_peek_pointer (format_value))->len != 2 ||
      G_VALUE_TYPE (drm_format_value) != G_TYPE_STRING)
    return;

  drm_format_string = g_value_get_string (drm_format_value);
  fourcc = gst_video_dma_drm_fourcc_from_string (drm_format_string, &mod);
  drm_idx = gst_video_dma_drm_fourcc_to_format (fourcc);
  if (drm_idx == GST_VIDEO_FORMAT_UNKNOWN || mod != DRM_FORMAT_MOD_LINEAR)
    return;

  for (i = 0; (format_string = get_nth_string (format_value, i)); i++) {
    int idx;

    idx = gst_video_format_from_string (format_string);
    if (idx != GST_VIDEO_FORMAT_DMA_DRM && idx != drm_idx)
      return;
  }

  gst_caps_set_simple (caps, "format", G_TYPE_STRING, "DMA_DRM", NULL);
  g_warn_if_fail (gst_caps_is_fixed (caps));
#endif
}
