/* GStreamer
 * SPDX-FileCopyrightText: Copyright © 2026 Viorel Cernateanu
 * SPDX-License-Identifier: MIT
 */

#include <gst/gst.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <stdio.h>
#include <string.h>
extern GPtrArray *gst_caps_to_format_all (GstCaps *caps);
extern GstCaps *gst_caps_from_format (const struct spa_pod *format);
struct test
{
  const char *name, *caps;
  unsigned count;
  int w, h, fps, channels, step;
};
static const struct test cases[] = {
  { "empty", "EMPTY", 0, 0, 0, 0, 0, 0 },
  { "jpeg-preferred",
    "image/jpeg,width=(int){1296,[320,1296]},height=(int){736,[240,736]},framerate=30/1", 4, 1296,
    736, 30, 0, 0 },
  { "audio-rate-list",
    "audio/x-raw,format=S16LE,layout=interleaved,channels=2,rate=(int){48000,[8000,192000]}", 2, 0,
    0, 0, 2, 0 },
  { "dmabuf-list",
    "video/"
    "x-raw(memory:DMABuf),format=DMA_DRM,drm-format=YUYV,width=(int){640,1280},height=480,"
    "framerate=30/1",
    2, 640, 480, 30, 0, 0 },
  { "fixed", "video/x-raw,format=YUY2,width=640,height=480,framerate=30/1", 1, 640, 480, 30, 0, 0 },
  { "preferred-size",
    "video/x-raw,format=YUY2,width=(int){1296,[320,1296]},height=(int){736,[240,736]},framerate=30/"
    "1",
    4, 1296, 736, 30, 0, 0 },
  { "scalar-range", "video/x-raw,format=YUY2,width=640,height=(int)[240,736],framerate=30/1", 1,
    640, 240, 30, 0, 0 },
  { "range-scalar", "video/x-raw,format=YUY2,width=(int)[320,1296],height=480,framerate=30/1", 1,
    320, 480, 30, 0, 0 },
  { "fraction-list",
    "video/x-raw,format=YUY2,width=640,height=480,framerate=(fraction){30/1,[2/1,60/1]}", 2, 640,
    480, 30, 0, 0 },
  { "independent-lists",
    "video/x-raw,format=YUY2,width=(int){640,1280},height=(int){480,720},framerate=30/1", 4, 640,
    480, 30, 0, 0 },
  { "scalar-step", "video/x-raw,format=YUY2,width=640,height=(int)[240,736,8],framerate=30/1", 1,
    640, 240, 30, 0, 8 },
  { "step-scalar", "video/x-raw,format=YUY2,width=(int)[320,1280,8],height=480,framerate=30/1", 1,
    320, 480, 30, 0, 8 },
  { "channels-list",
    "audio/x-raw,format=S16LE,layout=interleaved,channels=(int){2,[1,8]},rate=48000", 2, 0, 0, 0, 2,
    0 },
  { "channels-step", "audio/x-raw,format=S16LE,layout=interleaved,channels=(int)[2,8,2],rate=48000",
    1, 0, 0, 0, 2, 2 },
  { "format-list", "video/x-raw,format=(string){YUY2,RGBx},width=640,height=480,framerate=30/1", 2,
    640, 480, 30, 0, 0 },
  { "all-preferences",
    "video/"
    "x-raw,format=YUY2,width=(int){1296,[320,1296]},height=(int){736,[240,736]},framerate=("
    "fraction){30/1,[2/1,60/1]}",
    8, 1296, 736, 30, 0, 0 }
};
static int
enumeration (const char *name)
{
  unsigned char data[2048];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT (data, sizeof (data));
  struct spa_rectangle a = SPA_RECTANGLE (640, 480), b = SPA_RECTANGLE (1280, 720);
  struct spa_fraction fps = SPA_FRACTION (30, 1);
  struct spa_pod *pod = spa_pod_builder_add_object (
      &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
      SPA_POD_Id (SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
      SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
      SPA_POD_Id (SPA_VIDEO_FORMAT_YUY2), SPA_FORMAT_VIDEO_size,
      SPA_POD_CHOICE_ENUM_Rectangle (3, &a, &a, &b), SPA_FORMAT_VIDEO_framerate,
      SPA_POD_Fraction (&fps));
  if (!strcmp (name, "enumeration-dmabuf"))
    pod = spa_pod_builder_add_object (
        &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
        SPA_POD_Id (SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
        SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
        SPA_POD_Id (SPA_VIDEO_FORMAT_YUY2), SPA_FORMAT_VIDEO_modifier, SPA_POD_Long ((int64_t) 0),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_ENUM_Rectangle (3, &a, &a, &b),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction (&fps));
  GstCaps *caps = gst_caps_from_format (pod);
  g_assert_nonnull (caps);
  char *original_caps = gst_caps_to_string (caps);
  unsigned count = 0;
  if (!strcmp (name, "enumeration-dmabuf"))
    for (unsigned i = 0; i < gst_caps_get_size (caps); i++)
      g_assert_true (gst_caps_features_contains (gst_caps_get_features (caps, i), "memory:DMABuf"));
  if (!strcmp (name, "enumeration-forward"))
    {
      GstCaps *bad = gst_caps_from_string ("video/x-raw,width=640,height=720");
      g_assert_false (gst_caps_can_intersect (caps, bad));
      gst_caps_unref (bad);
      count = gst_caps_get_size (caps);
    }
  else
    {
      GPtrArray *formats = gst_caps_to_format_all (caps);
      unsigned seen = 0;
      count = formats->len;
      for (unsigned i = 0; i < formats->len; i++)
        {
          const struct spa_pod_prop *p
              = spa_pod_find_prop (formats->pdata[i], NULL, SPA_FORMAT_VIDEO_size);
          g_assert_nonnull (p);
          uint32_t n, choice;
          const struct spa_pod *v = spa_pod_get_values (&p->value, &n, &choice);
          const struct spa_rectangle *r = SPA_POD_BODY (v);
          for (unsigned j = 0; j < n; j++)
            {
              if (r[j].width == 640 && r[j].height == 480)
                seen |= 1;
              else if (r[j].width == 1280 && r[j].height == 720)
                seen |= 2;
              else
                g_error ("phantom size %ux%u", r[j].width, r[j].height);
            }
        }
      g_assert_cmpuint (seen, ==, 3);
      g_ptr_array_unref (formats);
    }
  printf ("{\"case\":\"%s\",\"formats\":%u,\"caller_caps_unchanged\":true}\n", name, count);
  char *final_caps = gst_caps_to_string (caps);
  g_assert_cmpstr (original_caps, ==, final_caps);
  g_free (original_caps);
  g_free (final_caps);
  gst_caps_unref (caps);
  return 0;
}
int
main (int argc, char **argv)
{
  gst_init (NULL, NULL);
  g_assert_cmpint (argc, ==, 2);
  if (!strncmp (argv[1], "enumeration-", 12))
    return enumeration (argv[1]);
  const struct test *t = NULL;
  for (unsigned i = 0; i < G_N_ELEMENTS (cases); i++)
    if (!strcmp (cases[i].name, argv[1]))
      t = &cases[i];
  g_assert_nonnull (t);
  GstCaps *caps = gst_caps_from_string (t->caps);
  g_assert_nonnull (caps);
  char *before = gst_caps_to_string (caps);
  GPtrArray *formats = gst_caps_to_format_all (caps);
  g_assert_nonnull (formats);
  g_assert_cmpuint (formats->len, ==, t->count);
  char *after = gst_caps_to_string (caps);
  g_assert_cmpstr (before, ==, after);
  g_free (before);
  g_free (after);
  for (unsigned i = 0; i < formats->len; i++)
    {
      struct spa_pod *pod = formats->pdata[i];
      const struct spa_pod_prop *prop = spa_pod_find_prop (
          pod, NULL, t->channels ? SPA_FORMAT_AUDIO_channels : SPA_FORMAT_VIDEO_size);
      g_assert_nonnull (prop);
      uint32_t n, choice;
      const struct spa_pod *v = spa_pod_get_values (&prop->value, &n, &choice);
      g_assert_cmpuint (n, >=, 1);
      if (t->step)
        {
          g_assert_cmpuint (choice, ==, SPA_CHOICE_Step);
          g_assert_cmpuint (n, ==, 4);
          if (t->channels)
            g_assert_cmpint (((int *)SPA_POD_BODY (v))[3], ==, 2);
          else
            {
              const struct spa_rectangle *r = SPA_POD_BODY (v);
              g_assert_cmpuint (r[3].width, ==, t->w == 640 ? 1 : 8);
              g_assert_cmpuint (r[3].height, ==, t->w == 640 ? 8 : 1);
            }
        }
      if (i == 0)
        {
          if (t->channels)
            g_assert_cmpint (*(int *)SPA_POD_BODY (v), ==, t->channels);
          else
            {
              const struct spa_rectangle *r = SPA_POD_BODY (v);
              g_assert_cmpuint (r[0].width, ==, t->w);
              g_assert_cmpuint (r[0].height, ==, t->h);
              const struct spa_pod_prop *fp
                  = spa_pod_find_prop (pod, NULL, SPA_FORMAT_VIDEO_framerate);
              g_assert_nonnull (fp);
              const struct spa_pod *f = spa_pod_get_values (&fp->value, &n, &choice);
              const struct spa_fraction *ff = SPA_POD_BODY (f);
              g_assert_cmpuint (ff[0].num, ==, t->fps);
              g_assert_cmpuint (ff[0].denom, ==, 1);
            }
        }
      if (!t->step)
        {
          GstCaps *out = gst_caps_from_format (pod);
          g_assert_nonnull (out);
          g_assert_true (gst_caps_is_subset (out, caps));
          gst_caps_unref (out);
        }
    }
  printf ("{\"case\":\"%s\",\"formats\":%u,\"caller_caps_unchanged\":true}\n", t->name,
          formats->len);
  g_ptr_array_unref (formats);
  gst_caps_unref (caps);
  return 0;
}
