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

#ifndef __SPA_NODE_TYPES_H__
#define __SPA_NODE_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/type-info.h>

#include <spa/node/command.h>
#include <spa/node/event.h>
#include <spa/node/io.h>

#define SPA_TYPE__IO			SPA_TYPE_ENUM_BASE "IO"
#define SPA_TYPE_IO_BASE		SPA_TYPE__IO ":"

static const struct spa_type_info spa_type_io[] = {
	{ SPA_IO_Invalid, SPA_TYPE_IO_BASE "Invalid", SPA_TYPE_Int, },
	{ SPA_IO_Buffers, SPA_TYPE_IO_BASE "Buffers", SPA_TYPE_Int, },
	{ SPA_IO_Range, SPA_TYPE_IO_BASE "Range", SPA_TYPE_Int, },
	{ SPA_IO_Clock, SPA_TYPE_IO_BASE "Clock", SPA_TYPE_Int, },
	{ SPA_IO_Latency, SPA_TYPE_IO_BASE "Latency", SPA_TYPE_Int, },
	{ SPA_IO_Control, SPA_TYPE_IO_BASE "Control", SPA_TYPE_Int, },
	{ SPA_IO_Notify, SPA_TYPE_IO_BASE "Notify", SPA_TYPE_Int, },
	{ SPA_IO_Position, SPA_TYPE_IO_BASE "Position", SPA_TYPE_Int, },
	{ 0, NULL, },
};

#define SPA_TYPE__NodeEvent			SPA_TYPE_EVENT_BASE "Node"
#define SPA_TYPE_NODE_EVENT_BASE		SPA_TYPE__NodeEvent ":"

static const struct spa_type_info spa_type_node_event_id[] = {
	{ SPA_NODE_EVENT_Error,		 SPA_TYPE_NODE_EVENT_BASE "Error",   SPA_TYPE_Int, },
	{ SPA_NODE_EVENT_Buffering,	 SPA_TYPE_NODE_EVENT_BASE "Buffering", SPA_TYPE_Int, },
	{ SPA_NODE_EVENT_RequestRefresh, SPA_TYPE_NODE_EVENT_BASE "RequestRefresh", SPA_TYPE_Int, },
	{ SPA_NODE_EVENT_PortsChanged,   SPA_TYPE_NODE_EVENT_BASE "PortsChanged", SPA_TYPE_Int, },
	{ 0, NULL, },
};

static const struct spa_type_info spa_type_node_event[] = {
	{ 0,		 SPA_TYPE_NODE_EVENT_BASE,   SPA_TYPE_Id, spa_type_node_event_id },
	{ 0, NULL, },
};

#define SPA_TYPE__NodeCommand			SPA_TYPE_COMMAND_BASE "Node"
#define SPA_TYPE_NODE_COMMAND_BASE		SPA_TYPE__NodeCommand ":"

static const struct spa_type_info spa_type_node_command_id[] = {
	{ SPA_NODE_COMMAND_Suspend,	SPA_TYPE_NODE_COMMAND_BASE "Suspend", SPA_TYPE_Int, },
	{ SPA_NODE_COMMAND_Pause,	SPA_TYPE_NODE_COMMAND_BASE "Pause",   SPA_TYPE_Int, },
	{ SPA_NODE_COMMAND_Start,	SPA_TYPE_NODE_COMMAND_BASE "Start",   SPA_TYPE_Int, },
	{ SPA_NODE_COMMAND_Enable,	SPA_TYPE_NODE_COMMAND_BASE "Enable",  SPA_TYPE_Int, },
	{ SPA_NODE_COMMAND_Disable,	SPA_TYPE_NODE_COMMAND_BASE "Disable", SPA_TYPE_Int, },
	{ SPA_NODE_COMMAND_Flush,	SPA_TYPE_NODE_COMMAND_BASE "Flush",   SPA_TYPE_Int, },
	{ SPA_NODE_COMMAND_Drain,	SPA_TYPE_NODE_COMMAND_BASE "Drain",   SPA_TYPE_Int, },
	{ SPA_NODE_COMMAND_Marker,	SPA_TYPE_NODE_COMMAND_BASE "Marker",  SPA_TYPE_Int, },
	{ 0, NULL, },
};

static const struct spa_type_info spa_type_node_command[] = {
	{ 0,		 SPA_TYPE_NODE_COMMAND_BASE,   SPA_TYPE_Id, spa_type_node_command_id },
	{ 0, NULL, },
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* _SPA_NODE_TYPES_H__ */
