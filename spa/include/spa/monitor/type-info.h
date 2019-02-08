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

#ifndef SPA_MONITOR_TYPES_H
#define SPA_MONITOR_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/type-info.h>
#include <spa/monitor/monitor.h>

#define SPA_TYPE_INFO_MonitorEvent			SPA_TYPE_INFO_EVENT_BASE "Monitor"
#define SPA_TYPE_INFO_MONITOR_EVENT_BASE		SPA_TYPE_INFO_MonitorEvent ":"

static const struct spa_type_info spa_type_monitor_event_id[] = {
	{ SPA_MONITOR_EVENT_Added,   SPA_TYPE_Int, SPA_TYPE_INFO_MONITOR_EVENT_BASE "Added",   NULL },
	{ SPA_MONITOR_EVENT_Removed, SPA_TYPE_Int, SPA_TYPE_INFO_MONITOR_EVENT_BASE "Removed", NULL },
	{ SPA_MONITOR_EVENT_Changed, SPA_TYPE_Int, SPA_TYPE_INFO_MONITOR_EVENT_BASE "Changed", NULL },
	{ 0, 0, NULL, NULL },
};

static const struct spa_type_info spa_type_monitor_event[] = {
	{ 0, SPA_TYPE_Id, SPA_TYPE_INFO_MONITOR_EVENT_BASE, spa_type_monitor_event_id },
	{ 0, 0, NULL, NULL },
};

#define SPA_TYPE_INFO_MonitorItemFlags		SPA_TYPE_INFO_FLAGS_BASE "MonitorItemFlags"
#define SPA_TYPE_INFO_MONITOR_ITEM_FLAGS_BASE	SPA_TYPE_INFO_MonitorItemFlags ":"

static const struct spa_type_info spa_type_monitor_item_flags[] = {
	{ SPA_MONITOR_ITEM_FLAG_NONE, SPA_TYPE_Int, SPA_TYPE_INFO_MONITOR_ITEM_FLAGS_BASE "none", NULL },
	{ 0, 0, NULL, NULL },
};

#define SPA_TYPE_INFO_MonitorItemState		SPA_TYPE_INFO_ENUM_BASE "MonitorItemState"
#define SPA_TYPE_INFO_MONITOR_ITEM_STATE_BASE	SPA_TYPE_INFO_MonitorItemState ":"

static const struct spa_type_info spa_type_monitor_item_state[] = {
	{ SPA_MONITOR_ITEM_STATE_Available,   SPA_TYPE_Int, SPA_TYPE_INFO_MONITOR_ITEM_STATE_BASE "Available",   NULL },
	{ SPA_MONITOR_ITEM_STATE_Disabled,    SPA_TYPE_Int, SPA_TYPE_INFO_MONITOR_ITEM_STATE_BASE "Disabled",    NULL },
	{ SPA_MONITOR_ITEM_STATE_Unavailable, SPA_TYPE_Int, SPA_TYPE_INFO_MONITOR_ITEM_STATE_BASE "Unavailable", NULL },
	{ 0, 0, NULL, NULL },
};

#define SPA_TYPE_INFO_MonitorItem			SPA_TYPE_INFO_OBJECT_BASE "MonitorItem"
#define SPA_TYPE_INFO_MONITOR_ITEM_BASE		SPA_TYPE_INFO_MonitorItem ":"

static const struct spa_type_info spa_type_monitor_item[] = {
	{ SPA_MONITOR_ITEM_START,   SPA_TYPE_Int, SPA_TYPE_INFO_MONITOR_ITEM_BASE, NULL },
	{ SPA_MONITOR_ITEM_id,      SPA_TYPE_String, SPA_TYPE_INFO_MONITOR_ITEM_BASE "id", NULL },
	{ SPA_MONITOR_ITEM_flags,   SPA_TYPE_Id, SPA_TYPE_INFO_MONITOR_ITEM_BASE "flags",
		spa_type_monitor_item_flags },
	{ SPA_MONITOR_ITEM_state,   SPA_TYPE_Id, SPA_TYPE_INFO_MONITOR_ITEM_BASE "state",
		spa_type_monitor_item_state },
	{ SPA_MONITOR_ITEM_name,    SPA_TYPE_String, SPA_TYPE_INFO_MONITOR_ITEM_BASE "name", NULL },
	{ SPA_MONITOR_ITEM_class,   SPA_TYPE_String, SPA_TYPE_INFO_MONITOR_ITEM_BASE "class", NULL },
	{ SPA_MONITOR_ITEM_info,    SPA_TYPE_Pod, SPA_TYPE_INFO_MONITOR_ITEM_BASE "info", NULL },
	{ SPA_MONITOR_ITEM_factory, SPA_TYPE_Pointer, SPA_TYPE_INFO_MONITOR_ITEM_BASE "factory", NULL },
	{ SPA_MONITOR_ITEM_type,    SPA_TYPE_Id, SPA_TYPE_INFO_MONITOR_ITEM_BASE "type", NULL },
	{ 0, 0, NULL, NULL },
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_MONITOR_TYPES_H */
