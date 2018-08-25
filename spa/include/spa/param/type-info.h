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

#ifndef __SPA_PARAM_TYPES_H__
#define __SPA_PARAM_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/utils/type-info.h>
#include <spa/param/props.h>

/* base for parameter objects */
#define SPA_TYPE__Param			SPA_TYPE_OBJECT_BASE "Param"
#define SPA_TYPE_PARAM_BASE		SPA_TYPE__Param ":"

/* object with supported parameter id */
#define SPA_TYPE_PARAM__List		SPA_TYPE_PARAM_BASE "List"
#define SPA_TYPE_PARAM_LIST_BASE	SPA_TYPE_PARAM__List ":"

static const struct spa_type_info spa_type_param_list[] = {
	{ SPA_PARAM_LIST_id, SPA_TYPE_PARAM_LIST_BASE "id",  SPA_ID_Enum, },
	{ 0, NULL, },
};

#define SPA_TYPE__Props			SPA_TYPE_PARAM_BASE "Props"
#define SPA_TYPE_PROPS_BASE		SPA_TYPE__Props ":"

static const struct spa_type_info spa_type_props[] = {
	{ SPA_PROP_unknown, SPA_TYPE_PROPS_BASE "unknown", SPA_ID_INVALID, },
	{ SPA_PROP_device, SPA_TYPE_PROPS_BASE "device", SPA_ID_String, },
	{ SPA_PROP_deviceName, SPA_TYPE_PROPS_BASE "deviceName", SPA_ID_String, },
	{ SPA_PROP_deviceFd, SPA_TYPE_PROPS_BASE "deviceFd", SPA_ID_Fd, },
	{ SPA_PROP_card, SPA_TYPE_PROPS_BASE "card", SPA_ID_String, },
	{ SPA_PROP_cardName, SPA_TYPE_PROPS_BASE "cardName", SPA_ID_String, },
	{ SPA_PROP_minLatency, SPA_TYPE_PROPS_BASE "minLatency", SPA_ID_Int, },
	{ SPA_PROP_maxLatency, SPA_TYPE_PROPS_BASE "maxLatency", SPA_ID_Int, },
	{ SPA_PROP_periods, SPA_TYPE_PROPS_BASE "periods", SPA_ID_Int, },
	{ SPA_PROP_periodSize, SPA_TYPE_PROPS_BASE "periodSize", SPA_ID_Int, },
	{ SPA_PROP_periodEvent, SPA_TYPE_PROPS_BASE "periodEvent", SPA_ID_Bool, },
	{ SPA_PROP_live, SPA_TYPE_PROPS_BASE "live", SPA_ID_Bool, },
	{ SPA_PROP_waveType, SPA_TYPE_PROPS_BASE "waveType", SPA_ID_Enum, },
	{ SPA_PROP_frequency, SPA_TYPE_PROPS_BASE "frequency", SPA_ID_Int, },
	{ SPA_PROP_volume, SPA_TYPE_PROPS_BASE "volume", SPA_ID_Float, },
	{ SPA_PROP_mute, SPA_TYPE_PROPS_BASE "mute", SPA_ID_Bool, },
	{ SPA_PROP_patternType, SPA_TYPE_PROPS_BASE "patternType", SPA_ID_Enum, },
	{ SPA_PROP_ditherType, SPA_TYPE_PROPS_BASE "ditherType", SPA_ID_Enum, },
	{ SPA_PROP_truncate, SPA_TYPE_PROPS_BASE "truncate", SPA_ID_Bool, },
	{ SPA_PROP_brightness, SPA_TYPE_PROPS_BASE "brightness", SPA_ID_Int, },
	{ SPA_PROP_contrast, SPA_TYPE_PROPS_BASE "contrast", SPA_ID_Int, },
	{ SPA_PROP_saturation, SPA_TYPE_PROPS_BASE "saturation", SPA_ID_Int, },
	{ SPA_PROP_hue, SPA_TYPE_PROPS_BASE "hue", SPA_ID_Int, },
	{ SPA_PROP_gamma, SPA_TYPE_PROPS_BASE "gamma", SPA_ID_Int, },
	{ SPA_PROP_exposure, SPA_TYPE_PROPS_BASE "exposure", SPA_ID_Int, },
	{ SPA_PROP_gain, SPA_TYPE_PROPS_BASE "gain", SPA_ID_Int, },
	{ SPA_PROP_sharpness, SPA_TYPE_PROPS_BASE "sharpness", SPA_ID_Int, },
	{ 0, NULL, },
};

/** Enum Property info */
#define SPA_TYPE__PropInfo		SPA_TYPE_PARAM_BASE "PropInfo"
#define SPA_TYPE_PROP_INFO_BASE		SPA_TYPE__PropInfo ":"

