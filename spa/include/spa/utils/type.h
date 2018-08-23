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
	SPA_ID_UNASSIGNED	= 0,
	/* Interfaces */
	SPA_ID_INTERFACE_Handle,
	SPA_ID_INTERFACE_HandleFactory,
	SPA_ID_INTERFACE_Log,
	SPA_ID_INTERFACE_Loop,
	SPA_ID_INTERFACE_LoopControl,
	SPA_ID_INTERFACE_LoopUtils,
	SPA_ID_INTERFACE_DataLoop,
	SPA_ID_INTERFACE_MainLoop,
	SPA_ID_INTERFACE_DBus,
	SPA_ID_INTERFACE_Monitor,
	SPA_ID_INTERFACE_Node,

	/* Events */
	SPA_ID_EVENT_BASE = 0x10000,
	SPA_ID_EVENT_MONITOR_Added,
	SPA_ID_EVENT_MONITOR_Removed,
	SPA_ID_EVENT_MONITOR_Changed,
	SPA_ID_EVENT_NODE_Error,
	SPA_ID_EVENT_NODE_Buffering,
	SPA_ID_EVENT_NODE_RequestRefresh,

	/* Commands */
	SPA_ID_COMMAND_BASE = 0x20000,
	SPA_ID_COMMAND_NODE_Suspend,
	SPA_ID_COMMAND_NODE_Pause,
	SPA_ID_COMMAND_NODE_Start,
	SPA_ID_COMMAND_NODE_Enable,
	SPA_ID_COMMAND_NODE_Disable,
	SPA_ID_COMMAND_NODE_Flush,
	SPA_ID_COMMAND_NODE_Drain,
	SPA_ID_COMMAND_NODE_Marker,

	/* Objects */
	SPA_ID_OBJECT_BASE = 0x30000,
	SPA_ID_OBJECT_MonitorItem,

	SPA_ID_OBJECT_ParamList,
	SPA_ID_OBJECT_PropInfo,
	SPA_ID_OBJECT_Props,
	SPA_ID_OBJECT_Format,
	SPA_ID_OBJECT_ParamBuffers,
	SPA_ID_OBJECT_ParamMeta,
	SPA_ID_OBJECT_ParamIO,

	/* IO */
	SPA_ID_IO_BASE = 0x40000,
	SPA_ID_IO_Buffers,
	SPA_ID_IO_ControlRange,
	SPA_ID_IO_Clock,
	SPA_ID_IO_Prop,
	SPA_ID_IO_Latency,

	/* Params */
	SPA_ID_PARAM_BASE = 0x50000,
	SPA_ID_PARAM_List,		/**< available params */
	SPA_ID_PARAM_PropInfo,		/**< property information */
	SPA_ID_PARAM_Props,		/**< properties */
	SPA_ID_PARAM_EnumFormat,	/**< available formats */
	SPA_ID_PARAM_Format,		/**< configured format */
	SPA_ID_PARAM_Buffers,		/**< buffer configurations */
	SPA_ID_PARAM_Meta,		/**< allowed metadata for buffers */
	SPA_ID_PARAM_IO,		/**< configurable IO areas */

	/* vendor extensions */
	SPA_ID_VENDOR_PipeWire	= 0x01000000,

	SPA_ID_VENDOR_Other	= 0x7f000000,
};


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_TYPE_H__ */
