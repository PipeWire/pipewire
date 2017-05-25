/* Simple Plugin API
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

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <spa/format-builder.h>
#include <spa/video/format-utils.h>
#include <lib/props.h>
#include <lib/mapper.h>

int
spa_format_filter (const struct spa_format  *format,
                   const struct spa_format  *filter,
                   struct spa_pod_builder   *result)
{
  struct spa_pod_frame f;
  int res;

  if (format == NULL || result == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (filter == NULL) {
    spa_pod_builder_raw_padded (result, format, SPA_POD_SIZE (format));
    return SPA_RESULT_OK;
  }

  if (SPA_FORMAT_MEDIA_TYPE (filter) != SPA_FORMAT_MEDIA_TYPE (format) ||
      SPA_FORMAT_MEDIA_SUBTYPE (filter) != SPA_FORMAT_MEDIA_SUBTYPE (format))
    return SPA_RESULT_INVALID_MEDIA_TYPE;

  spa_pod_builder_push_format (result, &f, filter->body.obj_body.type,
                               SPA_FORMAT_MEDIA_TYPE (filter),
                               SPA_FORMAT_MEDIA_SUBTYPE (filter));
  res = spa_props_filter (result,
                          SPA_POD_CONTENTS (struct spa_format, format),
                          SPA_POD_CONTENTS_SIZE (struct spa_format, format),
                          SPA_POD_CONTENTS (struct spa_format, filter),
                          SPA_POD_CONTENTS_SIZE (struct spa_format, filter));
  spa_pod_builder_pop (result, &f);

  return res;
}
