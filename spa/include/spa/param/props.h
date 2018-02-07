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

#ifndef __SPA_PARAM_PROPS_H__
#define __SPA_PARAM_PROPS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/param/param.h>

#define SPA_TYPE__Props		SPA_TYPE_PARAM_BASE "Props"
#define SPA_TYPE_PROPS_BASE	SPA_TYPE__Props ":"

/** an unknown property */
#define SPA_TYPE_PROPS__unknown		SPA_TYPE_PROPS_BASE "unknown"

/** Common property ids */
#define SPA_TYPE_PROPS__device		SPA_TYPE_PROPS_BASE "device"
#define SPA_TYPE_PROPS__deviceName	SPA_TYPE_PROPS_BASE "deviceName"
#define SPA_TYPE_PROPS__deviceFd	SPA_TYPE_PROPS_BASE "deviceFd"
#define SPA_TYPE_PROPS__card		SPA_TYPE_PROPS_BASE "card"
#define SPA_TYPE_PROPS__cardName	SPA_TYPE_PROPS_BASE "cardName"

#define SPA_TYPE_PROPS__minLatency	SPA_TYPE_PROPS_BASE "minLatency"
#define SPA_TYPE_PROPS__maxLatency	SPA_TYPE_PROPS_BASE "maxLatency"
#define SPA_TYPE_PROPS__periods		SPA_TYPE_PROPS_BASE "periods"
#define SPA_TYPE_PROPS__periodSize	SPA_TYPE_PROPS_BASE "periodSize"
#define SPA_TYPE_PROPS__periodEvent	SPA_TYPE_PROPS_BASE "periodEvent"

#define SPA_TYPE_PROPS__live		SPA_TYPE_PROPS_BASE "live"
#define SPA_TYPE_PROPS__waveType	SPA_TYPE_PROPS_BASE "waveType"
#define SPA_TYPE_PROPS__frequency	SPA_TYPE_PROPS_BASE "frequency"
#define SPA_TYPE_PROPS__volume		SPA_TYPE_PROPS_BASE "volume"
#define SPA_TYPE_PROPS__mute		SPA_TYPE_PROPS_BASE "mute"
#define SPA_TYPE_PROPS__patternType	SPA_TYPE_PROPS_BASE "patternType"

#define SPA_TYPE_PROPS__brightness	SPA_TYPE_PROPS_BASE "brightness"
#define SPA_TYPE_PROPS__contrast	SPA_TYPE_PROPS_BASE "contrast"
#define SPA_TYPE_PROPS__saturation	SPA_TYPE_PROPS_BASE "saturation"
#define SPA_TYPE_PROPS__hue		SPA_TYPE_PROPS_BASE "hue"
#define SPA_TYPE_PROPS__gamma		SPA_TYPE_PROPS_BASE "gamma"
#define SPA_TYPE_PROPS__exposure	SPA_TYPE_PROPS_BASE "exposure"
#define SPA_TYPE_PROPS__gain		SPA_TYPE_PROPS_BASE "gain"
#define SPA_TYPE_PROPS__sharpness	SPA_TYPE_PROPS_BASE "sharpness"

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_PROPS_H__ */
