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

#ifndef __SPA_FORMAT_BUILDER_H__
#define __SPA_FORMAT_BUILDER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

#include <spa/format.h>
#include <spa/pod-builder.h>

static inline uint32_t
spa_pod_builder_push_format (SpaPODBuilder *builder,
                             SpaPODFrame   *frame,
                             uint32_t       media_type,
                             uint32_t       media_subtype)
{
  const SpaFormat p = { { sizeof (SpaFormatBody), SPA_POD_TYPE_OBJECT },
                        { { 0, 0 },
                        { { sizeof (uint32_t), SPA_POD_TYPE_INT }, media_type },
                        { { sizeof (uint32_t), SPA_POD_TYPE_INT }, media_subtype } } };
  return spa_pod_builder_push (builder, frame, &p.pod,
                               spa_pod_builder_raw (builder, &p, sizeof(p)));
}

static inline uint32_t
spa_pod_builder_format (SpaPODBuilder *builder,
                        uint32_t       media_type,
                        uint32_t       media_subtype,
                        uint32_t       type, ...)
{
  SpaPODFrame f;
  va_list args;
  uint32_t off;

  off = spa_pod_builder_push_format (builder, &f, media_type, media_subtype);

  va_start (args, type);
  spa_pod_builder_addv (builder, type, args);
  va_end (args);

  spa_pod_builder_pop (builder, &f);

  return off;
}

SpaResult
spa_format_filter (const SpaFormat  *format,
                   const SpaFormat  *filter,
                   SpaPODBuilder    *result);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_FORMAT_BUILDER_H__ */
