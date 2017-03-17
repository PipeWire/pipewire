/* Simple Plugin API
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

#ifndef __SPA_POD_ITER_H__
#define __SPA_POD_ITER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

#include <spa/defs.h>
#include <spa/pod-utils.h>

typedef struct {
  const void *data;
  uint32_t    size;
  uint32_t    offset;
} SpaPODIter;

static inline void
spa_pod_iter_contents (SpaPODIter *iter, const void *data, uint32_t size)
{
  iter->data = data;
  iter->size = size;
  iter->offset = 0;
}

static inline bool
spa_pod_iter_struct (SpaPODIter *iter, const void *data, uint32_t size)
{
  if (data == NULL || size < 8 || SPA_POD_SIZE (data) > size || SPA_POD_TYPE (data) != SPA_POD_TYPE_STRUCT)
    return false;

  spa_pod_iter_contents (iter, SPA_POD_CONTENTS (SpaPODStruct, data),
                               SPA_POD_CONTENTS_SIZE (SpaPODStruct, data));
  return true;
}

static inline bool
spa_pod_iter_object (SpaPODIter *iter, const void *data, uint32_t size)
{
  if (data == NULL || SPA_POD_SIZE (data) > size || SPA_POD_TYPE (data) != SPA_POD_TYPE_OBJECT)
    return false;

  spa_pod_iter_contents (iter, SPA_POD_CONTENTS (SpaPODObject, data),
                               SPA_POD_CONTENTS_SIZE (SpaPODObject, data));
  return true;
}
static inline bool
spa_pod_iter_pod (SpaPODIter *iter, SpaPOD *pod)
{
  void *data;
  uint32_t size;

  switch (SPA_POD_TYPE (pod)) {
    case SPA_POD_TYPE_STRUCT:
      data = SPA_POD_CONTENTS (SpaPODStruct, pod);
      size = SPA_POD_CONTENTS_SIZE (SpaPODStruct, pod);
      break;
    case SPA_POD_TYPE_OBJECT:
      data = SPA_POD_CONTENTS (SpaPODObject, pod);
      size = SPA_POD_CONTENTS_SIZE (SpaPODObject, pod);
      break;
    default:
      spa_pod_iter_contents (iter, NULL, 0);
      return false;
  }
  spa_pod_iter_contents (iter, data, size);
  return true;
}

static inline bool
spa_pod_iter_has_next (SpaPODIter *iter)
{
  return (iter->offset + 8 <= iter->size &&
      SPA_POD_SIZE (SPA_MEMBER (iter->data, iter->offset, SpaPOD)) <= iter->size);
}

static inline SpaPOD *
spa_pod_iter_next (SpaPODIter *iter)
{
  SpaPOD *res = SPA_MEMBER (iter->data, iter->offset, SpaPOD);
  iter->offset += SPA_ROUND_UP_N (SPA_POD_SIZE (res), 8);
  return res;
}

static inline SpaPOD *
spa_pod_iter_first (SpaPODIter *iter, SpaPOD *pod)
{
  if (!spa_pod_iter_pod (iter, pod) ||
      !spa_pod_iter_has_next (iter))
    return false;
  return spa_pod_iter_next (iter);
}

static inline bool
spa_pod_iter_getv (SpaPODIter *iter,
                   uint32_t    type,
                   va_list     args)
{
  while (type && spa_pod_iter_has_next (iter)) {
    SpaPOD *pod = spa_pod_iter_next (iter);

    if (type != SPA_POD_TYPE_POD && pod->type != type)
      return false;

    SPA_POD_COLLECT (pod, type, args);

    type = va_arg (args, uint32_t);
  }
  return true;
}

static inline bool
spa_pod_iter_get (SpaPODIter *iter, uint32_t type, ...)
{
  va_list args;
  bool res;

  va_start (args, type);
  res = spa_pod_iter_getv (iter, type, args);
  va_end (args);

  return res;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_POD_H__ */
