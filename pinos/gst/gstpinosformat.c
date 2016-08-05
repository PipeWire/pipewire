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

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>

#include <spa/include/spa/video/format.h>
#include <spa/include/spa/audio/format.h>
#include <spa/include/spa/format.h>

#include "gstpinosformat.h"

static SpaFormat *
convert_1 (GstCapsFeatures *cf, GstStructure *cs)
{
  SpaMemory *mem;
  SpaFormat *res = NULL;
  const gchar *s;
  int i;

  if (gst_structure_has_name (cs, "video/x-raw")) {
    SpaVideoRawFormat *f;

    mem = spa_memory_alloc_size (SPA_MEMORY_POOL_LOCAL, NULL, sizeof (SpaVideoRawFormat));
    f = spa_memory_ensure_ptr (mem);
    f->format.mem.mem = mem->mem;
    f->format.mem.offset = 0;
    f->format.mem.size = mem->size;

    spa_video_raw_format_init (f);

    if (!(s = gst_structure_get_string (cs, "format")))
      goto done;
    f->info.format = gst_video_format_from_string (s);
    if (!gst_structure_get_int (cs, "width", &i))
      goto done;
    f->info.size.width = i;
    if (!gst_structure_get_int (cs, "height", &i))
      goto done;
    f->info.size.height = i;
    f->unset_mask = 0;

    res = &f->format;
  } else if (gst_structure_has_name (cs, "audio/x-raw")) {
    SpaAudioRawFormat *f;

    mem = spa_memory_alloc_size (SPA_MEMORY_POOL_LOCAL, NULL, sizeof (SpaAudioRawFormat));
    f = spa_memory_ensure_ptr (mem);
    f->format.mem.mem = mem->mem;
    f->format.mem.offset = 0;
    f->format.mem.size = mem->size;

    spa_audio_raw_format_init (f);

    if (!(s = gst_structure_get_string (cs, "format")))
      goto done;
    f->info.format = gst_audio_format_from_string (s);
    if (!(s = gst_structure_get_string (cs, "layout")))
      goto done;
    f->info.layout = 0;
    if (!gst_structure_get_int (cs, "rate", &i))
      goto done;
    f->info.rate = i;
    if (!gst_structure_get_int (cs, "channels", &i))
      goto done;
    f->info.channels = i;
    f->unset_mask = 0;

    res = &f->format;
  }

done:
  return res;
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

  res = g_ptr_array_new_full (gst_caps_get_size (caps),
                              (GDestroyNotify)spa_format_unref);

  gst_caps_foreach (caps, (GstCapsForeachFunc) foreach_func, res);

  return res;
}

GstCaps *
gst_caps_from_format (SpaFormat *format)
{
  GstCaps *res = NULL;

  if (format->media_type == SPA_MEDIA_TYPE_VIDEO) {
    if (format->media_subtype == SPA_MEDIA_SUBTYPE_RAW) {
      SpaVideoRawFormat f;

      spa_video_raw_format_parse (format, &f);

      res = gst_caps_new_simple ("video/x-raw",
          "format", G_TYPE_STRING, gst_video_format_to_string (f.info.format),
          "width", G_TYPE_INT, f.info.size.width,
          "height", G_TYPE_INT, f.info.size.height,
          NULL);
    }
  } else if (format->media_type == SPA_MEDIA_TYPE_AUDIO) {
    if (format->media_subtype == SPA_MEDIA_SUBTYPE_RAW) {
      SpaAudioRawFormat f;

      spa_audio_raw_format_parse (format, &f);

      res = gst_caps_new_simple ("audio/x-raw",
          "format", G_TYPE_STRING, gst_audio_format_to_string (f.info.format),
          "layout", G_TYPE_STRING, "interleaved",
          "rate", G_TYPE_INT, f.info.rate,
          "channels", G_TYPE_INT, f.info.channels,
          NULL);
    }
  }
  return res;
}