static const struct spa_type_info spa_type_prop_info[] = {
	{ SPA_PROP_INFO_id, SPA_TYPE_PROP_INFO_BASE "id",  SPA_ID_Enum, spa_type_props },
	{ SPA_PROP_INFO_name, SPA_TYPE_PROP_INFO_BASE "name",  SPA_ID_String, },
	{ SPA_PROP_INFO_type, SPA_TYPE_PROP_INFO_BASE "type",  SPA_ID_Prop, },
	{ SPA_PROP_INFO_labels, SPA_TYPE_PROP_INFO_BASE "labels",  SPA_ID_Struct, },
	{ 0, NULL, },
};

#define SPA_TYPE_PARAM__Meta			SPA_TYPE_PARAM_BASE "Meta"
#define SPA_TYPE_PARAM_META_BASE		SPA_TYPE_PARAM__Meta ":"

static const struct spa_type_info spa_type_param_meta[] = {
	{ SPA_PARAM_META_type, SPA_TYPE_PARAM_META_BASE "type",  SPA_ID_Enum, },
	{ SPA_PARAM_META_size, SPA_TYPE_PARAM_META_BASE "size",  SPA_ID_Int, },
	{ 0, NULL, },
};

/** Base for parameters that describe IO areas to exchange data,
 * control and properties with a node.
 */
#define SPA_TYPE_PARAM__IO		SPA_TYPE_PARAM_BASE "IO"
#define SPA_TYPE_PARAM_IO_BASE		SPA_TYPE_PARAM__IO ":"

static const struct spa_type_info spa_type_param_io[] = {
	{ SPA_PARAM_IO_id, SPA_TYPE_PARAM_IO_BASE "id",  SPA_ID_Enum, },
	{ SPA_PARAM_IO_size, SPA_TYPE_PARAM_IO_BASE "size",  SPA_ID_Int, },
	{ 0, NULL, },
};

#include <spa/param/format-types.h>

#define SPA_TYPE_PARAM__Buffers			SPA_TYPE_PARAM_BASE "Buffers"
#define SPA_TYPE_PARAM_BUFFERS_BASE		SPA_TYPE_PARAM__Buffers ":"

#define SPA_TYPE_PARAM__BlockInfo		SPA_TYPE_PARAM_BUFFERS_BASE "BlockInfo"
#define SPA_TYPE_PARAM_BLOCK_INFO_BASE		SPA_TYPE_PARAM__BlockInfo ":"

static const struct spa_type_info spa_type_param_buffers[] = {
	{ SPA_PARAM_BUFFERS_buffers, SPA_TYPE_PARAM_BUFFERS_BASE "buffers",  SPA_ID_Int, },
	{ SPA_PARAM_BUFFERS_blocks,  SPA_TYPE_PARAM_BUFFERS_BASE "blocks",   SPA_ID_Int, },
	{ SPA_PARAM_BUFFERS_size,    SPA_TYPE_PARAM_BLOCK_INFO_BASE "size",   SPA_ID_Int, },
	{ SPA_PARAM_BUFFERS_stride,  SPA_TYPE_PARAM_BLOCK_INFO_BASE "stride", SPA_ID_Int, },
	{ SPA_PARAM_BUFFERS_align,   SPA_TYPE_PARAM_BLOCK_INFO_BASE "align",  SPA_ID_Int, },
	{ 0, NULL, },
};

/* base for parameter object enumerations */
#define SPA_TYPE__ParamId		SPA_TYPE_ENUM_BASE "ParamId"
#define SPA_TYPE_PARAM_ID_BASE		SPA_TYPE__ParamId ":"

static const struct spa_type_info spa_type_param[] = {
	{ SPA_PARAM_List,  SPA_TYPE_PARAM_ID_BASE "List", SPA_ID_Int, },
	{ SPA_PARAM_PropInfo, SPA_TYPE_PARAM_ID_BASE "PropInfo", SPA_ID_Int, },
	{ SPA_PARAM_Props, SPA_TYPE_PARAM_ID_BASE "Props", SPA_ID_Int, },
	{ SPA_PARAM_EnumFormat, SPA_TYPE_PARAM_ID_BASE "EnumFormat", SPA_ID_Int, },
	{ SPA_PARAM_Format, SPA_TYPE_PARAM_ID_BASE "Format", SPA_ID_Int, },
	{ SPA_PARAM_Buffers, SPA_TYPE_PARAM_ID_BASE "Buffers", SPA_ID_Int, },
	{ SPA_PARAM_Meta, SPA_TYPE_PARAM_ID_BASE "Meta", SPA_ID_Int, },
	{ SPA_PARAM_IO, SPA_TYPE_PARAM_ID_BASE "IO", SPA_ID_Int, },
	{ 0, NULL, },
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_TYPES_H__ */
