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

struct spa_props {
	struct spa_pod_object object;
};

#define SPA_TYPE__Props		SPA_TYPE_POD_OBJECT_BASE "Props"
#define SPA_TYPE_PROPS_BASE	SPA_TYPE__Props ":"

#define SPA_TYPE_PROPS__device		SPA_TYPE_PROPS_BASE "device"
#define SPA_TYPE_PROPS__deviceName	SPA_TYPE_PROPS_BASE "deviceName"
#define SPA_TYPE_PROPS__deviceFd	SPA_TYPE_PROPS_BASE "deviceFd"
#define SPA_TYPE_PROPS__card		SPA_TYPE_PROPS_BASE "card"
#define SPA_TYPE_PROPS__cardName	SPA_TYPE_PROPS_BASE "cardName"
#define SPA_TYPE_PROPS__minLatency	SPA_TYPE_PROPS_BASE "minLatency"
#define SPA_TYPE_PROPS__periods		SPA_TYPE_PROPS_BASE "periods"
#define SPA_TYPE_PROPS__periodSize	SPA_TYPE_PROPS_BASE "periodSize"
#define SPA_TYPE_PROPS__periodEvent	SPA_TYPE_PROPS_BASE "periodEvent"
#define SPA_TYPE_PROPS__live		SPA_TYPE_PROPS_BASE "live"
#define SPA_TYPE_PROPS__waveType	SPA_TYPE_PROPS_BASE "waveType"
#define SPA_TYPE_PROPS__frequency	SPA_TYPE_PROPS_BASE "frequency"
#define SPA_TYPE_PROPS__volume		SPA_TYPE_PROPS_BASE "volume"
#define SPA_TYPE_PROPS__mute		SPA_TYPE_PROPS_BASE "mute"
#define SPA_TYPE_PROPS__patternType	SPA_TYPE_PROPS_BASE "patternType"

static inline uint32_t
spa_pod_builder_push_props(struct spa_pod_builder *builder,
			   struct spa_pod_frame *frame,
			   uint32_t props_type)
{
	return spa_pod_builder_push_object(builder, frame, 0, props_type);
}

#define spa_pod_builder_props(b,f,props_type,...)		\
	spa_pod_builder_object(b, f, 0, props_type,__VA_ARGS__)

static inline uint32_t spa_props_query(const struct spa_props *props, uint32_t key, ...)
{
	uint32_t count;
	va_list args;

	va_start(args, key);
	count = spa_pod_contents_queryv(&props->object.pod, sizeof(struct spa_props), key, args);
	va_end(args);

	return count;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PROPS_H__ */
