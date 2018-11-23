/* Simple Plugin API
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
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
        SPA_TYPE_Id,
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
        SPA_TYPE_Choice,
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
	SPA_TYPE_INTERFACE_Device,

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
	SPA_TYPE_OBJECT_ParamProfile,

	/* vendor extensions */
	SPA_TYPE_VENDOR_PipeWire	= 0x02000000,

	SPA_TYPE_VENDOR_Other		= 0x7f000000,
};


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_TYPE_H__ */
