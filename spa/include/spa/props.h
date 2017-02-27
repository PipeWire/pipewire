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

#ifndef __SPA_PROPS_H__
#define __SPA_PROPS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/pod.h>
#include <spa/pod-builder.h>

typedef SpaPODObject SpaProps;

#define SPA_PROPS_URI             "http://spaplug.in/ns/props"
#define SPA_PROPS_PREFIX          SPA_PROPS_URI "#"


static inline off_t
spa_pod_builder_push_props (SpaPODBuilder *builder,
                            SpaPODFrame   *frame)
{
  return spa_pod_builder_push_object (builder, frame, 0, 0);
}

static inline off_t
spa_pod_builder_props (SpaPODBuilder *builder,
                       uint32_t       propid, ...)
{
  SpaPODFrame f;
  va_list args;
  off_t off;

  off = spa_pod_builder_push_props (builder, &f);

  va_start (args, propid);
  spa_pod_builder_propv (builder, propid, args);
  va_end (args);

  spa_pod_builder_pop (builder, &f);

  return off;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PROPS_H__ */
