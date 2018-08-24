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

#ifndef __SPA_TYPE_INFO_H__
#define __SPA_TYPE_INFO_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>


static inline bool spa_type_is_a(const char *type, const char *parent)
{
	return type != NULL && parent != NULL && strncmp(type, parent, strlen(parent)) == 0;
}

struct spa_type_info {
	uint32_t id;
	const char *name;
	uint32_t type;
	const struct spa_type_info *values;
};

#define SPA_TYPE_BASE	"Spa:"

#define SPA_TYPE__Flags				SPA_TYPE_BASE "Flags"
#define SPA_TYPE_FLAGS_BASE			SPA_TYPE__Flags ":"

#define SPA_TYPE__Enum				SPA_TYPE_BASE "Enum"
#define SPA_TYPE_ENUM_BASE			SPA_TYPE__Enum ":"

#define SPA_TYPE__Pod				SPA_TYPE_BASE "Pod"
#define SPA_TYPE_POD_BASE			SPA_TYPE__Pod ":"

#define SPA_TYPE__Struct			SPA_TYPE_POD_BASE "Struct"
#define SPA_TYPE_STRUCT_BASE			SPA_TYPE__Struct ":"

#define SPA_TYPE__Object			SPA_TYPE_POD_BASE "Object"
#define SPA_TYPE_OBJECT_BASE			SPA_TYPE__Object ":"

#define SPA_TYPE__Pointer			SPA_TYPE_BASE "Pointer"
#define SPA_TYPE_POINTER_BASE			SPA_TYPE__Pointer ":"

#define SPA_TYPE__Interface			SPA_TYPE_POINTER_BASE "Interface"
#define SPA_TYPE_INTERFACE_BASE			SPA_TYPE__Interface ":"

#define SPA_TYPE__Event				SPA_TYPE_OBJECT_BASE "Event"
#define SPA_TYPE_EVENT_BASE			SPA_TYPE__Event ":"

#include <spa/monitor/type-info.h>
#include <spa/node/type-info.h>

static const struct spa_type_info spa_types[] = {
	{ SPA_ID_INVALID, "*invalid*", SPA_ID_INVALID, },

        /* Basic types */
	{ SPA_ID_BASE, SPA_TYPE_BASE, SPA_ID_BASE,   },
	{ SPA_ID_None, SPA_TYPE_BASE "None", SPA_ID_None, },
	{ SPA_ID_Bool, SPA_TYPE_BASE "Bool", SPA_ID_Bool, },
	{ SPA_ID_Enum, SPA_TYPE__Enum, SPA_ID_Int, },
	{ SPA_ID_Int, SPA_TYPE_BASE "Int", SPA_ID_Int, },
	{ SPA_ID_Long, SPA_TYPE_BASE "Long", SPA_ID_Long, },
	{ SPA_ID_Float, SPA_TYPE_BASE "Float", SPA_ID_Float, },
	{ SPA_ID_Double, SPA_TYPE_BASE "Double", SPA_ID_Double, },
	{ SPA_ID_String, SPA_TYPE_BASE "String", SPA_ID_String, },
	{ SPA_ID_Bytes, SPA_TYPE_BASE "Bytes", SPA_ID_Bytes, },
	{ SPA_ID_Rectangle, SPA_TYPE_BASE "Rectangle", SPA_ID_Rectangle, },
	{ SPA_ID_Fraction, SPA_TYPE_BASE "Fraction", SPA_ID_Fraction, },
	{ SPA_ID_Bitmap, SPA_TYPE_BASE "Bitmap", SPA_ID_Bitmap, },
	{ SPA_ID_Array, SPA_TYPE_BASE "Array", SPA_ID_Array, },
	{ SPA_ID_Pod, SPA_TYPE__Pod, SPA_ID_Pod, },
	{ SPA_ID_Struct, SPA_TYPE__Struct, SPA_ID_Pod, },
	{ SPA_ID_Object, SPA_TYPE__Object, SPA_ID_Pod, },
	{ SPA_ID_Sequence, SPA_TYPE_POD_BASE "Sequence", SPA_ID_Pod, },
	{ SPA_ID_Pointer, SPA_TYPE__Pointer, SPA_ID_Pointer, },
	{ SPA_ID_Fd, SPA_TYPE_BASE "Fd", SPA_ID_Fd, },
	{ SPA_ID_Prop, SPA_TYPE_POD_BASE "Prop", SPA_ID_Pod, },

	{ SPA_ID_INTERFACE_BASE, SPA_TYPE__Interface, SPA_ID_Pointer, },
	{ SPA_ID_INTERFACE_Handle, SPA_TYPE_INTERFACE_BASE "Handle", SPA_ID_INTERFACE_BASE, },
	{ SPA_ID_INTERFACE_HandleFactory, SPA_TYPE_INTERFACE_BASE "HandleFactory", SPA_ID_INTERFACE_BASE, },
	{ SPA_ID_INTERFACE_Log, SPA_TYPE_INTERFACE_BASE "Log", SPA_ID_INTERFACE_BASE, },
	{ SPA_ID_INTERFACE_Loop, SPA_TYPE_INTERFACE_BASE "Loop", SPA_ID_INTERFACE_BASE, },
	{ SPA_ID_INTERFACE_LoopControl, SPA_TYPE_INTERFACE_BASE "LoopControl", SPA_ID_INTERFACE_BASE, },
	{ SPA_ID_INTERFACE_LoopUtils, SPA_TYPE_INTERFACE_BASE "LoopUtils", SPA_ID_INTERFACE_BASE, },
	{ SPA_ID_INTERFACE_DataLoop, SPA_TYPE_INTERFACE_BASE "DataLoop", SPA_ID_INTERFACE_BASE, },
	{ SPA_ID_INTERFACE_MainLoop, SPA_TYPE_INTERFACE_BASE "MainLoop", SPA_ID_INTERFACE_BASE, },
	{ SPA_ID_INTERFACE_DBus, SPA_TYPE_INTERFACE_BASE "DBus", SPA_ID_INTERFACE_BASE, },
	{ SPA_ID_INTERFACE_Monitor, SPA_TYPE_INTERFACE_BASE "Monitor", SPA_ID_INTERFACE_BASE, },
	{ SPA_ID_INTERFACE_Node, SPA_TYPE_INTERFACE_BASE "Node", SPA_ID_INTERFACE_BASE, },

	{ SPA_ID_EVENT_BASE, SPA_TYPE__Event, SPA_ID_Object, },
	{ SPA_ID_EVENT_Monitor, SPA_TYPE_EVENT_BASE "Monitor", SPA_ID_EVENT_BASE, },
	{ SPA_ID_EVENT_Node, SPA_TYPE_EVENT_BASE "Node", SPA_ID_EVENT_BASE, },


	{ 0, NULL, }
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_TYPE_INFO_H__ */
