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
#include <lib/debug.h>
#include <lib/props.h>
#include <lib/prop-builder.h>
#include <lib/video-raw.h>
#include <lib/mapper.h>

int
main (int argc, char *argv[])
{
  SpaPropBuilder b;
  SpaPropBuilderInfo i[10];
  SpaPropBuilderRange r[10];
  SpaFormatVideo *f;

  spa_prop_builder_init (&b, sizeof (SpaFormatVideo), offsetof (SpaFormatVideo, format.props));

  spa_format_video_builder_add (&b, &i[0], SPA_PROP_ID_VIDEO_FORMAT, offsetof (SpaFormatVideo, info.raw));
  spa_format_video_builder_add_range (&b, &r[0], SPA_VIDEO_FORMAT_I420);
  spa_format_video_builder_add_range (&b, &r[1], SPA_VIDEO_FORMAT_YV12);

  b.dest = alloca (b.size);
  f = spa_prop_builder_finish (&b);
  f->format.media_type = SPA_MEDIA_TYPE_VIDEO;
  f->format.media_subtype = SPA_MEDIA_SUBTYPE_RAW;

  spa_debug_format (&f->format);

  return 0;
}
