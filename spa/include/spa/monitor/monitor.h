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

#ifndef __SPA_MONITOR_H__
#define __SPA_MONITOR_H__

#ifdef __cplusplus
extern "C" {
#endif

struct spa_monitor;

#include <spa/utils/defs.h>
#include <spa/utils/dict.h>
#include <spa/pod/event.h>
#include <spa/pod/builder.h>

enum spa_monitor_event {
	SPA_MONITOR_EVENT_Invalid,
	SPA_MONITOR_EVENT_Added,
	SPA_MONITOR_EVENT_Removed,
	SPA_MONITOR_EVENT_Changed,
};

/** monitor event id, one of enum spa_monitor_event */
#define SPA_MONITOR_EVENT_ID(ev)	SPA_EVENT_ID(ev, SPA_TYPE_EVENT_Monitor)

enum spa_monitor_item_flags {
	SPA_MONITOR_ITEM_FLAG_NONE = 0,
};

/** The monitor item state */
enum spa_monitor_item_state {
	SPA_MONITOR_ITEM_STATE_Invalid,		/*< The item is available */
	SPA_MONITOR_ITEM_STATE_Available,	/*< The item is available */
	SPA_MONITOR_ITEM_STATE_Disabled,	/*< The item is disabled */
	SPA_MONITOR_ITEM_STATE_Unavailable,	/*< The item is unavailable */
};

/** properties for SPA_TYPE_OBJECT_MonitorItem */
enum spa_monitor_item {
	SPA_MONITOR_ITEM_START,		/**< id of object, one of enum spa_monitor_event */
	SPA_MONITOR_ITEM_id,
	SPA_MONITOR_ITEM_flags,		/**< one of enum spa_monitor_item_flags */
	SPA_MONITOR_ITEM_state,		/**< one of enum spa_monitor_item_state */
	SPA_MONITOR_ITEM_name,
	SPA_MONITOR_ITEM_class,
	SPA_MONITOR_ITEM_info,
	SPA_MONITOR_ITEM_factory,
};

/**
 * spa_monitor_callbacks:
 */
struct spa_monitor_callbacks {
	/** version of the structure */
#define SPA_VERSION_MONITOR_CALLBACKS	0
	uint32_t version;

	/** an item is added/removed/changed on the monitor */
	void (*event) (void *data, struct spa_event *event);
};

/**
 * spa_monitor:
 *
 * The device monitor interface.
 */
struct spa_monitor {
	/* the version of this monitor. This can be used to expand this
	 * structure in the future */
#define SPA_VERSION_MONITOR	0
	uint32_t version;

	/**
	 * Extra information about the monitor
	 */
	const struct spa_dict *info;

	/**
	 * Set callbacks to receive asynchronous notifications from
	 * the monitor.
	 *
	 * \param monitor: a #spa_monitor
	 * \param callback: a #callbacks
	 * \return 0 on success
	 *	   < 0 errno on error
	 */
	int (*set_callbacks) (struct spa_monitor *monitor,
			      const struct spa_monitor_callbacks *callbacks,
			      void *data);

	/**
	 * Get the next item of the monitor. \a index should contain 0 to get the
	 * first item and is updated with an opaque value that should be passed
	 * unmodified to get the next items.
	 *
	 * \param monitor a spa_monitor
	 * \param index state, use 0 for the first item
	 * \param item result item
	 * \param builder builder for \a item
	 * \return 1 when an item is available
	 *	   0 when no more items are available
	 *	   < 0 errno on error
	 */
	int (*enum_items) (struct spa_monitor *monitor,
			   uint32_t *index,
			   struct spa_pod **item,
			   struct spa_pod_builder *builder);

};

#define spa_monitor_set_callbacks(m,...)	(m)->set_callbacks((m),__VA_ARGS__)
#define spa_monitor_enum_items(m,...)		(m)->enum_items((m),__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_MONITOR_H__ */
