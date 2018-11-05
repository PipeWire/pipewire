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
	{ 0,   SPA_TYPE_MONITOR_EVENT_BASE,   SPA_TYPE_Id, spa_type_monitor_event_id },
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
	{ SPA_MONITOR_ITEM_STATE_Available,   SPA_TYPE_MONITOR_ITEM_STATE_BASE "Available",   SPA_TYPE_Int, },
	{ SPA_MONITOR_ITEM_STATE_Disabled,    SPA_TYPE_MONITOR_ITEM_STATE_BASE "Disabled",    SPA_TYPE_Int, },
	{ SPA_MONITOR_ITEM_STATE_Unavailable, SPA_TYPE_MONITOR_ITEM_STATE_BASE "Unavailable", SPA_TYPE_Int, },
	{ 0, NULL, },
};

#define SPA_TYPE__MonitorItem			SPA_TYPE_OBJECT_BASE "MonitorItem"
#define SPA_TYPE_MONITOR_ITEM_BASE		SPA_TYPE__MonitorItem ":"

static const struct spa_type_info spa_type_monitor_item[] = {
	{ SPA_MONITOR_ITEM_START,   SPA_TYPE_MONITOR_ITEM_BASE,           SPA_TYPE_Int, },
	{ SPA_MONITOR_ITEM_id,      SPA_TYPE_MONITOR_ITEM_BASE "id",      SPA_TYPE_String, },
	{ SPA_MONITOR_ITEM_flags,   SPA_TYPE_MONITOR_ITEM_BASE "flags",   SPA_TYPE_Id,
		spa_type_monitor_item_flags },
	{ SPA_MONITOR_ITEM_state,   SPA_TYPE_MONITOR_ITEM_BASE "state",   SPA_TYPE_Id,
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
