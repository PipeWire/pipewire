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

#define SPA_PROPS__device               SPA_PROPS_PREFIX "device"
#define SPA_PROPS__deviceName           SPA_PROPS_PREFIX "deviceName"
#define SPA_PROPS__deviceFd             SPA_PROPS_PREFIX "deviceFd"
#define SPA_PROPS__card                 SPA_PROPS_PREFIX "card"
#define SPA_PROPS__cardName             SPA_PROPS_PREFIX "cardName"
#define SPA_PROPS__periods              SPA_PROPS_PREFIX "periods"
#define SPA_PROPS__periodSize           SPA_PROPS_PREFIX "periodSize"
#define SPA_PROPS__periodEvent          SPA_PROPS_PREFIX "periodEvent"
#define SPA_PROPS__live                 SPA_PROPS_PREFIX "live"
#define SPA_PROPS__waveType             SPA_PROPS_PREFIX "waveType"
#define SPA_PROPS__frequency            SPA_PROPS_PREFIX "frequency"
#define SPA_PROPS__volume               SPA_PROPS_PREFIX "volume"
#define SPA_PROPS__mute                 SPA_PROPS_PREFIX "mute"
#define SPA_PROPS__patternType          SPA_PROPS_PREFIX "patternType"

static inline uint32_t
spa_pod_builder_push_props (SpaPODBuilder *builder,
                            SpaPODFrame   *frame,
                            uint32_t       props_type)
{
  return spa_pod_builder_push_object (builder, frame, 0, props_type);
}

#define spa_pod_builder_props(b,f,props_type,...)               \
  spa_pod_builder_object(b, f, 0, props_type,__VA_ARGS__)

static inline uint32_t
spa_props_query (const SpaProps *props, uint32_t key, ...)
{
  uint32_t count;
  va_list args;

  va_start (args, key);
  count = spa_pod_contents_queryv (&props->pod, sizeof (SpaProps), key, args);
  va_end (args);

  return count;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PROPS_H__ */
