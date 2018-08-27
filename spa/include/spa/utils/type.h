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

#ifndef __SPA_TYPE_H__
#define __SPA_TYPE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>

enum {
	/* Basic types */
	SPA_TYPE_START = 0x00000,
	SPA_TYPE_None,
        SPA_TYPE_Bool,
        SPA_TYPE_Enum,
        SPA_TYPE_Int,
        SPA_TYPE_Long,
        SPA_TYPE_Float,
        SPA_TYPE_Double,
        SPA_TYPE_String,
        SPA_TYPE_Bytes,
        SPA_TYPE_Rectangle,
        SPA_TYPE_Fraction,
        SPA_TYPE_Bitmap,
        SPA_TYPE_Array,
        SPA_TYPE_Struct,
        SPA_TYPE_Object,
        SPA_TYPE_Sequence,
        SPA_TYPE_Pointer,
        SPA_TYPE_Fd,
        SPA_TYPE_Prop,
        SPA_TYPE_Pod,

	/* Pointers */
	SPA_TYPE_POINTER_START = 0x10000,
	SPA_TYPE_POINTER_Buffer,
	SPA_TYPE_POINTER_Meta,
	SPA_TYPE_POINTER_Dict,

	/* Interfaces */
	SPA_TYPE_INTERFACE_START = 0x20000,
	SPA_TYPE_INTERFACE_Handle,
	SPA_TYPE_INTERFACE_HandleFactory,
	SPA_TYPE_INTERFACE_Log,
	SPA_TYPE_INTERFACE_Loop,
	SPA_TYPE_INTERFACE_LoopControl,
	SPA_TYPE_INTERFACE_LoopUtils,
	SPA_TYPE_INTERFACE_DataLoop,
	SPA_TYPE_INTERFACE_MainLoop,
	SPA_TYPE_INTERFACE_DBus,
	SPA_TYPE_INTERFACE_Monitor,
	SPA_TYPE_INTERFACE_Node,

	/* Events */
	SPA_TYPE_EVENT_START = 0x30000,
	SPA_TYPE_EVENT_Monitor,
	SPA_TYPE_EVENT_Node,

	/* Commands */
	SPA_TYPE_COMMAND_START = 0x40000,
	SPA_TYPE_COMMAND_Node,

	/* Objects */
	SPA_TYPE_OBJECT_START = 0x50000,
	SPA_TYPE_OBJECT_MonitorItem,
	SPA_TYPE_OBJECT_ParamList,
	SPA_TYPE_OBJECT_PropInfo,
	SPA_TYPE_OBJECT_Props,
	SPA_TYPE_OBJECT_Format,
	SPA_TYPE_OBJECT_ParamBuffers,
	SPA_TYPE_OBJECT_ParamMeta,
	SPA_TYPE_OBJECT_ParamIO,

	/* vendor extensions */
	SPA_TYPE_VENDOR_PipeWire	= 0x01000000,

	SPA_TYPE_VENDOR_Other	= 0x7f000000,
};


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_TYPE_H__ */
