/* Simple Plugin API
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_MONITOR_TYPES_H__
#define __SPA_MONITOR_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/type.h>
#include <spa/monitor/monitor.h>
#include <spa/pod/event-types.h>

#define SPA_TYPE_EVENT__Monitor			SPA_TYPE_EVENT_BASE "Monitor"
#define SPA_TYPE_EVENT_MONITOR_BASE		SPA_TYPE_EVENT__Monitor ":"


#define SPA_TYPE__MonitorItemFlags		SPA_TYPE_FLAGS_BASE "MonitorItemFlags"
#define SPA_TYPE_MONITOR_ITEM_FLAGS_BASE	SPA_TYPE__MonitorItemFlags ":"

static const struct spa_type_info spa_type_monitor_item_flags[] = {
	{ SPA_MONITOR_ITEM_FLAG_NONE, SPA_TYPE_MONITOR_ITEM_FLAGS_BASE "none", SPA_POD_TYPE_INT, },
	{ 0, NULL, },
};

#define SPA_TYPE__MonitorItemState		SPA_TYPE_ENUM_BASE "MonitorItemState"
#define SPA_TYPE_MONITOR_ITEM_STATE_BASE	SPA_TYPE__MonitorItemState ":"

static const struct spa_type_info spa_type_monitor_item_state[] = {
	{ SPA_MONITOR_ITEM_STATE_AVAILABLE,   SPA_TYPE_MONITOR_ITEM_STATE_BASE "available",   SPA_POD_TYPE_INT, },
	{ SPA_MONITOR_ITEM_STATE_DISABLED,    SPA_TYPE_MONITOR_ITEM_STATE_BASE "disabled",    SPA_POD_TYPE_INT, },
	{ SPA_MONITOR_ITEM_STATE_UNAVAILABLE, SPA_TYPE_MONITOR_ITEM_STATE_BASE "unavailable", SPA_POD_TYPE_INT, },
	{ 0, NULL, },
};

#define SPA_TYPE__MonitorItem			SPA_TYPE_POD_OBJECT_BASE "MonitorItem"
#define SPA_TYPE_MONITOR_ITEM_BASE		SPA_TYPE__MonitorItem ":"

static const struct spa_type_info spa_type_monitor_item_ids[] = {
	{ SPA_MONITOR_ITEM_id,      SPA_TYPE_MONITOR_ITEM_BASE "id",      SPA_POD_TYPE_STRING, },
	{ SPA_MONITOR_ITEM_flags,   SPA_TYPE_MONITOR_ITEM_BASE "flags",   SPA_POD_TYPE_INT,
		spa_type_monitor_item_flags },
	{ SPA_MONITOR_ITEM_state,   SPA_TYPE_MONITOR_ITEM_BASE "state",   SPA_POD_TYPE_INT,
		spa_type_monitor_item_state },
	{ SPA_MONITOR_ITEM_name,    SPA_TYPE_MONITOR_ITEM_BASE "name",    SPA_POD_TYPE_STRING, },
	{ SPA_MONITOR_ITEM_class,   SPA_TYPE_MONITOR_ITEM_BASE "class",   SPA_POD_TYPE_STRING, },
	{ SPA_MONITOR_ITEM_info,    SPA_TYPE_MONITOR_ITEM_BASE "info",    SPA_POD_TYPE_POD, },
	{ SPA_MONITOR_ITEM_factory, SPA_TYPE_MONITOR_ITEM_BASE "factory", SPA_POD_TYPE_POINTER, },
	{ 0, NULL, },
};

#define SPA_TYPE__Monitor		SPA_TYPE_INTERFACE_BASE "Monitor"
#define SPA_TYPE_MONITOR_BASE		SPA_TYPE__Monitor ":"

static const struct spa_type_info spa_type_monitor[] = {
	{ SPA_ID_INTERFACE_Monitor,     SPA_TYPE__Monitor,		 SPA_POD_TYPE_ID, },
	{ SPA_ID_EVENT_MONITOR_Added,   SPA_TYPE_EVENT_MONITOR_BASE "Added",   SPA_POD_TYPE_ID, },
	{ SPA_ID_EVENT_MONITOR_Removed, SPA_TYPE_EVENT_MONITOR_BASE "Removed", SPA_POD_TYPE_ID, },
	{ SPA_ID_EVENT_MONITOR_Changed, SPA_TYPE_EVENT_MONITOR_BASE "Changed", SPA_POD_TYPE_ID, },
	{ SPA_ID_OBJECT_MonitorItem,    SPA_TYPE__MonitorItem,           SPA_POD_TYPE_OBJECT,
		spa_type_monitor_item_ids},
	{ 0, NULL, },
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_MONITOR_TYPES_H__ */
