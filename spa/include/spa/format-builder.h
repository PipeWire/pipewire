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

static inline off_t
spa_pod_builder_push_format (SpaPODBuilder *builder,
                             SpaPODFrame   *frame,
                             uint32_t       media_type,
                             uint32_t       media_subtype)
{
  SpaFormat f = { media_type, media_subtype, { { 0, 0 }, { 0, 0 } } };
  off_t offset;

  offset = spa_pod_builder_raw (builder, &f, sizeof(f) - sizeof(SpaPODObject), false);
  if (spa_pod_builder_push_object (builder, frame, 0, 0) == -1)
    offset = -1;
  return offset;
}

static inline void
spa_pod_builder_format_propv (SpaPODBuilder *builder,
                              uint32_t       propid,
                              va_list        args)
{
  while (propid != 0) {
    int type, n_alternatives = -1;
    SpaPODProp *prop = NULL;
    SpaPODFrame f;
    off_t off;

    if ((off = spa_pod_builder_push_prop (builder, &f, propid, SPA_POD_PROP_FLAG_READWRITE)) != -1)
      prop = SPA_MEMBER (builder->data, off, SpaPODProp);

    type = va_arg (args, uint32_t);

    while (n_alternatives != 0) {
      switch (type) {
        case SPA_POD_TYPE_INVALID:
          break;
        case SPA_POD_TYPE_BOOL:
          spa_pod_builder_bool (builder, va_arg (args, int));
          break;
        case SPA_POD_TYPE_INT:
          spa_pod_builder_int (builder, va_arg (args, int32_t));
          break;
        case SPA_POD_TYPE_LONG:
          spa_pod_builder_long (builder, va_arg (args, int64_t));
          break;
        case SPA_POD_TYPE_FLOAT:
          spa_pod_builder_float (builder, va_arg (args, double));
          break;
        case SPA_POD_TYPE_DOUBLE:
          spa_pod_builder_double (builder, va_arg (args, double));
          break;
        case SPA_POD_TYPE_STRING:
        {
          const char *str = va_arg (args, char *);
          uint32_t len = va_arg (args, uint32_t);
          spa_pod_builder_string (builder, str, len);
          break;
        }
        case SPA_POD_TYPE_RECTANGLE:
        {
          uint32_t width = va_arg (args, uint32_t), height = va_arg (args, uint32_t);
          spa_pod_builder_rectangle (builder, width, height);
          break;
        }
        case SPA_POD_TYPE_FRACTION:
        {
          uint32_t num = va_arg (args, uint32_t), denom = va_arg (args, uint32_t);
          spa_pod_builder_fraction (builder, num, denom);
          break;
        }
        case SPA_POD_TYPE_BITMASK:
          break;
        case SPA_POD_TYPE_ARRAY:
        case SPA_POD_TYPE_STRUCT:
        case SPA_POD_TYPE_OBJECT:
        case SPA_POD_TYPE_PROP:
          break;
      }
      if (n_alternatives == -1) {
        uint32_t flags = va_arg (args, uint32_t);
        if (prop)
          prop->body.flags = flags;

        switch (flags & SPA_POD_PROP_RANGE_MASK) {
          case SPA_POD_PROP_RANGE_NONE:
            n_alternatives = 0;
            break;
          case SPA_POD_PROP_RANGE_MIN_MAX:
            n_alternatives = 2;
            break;
          case SPA_POD_PROP_RANGE_STEP:
            n_alternatives = 3;
            break;
          case SPA_POD_PROP_RANGE_ENUM:
          case SPA_POD_PROP_RANGE_MASK:
            n_alternatives = va_arg (args, int);
            break;
        }
      } else
        n_alternatives--;
    }
    spa_pod_builder_pop (builder, &f);

    propid = va_arg (args, uint32_t);
  }
}

static inline void
spa_pod_builder_format_prop (SpaPODBuilder *builder,
                             uint32_t       propid, ...)
{
  va_list args;

  va_start (args, propid);
  spa_pod_builder_format_propv (builder, propid, args);
  va_end (args);
}

static inline off_t
spa_pod_builder_format (SpaPODBuilder *builder,
                        uint32_t       media_type,
                        uint32_t       media_subtype,
                        uint32_t       propid, ...)
{
  SpaPODFrame f;
  va_list args;
  off_t off;

  off = spa_pod_builder_push_format (builder, &f, media_type, media_subtype);

  va_start (args, propid);
  spa_pod_builder_format_propv (builder, propid, args);
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
