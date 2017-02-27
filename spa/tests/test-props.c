/* Spa
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <spa/id-map.h>
#include <spa/log.h>
#include <spa/node.h>
#include <spa/loop.h>
#include <spa/video/format.h>
#include <spa/format-builder.h>
#include <lib/debug.h>
#include <lib/mapper.h>

#if 0
/* { video/raw,
 *   format: (int 1) { 1  2 },
 *   size:   (rect (320 240)) { (1 1) (MAX MAX) },
 *   framerate: (frac (25 1)) { (0 1) (MAX 1) },
 */

spa_build (SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW,
           SPA_PROP_ID_VIDEO_FORMAT,    SPA_PROP_TYPE_INT,
                                                SPA_VIDEO_FORMAT_I420
                                        SPA_POD_PROP_FLAG_UNSET |
                                        SPA_PROP_RANGE_ENUM, 2,
                                                SPA_VIDEO_FORMAT_I420,
                                                SPA_VIDEO_FORMAT_YUY2,
           SPA_PROP_ID_VIDEO_SIZE  ,    SPA_PROP_TYPE_RECTANGLE,
                                                320, 240,
                                        SPA_POD_PROP_FLAG_UNSET |
                                        SPA_PROP_RANGE_MIN_MAX,
                                                1, 1,
                                                INT32_MAX, INT32_MAX,
           SPA_PROP_ID_VIDEO_FRAMERATE, SPA_PROP_TYPE_FRACTION, 25, 1,
                                        SPA_POD_PROP_FLAG_UNSET |
                                        SPA_PROP_RANGE_MIN_MAX,
                                                0, 1,
                                                INT32_MAX, 1,
           0);
#endif

static const struct _test_format {
  SpaFormat fmt;

  struct {
    SpaPODProp prop_format;
    struct {
      uint32_t def_format;
      uint32_t enum_format[2];
    } format_vals;
    uint32_t pad;

    SpaPODProp prop_size;
    struct {
      SpaRectangle def_size;
      SpaRectangle min_size;
      SpaRectangle max_size;
    } size_vals;

    SpaPODProp prop_framerate;
    struct {
      SpaFraction def_framerate;
      SpaFraction min_framerate;
      SpaFraction max_framerate;
    } framerate_vals;
  } props;
} test_format = {
  { { { sizeof (test_format.props) + sizeof (SpaFormatBody) + sizeof (SpaPODObjectBody), SPA_POD_TYPE_OBJECT },
      { 0, 0 } },
    { { { sizeof (uint32_t), SPA_POD_TYPE_INT }, SPA_MEDIA_TYPE_VIDEO },
      { { sizeof (uint32_t), SPA_POD_TYPE_INT }, SPA_MEDIA_SUBTYPE_RAW } },
  }, {
  { { sizeof (test_format.props.format_vals) + sizeof (SpaPODPropBody),
      SPA_POD_TYPE_PROP } ,
    { SPA_PROP_ID_VIDEO_FORMAT, SPA_POD_PROP_RANGE_ENUM | SPA_POD_PROP_FLAG_UNSET,
      { sizeof (uint32_t), SPA_POD_TYPE_INT } }, },
        { SPA_VIDEO_FORMAT_I420,
         { SPA_VIDEO_FORMAT_I420, SPA_VIDEO_FORMAT_YUY2 } }, 0,

  { { sizeof (test_format.props.size_vals) + sizeof (SpaPODPropBody),
      SPA_POD_TYPE_PROP } ,
    { SPA_PROP_ID_VIDEO_SIZE, SPA_POD_PROP_RANGE_MIN_MAX | SPA_POD_PROP_FLAG_UNSET,
      { sizeof (SpaRectangle), SPA_POD_TYPE_RECTANGLE } }, },
        { { 320, 240 },
          { 1, 1 },
          { INT32_MAX, INT32_MAX } },

  { { sizeof (test_format.props.framerate_vals) + sizeof (SpaPODPropBody),
      SPA_POD_TYPE_PROP } ,
    { SPA_PROP_ID_VIDEO_FRAMERATE, SPA_POD_PROP_RANGE_MIN_MAX | SPA_POD_PROP_FLAG_UNSET,
      { sizeof (SpaFraction), SPA_POD_TYPE_FRACTION } }, },
        { { 25, 1 },
          { 0, 1 },
          { INT32_MAX, 1 } },
  }
};

