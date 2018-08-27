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

#include <spa/utils/type-info.h>
#include <spa/monitor/monitor.h>

#define SPA_TYPE__MonitorEvent			SPA_TYPE_EVENT_BASE "Monitor"
#define SPA_TYPE_MONITOR_EVENT_BASE		SPA_TYPE__MonitorEvent ":"

static const struct spa_type_info spa_type_monitor_event_id[] = {
	{ SPA_MONITOR_EVENT_Added,   SPA_TYPE_MONITOR_EVENT_BASE "Added",   SPA_TYPE_Int, },
	{ SPA_MONITOR_EVENT_Removed, SPA_TYPE_MONITOR_EVENT_BASE "Removed", SPA_TYPE_Int, },
	{ SPA_MONITOR_EVENT_Changed, SPA_TYPE_MONITOR_EVENT_BASE "Changed", SPA_TYPE_Int, },
	{ 0, NULL, },
};

static const struct spa_type_info spa_type_monitor_event[] = {
	{ 0,   SPA_TYPE_MONITOR_EVENT_BASE,   SPA_TYPE_Enum, spa_type_monitor_event_id },
	{ 0, NULL, },
};

#define SPA_TYPE__MonitorItemFlags		SPA_TYPE_FLAGS_BASE "MonitorItemFlags"
#define SPA_TYPE_MONITOR_ITEM_FLAGS_BASE	SPA_TYPE__MonitorItemFlags ":"

static const struct spa_type_info spa_type_monitor_item_flags[] = {
	{ SPA_MONITOR_ITEM_FLAG_NONE, SPA_TYPE_MONITOR_ITEM_FLAGS_BASE "none", SPA_TYPE_Int, },
	{ 0, NULL, },
};

#define SPA_TYPE__MonitorItemState		SPA_TYPE_ENUM_BASE "MonitorItemState"
#define SPA_TYPE_MONITOR_ITEM_STATE_BASE	SPA_TYPE__MonitorItemState ":"

static const struct spa_type_info spa_type_monitor_item_state[] = {
	{ SPA_MONITOR_ITEM_STATE_AVAILABLE,   SPA_TYPE_MONITOR_ITEM_STATE_BASE "available",   SPA_TYPE_Int, },
	{ SPA_MONITOR_ITEM_STATE_DISABLED,    SPA_TYPE_MONITOR_ITEM_STATE_BASE "disabled",    SPA_TYPE_Int, },
	{ SPA_MONITOR_ITEM_STATE_UNAVAILABLE, SPA_TYPE_MONITOR_ITEM_STATE_BASE "unavailable", SPA_TYPE_Int, },
	{ 0, NULL, },
};

#define SPA_TYPE__MonitorItem			SPA_TYPE_OBJECT_BASE "MonitorItem"
#define SPA_TYPE_MONITOR_ITEM_BASE		SPA_TYPE__MonitorItem ":"

static const struct spa_type_info spa_type_monitor_item[] = {
	{ SPA_MONITOR_ITEM_START,    SPA_TYPE_MONITOR_ITEM_BASE,           SPA_TYPE_Int, },
	{ SPA_MONITOR_ITEM_id,      SPA_TYPE_MONITOR_ITEM_BASE "id",      SPA_TYPE_String, },
	{ SPA_MONITOR_ITEM_flags,   SPA_TYPE_MONITOR_ITEM_BASE "flags",   SPA_TYPE_Enum,
		spa_type_monitor_item_flags },
	{ SPA_MONITOR_ITEM_state,   SPA_TYPE_MONITOR_ITEM_BASE "state",   SPA_TYPE_Enum,
		spa_type_monitor_item_state },
	{ SPA_MONITOR_ITEM_name,    SPA_TYPE_MONITOR_ITEM_BASE "name",    SPA_TYPE_String, },
	{ SPA_MONITOR_ITEM_class,   SPA_TYPE_MONITOR_ITEM_BASE "class",   SPA_TYPE_String, },
	{ SPA_MONITOR_ITEM_info,    SPA_TYPE_MONITOR_ITEM_BASE "info",    SPA_TYPE_Pod, },
	{ SPA_MONITOR_ITEM_factory, SPA_TYPE_MONITOR_ITEM_BASE "factory", SPA_TYPE_Pointer, },
	{ 0, NULL, },
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_MONITOR_TYPES_H__ */
