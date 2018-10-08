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

#ifndef SPA_TYPE_ROOT
#define SPA_TYPE_ROOT	spa_types
#endif

static inline bool spa_type_is_a(const char *type, const char *parent)
{
	return type != NULL && parent != NULL && strncmp(type, parent, strlen(parent)) == 0;
}

struct spa_type_info {
	uint32_t type;
	const char *name;
	uint32_t parent;
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

#define SPA_TYPE__Command			SPA_TYPE_OBJECT_BASE "Command"
#define SPA_TYPE_COMMAND_BASE			SPA_TYPE__Command ":"

#include <spa/utils/type.h>

/* base for parameter object enumerations */
#define SPA_TYPE__Direction		SPA_TYPE_ENUM_BASE "Direction"
#define SPA_TYPE_DIRECTION_BASE		SPA_TYPE__Direction ":"

static const struct spa_type_info spa_type_direction[] = {
	{ SPA_DIRECTION_INPUT, SPA_TYPE_DIRECTION_BASE "Input", SPA_TYPE_Int,   },
	{ SPA_DIRECTION_OUTPUT, SPA_TYPE_DIRECTION_BASE "Output", SPA_TYPE_Int,   },
	{ 0, NULL, }
};

#include <spa/monitor/type-info.h>
#include <spa/node/type-info.h>
#include <spa/param/type-info.h>
#include <spa/control/type-info.h>

/* base for parameter object enumerations */
#define SPA_TYPE__Choice		SPA_TYPE_ENUM_BASE "Choice"
#define SPA_TYPE_CHOICE_BASE		SPA_TYPE__Choice ":"

static const struct spa_type_info spa_type_choice[] = {
	{ SPA_CHOICE_None, SPA_TYPE_CHOICE_BASE "None", SPA_TYPE_Int,   },
	{ SPA_CHOICE_Range, SPA_TYPE_CHOICE_BASE "Range", SPA_TYPE_Int,   },
	{ SPA_CHOICE_Step, SPA_TYPE_CHOICE_BASE "Step", SPA_TYPE_Int,   },
	{ SPA_CHOICE_Enum, SPA_TYPE_CHOICE_BASE "Enum", SPA_TYPE_Int,   },
	{ SPA_CHOICE_Flags, SPA_TYPE_CHOICE_BASE "Flags", SPA_TYPE_Int,   },
	{ 0, NULL, }
};

static const struct spa_type_info spa_types[] = {
        /* Basic types */
	{ SPA_TYPE_START, SPA_TYPE_BASE, SPA_TYPE_START,   },
	{ SPA_TYPE_None, SPA_TYPE_BASE "None", SPA_TYPE_None, },
	{ SPA_TYPE_Bool, SPA_TYPE_BASE "Bool", SPA_TYPE_Bool, },
	{ SPA_TYPE_Id, SPA_TYPE_BASE "Id", SPA_TYPE_Int, },
	{ SPA_TYPE_Int, SPA_TYPE_BASE "Int", SPA_TYPE_Int, },
	{ SPA_TYPE_Long, SPA_TYPE_BASE "Long", SPA_TYPE_Long, },
	{ SPA_TYPE_Float, SPA_TYPE_BASE "Float", SPA_TYPE_Float, },
	{ SPA_TYPE_Double, SPA_TYPE_BASE "Double", SPA_TYPE_Double, },
	{ SPA_TYPE_String, SPA_TYPE_BASE "String", SPA_TYPE_String, },
	{ SPA_TYPE_Bytes, SPA_TYPE_BASE "Bytes", SPA_TYPE_Bytes, },
	{ SPA_TYPE_Rectangle, SPA_TYPE_BASE "Rectangle", SPA_TYPE_Rectangle, },
	{ SPA_TYPE_Fraction, SPA_TYPE_BASE "Fraction", SPA_TYPE_Fraction, },
	{ SPA_TYPE_Bitmap, SPA_TYPE_BASE "Bitmap", SPA_TYPE_Bitmap, },
	{ SPA_TYPE_Array, SPA_TYPE_BASE "Array", SPA_TYPE_Array, },
	{ SPA_TYPE_Pod, SPA_TYPE__Pod, SPA_TYPE_Pod, },
	{ SPA_TYPE_Struct, SPA_TYPE__Struct, SPA_TYPE_Pod, },
	{ SPA_TYPE_Object, SPA_TYPE__Object, SPA_TYPE_Pod, },
	{ SPA_TYPE_Sequence, SPA_TYPE_POD_BASE "Sequence", SPA_TYPE_Pod, },
	{ SPA_TYPE_Pointer, SPA_TYPE__Pointer, SPA_TYPE_Pointer, },
	{ SPA_TYPE_Fd, SPA_TYPE_BASE "Fd", SPA_TYPE_Fd, },
	{ SPA_TYPE_Choice, SPA_TYPE_POD_BASE "Choice", SPA_TYPE_Pod, },

	{ SPA_TYPE_POINTER_START, SPA_TYPE__Pointer, SPA_TYPE_Pointer, },
	{ SPA_TYPE_POINTER_Buffer, SPA_TYPE_POINTER_BASE "Buffer", SPA_TYPE_Pointer, },
	{ SPA_TYPE_POINTER_Meta, SPA_TYPE_POINTER_BASE "Meta", SPA_TYPE_Pointer, },
	{ SPA_TYPE_POINTER_Dict, SPA_TYPE_POINTER_BASE "Dict", SPA_TYPE_Pointer, },

	{ SPA_TYPE_INTERFACE_START, SPA_TYPE__Interface, SPA_TYPE_Pointer, },
	{ SPA_TYPE_INTERFACE_Handle, SPA_TYPE_INTERFACE_BASE "Handle", SPA_TYPE_Pointer, },
	{ SPA_TYPE_INTERFACE_HandleFactory, SPA_TYPE_INTERFACE_BASE "HandleFactory", SPA_TYPE_Pointer, },
	{ SPA_TYPE_INTERFACE_Log, SPA_TYPE_INTERFACE_BASE "Log", SPA_TYPE_Pointer, },
	{ SPA_TYPE_INTERFACE_Loop, SPA_TYPE_INTERFACE_BASE "Loop", SPA_TYPE_Pointer, },
	{ SPA_TYPE_INTERFACE_LoopControl, SPA_TYPE_INTERFACE_BASE "LoopControl", SPA_TYPE_Pointer, },
	{ SPA_TYPE_INTERFACE_LoopUtils, SPA_TYPE_INTERFACE_BASE "LoopUtils", SPA_TYPE_Pointer, },
	{ SPA_TYPE_INTERFACE_DataLoop, SPA_TYPE_INTERFACE_BASE "DataLoop", SPA_TYPE_Pointer, },
	{ SPA_TYPE_INTERFACE_MainLoop, SPA_TYPE_INTERFACE_BASE "MainLoop", SPA_TYPE_Pointer, },
	{ SPA_TYPE_INTERFACE_DBus, SPA_TYPE_INTERFACE_BASE "DBus", SPA_TYPE_Pointer, },
	{ SPA_TYPE_INTERFACE_Monitor, SPA_TYPE_INTERFACE_BASE "Monitor", SPA_TYPE_Pointer, },
	{ SPA_TYPE_INTERFACE_Node, SPA_TYPE_INTERFACE_BASE "Node", SPA_TYPE_Pointer, },

	{ SPA_TYPE_EVENT_START, SPA_TYPE__Event, SPA_TYPE_Object, },
	{ SPA_TYPE_EVENT_Monitor, SPA_TYPE_EVENT_BASE "Monitor", SPA_TYPE_Object, spa_type_monitor_event },
	{ SPA_TYPE_EVENT_Node, SPA_TYPE_EVENT_BASE "Node", SPA_TYPE_Object, spa_type_node_event },

	{ SPA_TYPE_COMMAND_START, SPA_TYPE__Command, SPA_TYPE_Object, },
	{ SPA_TYPE_COMMAND_Node, SPA_TYPE_COMMAND_BASE "Node", SPA_TYPE_Object, spa_type_node_command },

	{ SPA_TYPE_OBJECT_START, SPA_TYPE__Object, SPA_TYPE_Object, },
	{ SPA_TYPE_OBJECT_MonitorItem, SPA_TYPE__MonitorItem, SPA_TYPE_Object, spa_type_monitor_item },
	{ SPA_TYPE_OBJECT_ParamList, SPA_TYPE_PARAM__List, SPA_TYPE_Object, spa_type_param_list, },
	{ SPA_TYPE_OBJECT_PropInfo, SPA_TYPE__PropInfo, SPA_TYPE_Object, spa_type_prop_info, },
	{ SPA_TYPE_OBJECT_Props, SPA_TYPE__Props, SPA_TYPE_Object, spa_type_props },
	{ SPA_TYPE_OBJECT_Format, SPA_TYPE__Format, SPA_TYPE_Object, spa_type_format },
	{ SPA_TYPE_OBJECT_ParamBuffers, SPA_TYPE_PARAM__Buffers, SPA_TYPE_Object, spa_type_param_buffers, },
	{ SPA_TYPE_OBJECT_ParamMeta, SPA_TYPE_PARAM__Meta, SPA_TYPE_Object, spa_type_param_meta },
	{ SPA_TYPE_OBJECT_ParamIO, SPA_TYPE_PARAM__IO, SPA_TYPE_Object, spa_type_param_io },
	{ SPA_TYPE_OBJECT_ParamProfile, SPA_TYPE_PARAM__Profile, SPA_TYPE_Object, spa_type_param_profile },

	{ 0, NULL, }
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_TYPE_INFO_H__ */
