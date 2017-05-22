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

#ifndef __SPA_PARAM_H__
#define __SPA_PARAM_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaParam SpaParam;

#include <spa/defs.h>
#include <spa/pod-utils.h>

#define SPA_TYPE__Param         SPA_TYPE_POD_OBJECT_BASE "Param"
#define SPA_TYPE_PARAM_BASE     SPA_TYPE__Param ":"

typedef struct {
  SpaPODObjectBody body;
  /* SpaPODProp follow */
} SpaParamBody;

struct _SpaParam {
  SpaPOD       pod;
  SpaParamBody body;
};

static inline uint32_t
spa_param_query (const SpaParam *param, uint32_t key, ...)
{
  uint32_t count;
  va_list args;

  va_start (args, key);
  count = spa_pod_contents_queryv (&param->pod, sizeof (SpaParam), key, args);
  va_end (args);

  return count;
}

#define SPA_PARAM_BODY_FOREACH(body, size, iter) \
  for ((iter) = SPA_MEMBER ((body), sizeof (SpaParamBody), SpaPODProp); \
       (iter) < SPA_MEMBER ((body), (size), SpaPODProp); \
       (iter) = SPA_MEMBER ((iter), SPA_ROUND_UP_N (SPA_POD_SIZE (iter), 8), SpaPODProp))

#define SPA_PARAM_FOREACH(param, iter) \
  SPA_PARAM_BODY_FOREACH(&param->body, SPA_POD_BODY_SIZE(param), iter)

static inline SpaResult
spa_param_fixate (SpaParam *param)
{
  SpaPODProp *prop;

  SPA_PARAM_FOREACH (param, prop)
    prop->body.flags &= ~SPA_POD_PROP_FLAG_UNSET;

  return SPA_RESULT_OK;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_H__ */
