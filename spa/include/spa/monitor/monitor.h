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
#define SPA_TYPE__Monitor		SPA_TYPE_INTERFACE_BASE "Monitor"
#define SPA_TYPE_MONITOR_BASE		SPA_TYPE__Monitor ":"

#include <spa/utils/defs.h>
#include <spa/utils/dict.h>
#include <spa/pod/event.h>
#include <spa/pod/builder.h>

#define SPA_TYPE_EVENT__Monitor		SPA_TYPE_EVENT_BASE "Monitor"
#define SPA_TYPE_EVENT_MONITOR_BASE	SPA_TYPE_EVENT__Monitor ":"

#define SPA_TYPE_EVENT_MONITOR__Added		SPA_TYPE_EVENT_MONITOR_BASE "Added"
#define SPA_TYPE_EVENT_MONITOR__Removed		SPA_TYPE_EVENT_MONITOR_BASE "Removed"
#define SPA_TYPE_EVENT_MONITOR__Changed		SPA_TYPE_EVENT_MONITOR_BASE "Changed"


#define SPA_TYPE__MonitorItem			SPA_TYPE_POD_OBJECT_BASE "MonitorItem"
#define SPA_TYPE_MONITOR_ITEM_BASE		SPA_TYPE__MonitorItem ":"

#define SPA_TYPE_MONITOR_ITEM__id		SPA_TYPE_MONITOR_ITEM_BASE "id"
#define SPA_TYPE_MONITOR_ITEM__flags		SPA_TYPE_MONITOR_ITEM_BASE "flags"
#define SPA_TYPE_MONITOR_ITEM__state		SPA_TYPE_MONITOR_ITEM_BASE "state"
#define SPA_TYPE_MONITOR_ITEM__name		SPA_TYPE_MONITOR_ITEM_BASE "name"
#define SPA_TYPE_MONITOR_ITEM__class		SPA_TYPE_MONITOR_ITEM_BASE "class"
#define SPA_TYPE_MONITOR_ITEM__info		SPA_TYPE_MONITOR_ITEM_BASE "info"
#define SPA_TYPE_MONITOR_ITEM__factory		SPA_TYPE_MONITOR_ITEM_BASE "factory"

struct spa_type_monitor {
	uint32_t Monitor;		/*< the monitor object */

	uint32_t Added;			/*< item added event */
	uint32_t Removed;		/*< item removed event */
	uint32_t Changed;		/*< item changed event */

	uint32_t MonitorItem;		/*< monitor item object */
	uint32_t id;			/*< item id property */
	uint32_t flags;			/*< item flags property */
	uint32_t state;			/*< item state property */
	uint32_t name;			/*< item name property */
	uint32_t klass;			/*< item klass property */
	uint32_t info;			/*< item info property */
	uint32_t factory;		/*< item factory property */
};

static inline void spa_type_monitor_map(struct spa_type_map *map, struct spa_type_monitor *type)
{
	if (type->Monitor == 0) {
		type->Monitor = spa_type_map_get_id(map, SPA_TYPE__Monitor);
		type->Added = spa_type_map_get_id(map, SPA_TYPE_EVENT_MONITOR__Added);
		type->Removed = spa_type_map_get_id(map, SPA_TYPE_EVENT_MONITOR__Removed);
		type->Changed = spa_type_map_get_id(map, SPA_TYPE_EVENT_MONITOR__Changed);
		type->MonitorItem = spa_type_map_get_id(map, SPA_TYPE__MonitorItem);
		type->id = spa_type_map_get_id(map, SPA_TYPE_MONITOR_ITEM__id);
		type->flags = spa_type_map_get_id(map, SPA_TYPE_MONITOR_ITEM__flags);
		type->state = spa_type_map_get_id(map, SPA_TYPE_MONITOR_ITEM__state);
		type->name = spa_type_map_get_id(map, SPA_TYPE_MONITOR_ITEM__name);
		type->klass = spa_type_map_get_id(map, SPA_TYPE_MONITOR_ITEM__class);
		type->info = spa_type_map_get_id(map, SPA_TYPE_MONITOR_ITEM__info);
		type->factory = spa_type_map_get_id(map, SPA_TYPE_MONITOR_ITEM__factory);
	}
}

enum spa_monitor_item_flags {
	SPA_MONITOR_ITEM_FLAG_NONE = 0,
};

/** The monitor item state */
enum spa_monitor_item_state {
	SPA_MONITOR_ITEM_STATE_AVAILABLE,	/*< The item is available */
	SPA_MONITOR_ITEM_STATE_DISABLED,	/*< The item is disabled */
	SPA_MONITOR_ITEM_STATE_UNAVAILABLE,	/*< The item is unavailable */
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
