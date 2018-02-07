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

#ifndef __SPA_PARAM_IO_H__
#define __SPA_PARAM_IO_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/param/param.h>
#include <spa/support/type-map.h>

/** type ID of property, uniquely identifies the io area for a port */
#define SPA_TYPE_PARAM_IO__id		SPA_TYPE_PARAM_IO_BASE "id"
/** size of the io area for a port */
#define SPA_TYPE_PARAM_IO__size		SPA_TYPE_PARAM_IO_BASE "size"

/** enumerate buffer io areas */
#define SPA_TYPE_PARAM_ID_IO__Buffers	SPA_TYPE_PARAM_ID_IO_BASE "Buffers"
/* an io area to exchange buffers */
#define SPA_TYPE_PARAM_IO__Buffers	SPA_TYPE_PARAM_IO_BASE "Buffers"

/** enumerate Control io areas */
#define SPA_TYPE_PARAM_ID_IO__Control	SPA_TYPE_PARAM_ID_IO_BASE "Control"
/* an io area to exchange control information */
#define SPA_TYPE_PARAM_IO__Control	SPA_TYPE_PARAM_IO_BASE "Control"

/** enumerate input or output properties */
#define SPA_TYPE_PARAM_ID_IO__Props	SPA_TYPE_PARAM_ID_IO_BASE "Props"
#define SPA_TYPE_PARAM_ID_IO_PROPS_BASE	SPA_TYPE_PARAM_ID_IO__Props ":"
/** enumerate input property io areas */
#define SPA_TYPE_PARAM_ID_IO_PROPS__In	SPA_TYPE_PARAM_ID_IO_PROPS_BASE "In"
/** enumerate output property io areas */
#define SPA_TYPE_PARAM_ID_IO_PROPS__Out	SPA_TYPE_PARAM_ID_IO_PROPS_BASE "Out"

/* an io area to exchange properties. Contents can include
 * SPA_TYPE_PARAM__PropInfo */
#define SPA_TYPE_PARAM_IO__Prop		SPA_TYPE_PARAM_IO_BASE "Prop"
#define SPA_TYPE_PARAM_IO_PROP_BASE	SPA_TYPE_PARAM_IO__Prop ":"

struct spa_type_param_io {
	uint32_t id;		/**< id to configure the io area */
	uint32_t size;		/**< size of io area */
	uint32_t idBuffers;	/**< id to enumerate buffer io */
	uint32_t Buffers;	/**< object type of buffer io area */
	uint32_t idControl;	/**< id to enumerate control io */
	uint32_t Control;	/**< object type of Control area */
	uint32_t idPropsIn;	/**< id to enumerate input properties io */
	uint32_t idPropsOut;	/**< id to enumerate output properties io */
	uint32_t Prop;		/**< object type of property area */
};

static inline void
spa_type_param_io_map(struct spa_type_map *map,
		      struct spa_type_param_io *type)
{
	if (type->id == 0) {
		type->id = spa_type_map_get_id(map, SPA_TYPE_PARAM_IO__id);
		type->size = spa_type_map_get_id(map, SPA_TYPE_PARAM_IO__size);
		type->idBuffers = spa_type_map_get_id(map, SPA_TYPE_PARAM_ID_IO__Buffers);
		type->Buffers = spa_type_map_get_id(map, SPA_TYPE_PARAM_IO__Buffers);
		type->idControl = spa_type_map_get_id(map, SPA_TYPE_PARAM_ID_IO__Control);
		type->Control = spa_type_map_get_id(map, SPA_TYPE_PARAM_IO__Control);
		type->idPropsIn = spa_type_map_get_id(map, SPA_TYPE_PARAM_ID_IO_PROPS__In);
		type->idPropsOut = spa_type_map_get_id(map, SPA_TYPE_PARAM_ID_IO_PROPS__Out);
		type->Prop = spa_type_map_get_id(map, SPA_TYPE_PARAM_IO__Prop);
	}
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_IO_H__ */