int
main (int argc, char *argv[])
{
  SpaPODBuilder b = { NULL, };
  SpaPODFrame frame[4];
  uint8_t buffer[1024];
  SpaFormat *fmt;
  off_t o;

  b.data = buffer;
  b.size = 1024;

  fmt = SPA_MEMBER (buffer, spa_pod_builder_push_format (&b, &frame[0],
                                                         SPA_MEDIA_TYPE_VIDEO,
                                                         SPA_MEDIA_SUBTYPE_RAW), SpaFormat);
  spa_pod_builder_push_prop (&b, &frame[1],
                             SPA_PROP_ID_VIDEO_FORMAT,
                             SPA_POD_PROP_RANGE_ENUM | SPA_POD_PROP_FLAG_UNSET | SPA_POD_PROP_FLAG_READWRITE);
  spa_pod_builder_int (&b, SPA_VIDEO_FORMAT_I420);
  spa_pod_builder_int (&b, SPA_VIDEO_FORMAT_I420);
  spa_pod_builder_int (&b, SPA_VIDEO_FORMAT_YUY2);
  spa_pod_builder_pop (&b, &frame[1]);

  SpaRectangle size_min_max[] = { { 1, 1 }, { INT32_MAX, INT32_MAX } };
  spa_pod_builder_push_prop (&b, &frame[1],
                             SPA_PROP_ID_VIDEO_SIZE,
                             SPA_POD_PROP_RANGE_MIN_MAX | SPA_POD_PROP_FLAG_UNSET | SPA_POD_PROP_FLAG_READWRITE);
  spa_pod_builder_rectangle (&b, 320, 240);
  spa_pod_builder_raw (&b, size_min_max, sizeof(size_min_max), false);
  spa_pod_builder_pop (&b, &frame[1]);

  SpaFraction rate_min_max[] = { { 0, 1 }, { INT32_MAX, 1 } };
  spa_pod_builder_push_prop (&b, &frame[1],
                             SPA_PROP_ID_VIDEO_FRAMERATE,
                             SPA_POD_PROP_RANGE_MIN_MAX | SPA_POD_PROP_FLAG_UNSET | SPA_POD_PROP_FLAG_READWRITE);
  spa_pod_builder_fraction (&b, 25, 1);
  spa_pod_builder_raw (&b, rate_min_max, sizeof(rate_min_max), false);
  spa_pod_builder_pop (&b, &frame[1]);

  spa_pod_builder_pop (&b, &frame[0]);

  spa_debug_pod (&fmt->obj.pod);

  memset (&b, 0, sizeof(b));
  b.data = buffer;
  b.size = 1024;

  o = spa_pod_builder_format (&b, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW,
           SPA_PROP_ID_VIDEO_FORMAT,    SPA_POD_TYPE_INT,
                                                SPA_VIDEO_FORMAT_I420,
                                        SPA_POD_PROP_FLAG_UNSET |
                                        SPA_POD_PROP_RANGE_ENUM, 2,
                                                SPA_VIDEO_FORMAT_I420,
                                                SPA_VIDEO_FORMAT_YUY2,
           SPA_PROP_ID_VIDEO_SIZE  ,    SPA_POD_TYPE_RECTANGLE,
                                                320, 240,
                                        SPA_POD_PROP_FLAG_UNSET |
                                        SPA_POD_PROP_RANGE_MIN_MAX,
                                                1, 1,
                                                INT32_MAX, INT32_MAX,
           SPA_PROP_ID_VIDEO_FRAMERATE, SPA_POD_TYPE_FRACTION, 25, 1,
                                        SPA_POD_PROP_FLAG_UNSET |
                                        SPA_POD_PROP_RANGE_MIN_MAX,
                                                0, 1,
                                                INT32_MAX, 1,
           0);

  fmt = SPA_MEMBER (buffer, o, SpaFormat);
  spa_debug_pod (&fmt->obj.pod);
  spa_debug_format (fmt);

  spa_debug_pod (&test_format.fmt.obj.pod);
  spa_debug_format (&test_format.fmt);

  return 0;
}
