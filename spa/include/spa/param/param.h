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

#include <spa/utils/defs.h>
#include <spa/pod/pod.h>
#include <spa/support/type-map.h>

/* base for parameter objects */
#define SPA_TYPE__Param			SPA_TYPE_POD_OBJECT_BASE "Param"
#define SPA_TYPE_PARAM_BASE		SPA_TYPE__Param ":"

/* base for parameter object enumerations */
#define SPA_TYPE__ParamId		SPA_TYPE_ENUM_BASE "ParamId"
#define SPA_TYPE_PARAM_ID_BASE		SPA_TYPE__ParamId ":"

/** List of supported parameters */
#define SPA_TYPE_PARAM_ID__List		SPA_TYPE_PARAM_ID_BASE "List"

/* object with supported parameter id */
#define SPA_TYPE_PARAM__List		SPA_TYPE_PARAM_BASE "List"
#define SPA_TYPE_PARAM_LIST_BASE	SPA_TYPE_PARAM__List ":"
#define SPA_TYPE_PARAM_LIST__id		SPA_TYPE_PARAM_LIST_BASE "id"

/** Enum Property info */
#define SPA_TYPE_PARAM_ID__PropInfo	SPA_TYPE_PARAM_ID_BASE "PropInfo"

#define SPA_TYPE_PARAM__PropInfo	SPA_TYPE_PARAM_BASE "PropInfo"
#define SPA_TYPE_PARAM_PROP_INFO_BASE	SPA_TYPE_PARAM__PropInfo ":"

/** associated id of the property */
#define SPA_TYPE_PARAM_PROP_INFO__id		SPA_TYPE_PARAM_PROP_INFO_BASE "id"
/** name of property */
#define SPA_TYPE_PARAM_PROP_INFO__name		SPA_TYPE_PARAM_PROP_INFO_BASE "name"
/** associated type and range/enums of property */
#define SPA_TYPE_PARAM_PROP_INFO__type		SPA_TYPE_PARAM_PROP_INFO_BASE "type"
/** associated labels of property if any, this is a struct with pairs of values,
 * the first one is of the type of the property, the second one is a string with
 * a user readable label for the value. */
#define SPA_TYPE_PARAM_PROP_INFO__labels	SPA_TYPE_PARAM_PROP_INFO_BASE "labels"

/** Property parameter id, deals with SPA_TYPE__Props */
#define SPA_TYPE_PARAM_ID__Props	SPA_TYPE_PARAM_ID_BASE "Props"

/** The available formats */
#define SPA_TYPE_PARAM_ID__EnumFormat	SPA_TYPE_PARAM_ID_BASE "EnumFormat"

/** The current format */
#define SPA_TYPE_PARAM_ID__Format	SPA_TYPE_PARAM_ID_BASE "Format"

/** The supported buffer sizes */
#define SPA_TYPE_PARAM_ID__Buffers	SPA_TYPE_PARAM_ID_BASE "Buffers"

/** The supported metadata */
#define SPA_TYPE_PARAM_ID__Meta		SPA_TYPE_PARAM_ID_BASE "Meta"

/** The supported io areas */
#define SPA_TYPE_PARAM_ID__IO		SPA_TYPE_PARAM_ID_BASE "IO"
#define SPA_TYPE_PARAM_ID_IO_BASE	SPA_TYPE_PARAM_ID__IO ":"

/** Base for parameters that describe IO areas to exchange data,
 * control and properties with a node.
 */
#define SPA_TYPE_PARAM__IO		SPA_TYPE_PARAM_BASE "IO"
#define SPA_TYPE_PARAM_IO_BASE		SPA_TYPE_PARAM__IO ":"

struct spa_type_param {
        uint32_t idList;	/**< id of the list param */
        uint32_t List;		/**< list object type */
        uint32_t listId;	/**< id in the list object */
        uint32_t idPropInfo;	/**< id to enumerate property info */
        uint32_t PropInfo;	/**< property info object */
	uint32_t propId;	/**< property id */
	uint32_t propName;	/**< property name */
	uint32_t propType;	/**< property type */
	uint32_t propLabels;	/**< property labels */
        uint32_t idProps;	/**< id to enumerate properties */
        uint32_t idEnumFormat;	/**< id to enumerate formats */
        uint32_t idFormat;	/**< id to get/set format parameter */
        uint32_t idBuffers;	/**< id to enumerate buffer requirements */
        uint32_t idMeta;	/**< id to enumerate supported metadata */
};

static inline void
spa_type_param_map(struct spa_type_map *map,
		   struct spa_type_param *type)
{
        if (type->idList == 0) {
                type->idList = spa_type_map_get_id(map, SPA_TYPE_PARAM_ID__List);
                type->List = spa_type_map_get_id(map, SPA_TYPE_PARAM__List);
                type->listId = spa_type_map_get_id(map, SPA_TYPE_PARAM_LIST__id);
                type->idPropInfo = spa_type_map_get_id(map, SPA_TYPE_PARAM_ID__PropInfo);
                type->PropInfo = spa_type_map_get_id(map, SPA_TYPE_PARAM__PropInfo);
		type->propId = spa_type_map_get_id(map, SPA_TYPE_PARAM_PROP_INFO__id);
		type->propName = spa_type_map_get_id(map, SPA_TYPE_PARAM_PROP_INFO__name);
		type->propType = spa_type_map_get_id(map, SPA_TYPE_PARAM_PROP_INFO__type);
		type->propLabels = spa_type_map_get_id(map, SPA_TYPE_PARAM_PROP_INFO__labels);
                type->idProps = spa_type_map_get_id(map, SPA_TYPE_PARAM_ID__Props);
                type->idEnumFormat = spa_type_map_get_id(map, SPA_TYPE_PARAM_ID__EnumFormat);
                type->idFormat = spa_type_map_get_id(map, SPA_TYPE_PARAM_ID__Format);
                type->idBuffers = spa_type_map_get_id(map, SPA_TYPE_PARAM_ID__Buffers);
                type->idMeta = spa_type_map_get_id(map, SPA_TYPE_PARAM_ID__Meta);
        }
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_H__ */
