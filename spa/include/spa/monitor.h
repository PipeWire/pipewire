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
#define SPA_TYPE__Monitor            SPA_TYPE_INTERFACE_BASE "Monitor"
#define SPA_TYPE_MONITOR_BASE        SPA_TYPE__Monitor ":"

#include <spa/defs.h>
#include <spa/dict.h>
#include <spa/event.h>

#define SPA_TYPE_EVENT__Monitor           SPA_TYPE_EVENT_BASE "Monitor"
#define SPA_TYPE_EVENT_MONITOR_BASE       SPA_TYPE_EVENT__Monitor ":"

#define SPA_TYPE_EVENT_MONITOR__Added          SPA_TYPE_EVENT_MONITOR_BASE "Added"
#define SPA_TYPE_EVENT_MONITOR__Removed        SPA_TYPE_EVENT_MONITOR_BASE "Removed"
#define SPA_TYPE_EVENT_MONITOR__Changed        SPA_TYPE_EVENT_MONITOR_BASE "Changed"

struct spa_monitor_item {
  struct spa_pod_object object;
};
#define SPA_TYPE__MonitorItem                  SPA_TYPE_POD_OBJECT_BASE "MonitorItem"
#define SPA_TYPE_MONITOR_ITEM_BASE             SPA_TYPE__MonitorItem ":"

#define SPA_TYPE_MONITOR_ITEM__id              SPA_TYPE_MONITOR_ITEM_BASE "id"
#define SPA_TYPE_MONITOR_ITEM__flags           SPA_TYPE_MONITOR_ITEM_BASE "flags"
#define SPA_TYPE_MONITOR_ITEM__state           SPA_TYPE_MONITOR_ITEM_BASE "state"
#define SPA_TYPE_MONITOR_ITEM__name            SPA_TYPE_MONITOR_ITEM_BASE "name"
#define SPA_TYPE_MONITOR_ITEM__class           SPA_TYPE_MONITOR_ITEM_BASE "class"
#define SPA_TYPE_MONITOR_ITEM__info            SPA_TYPE_MONITOR_ITEM_BASE "info"
#define SPA_TYPE_MONITOR_ITEM__factory         SPA_TYPE_MONITOR_ITEM_BASE "factory"

struct spa_type_monitor {
  uint32_t Monitor;

  uint32_t Added;
  uint32_t Removed;
  uint32_t Changed;

  uint32_t MonitorItem;
  uint32_t id;
  uint32_t flags;
  uint32_t state;
  uint32_t name;
  uint32_t klass;
  uint32_t info;
  uint32_t factory;
};

static inline void
spa_type_monitor_map (struct spa_type_map *map, struct spa_type_monitor *type)
{
  if (type->Added == 0) {
    type->Monitor      = spa_type_map_get_id (map, SPA_TYPE__Monitor);
    type->Added        = spa_type_map_get_id (map, SPA_TYPE_EVENT_MONITOR__Added);
    type->Removed      = spa_type_map_get_id (map, SPA_TYPE_EVENT_MONITOR__Removed);
    type->Changed      = spa_type_map_get_id (map, SPA_TYPE_EVENT_MONITOR__Changed);
    type->MonitorItem  = spa_type_map_get_id (map, SPA_TYPE__MonitorItem);
    type->id           = spa_type_map_get_id (map, SPA_TYPE_MONITOR_ITEM__id);
    type->flags        = spa_type_map_get_id (map, SPA_TYPE_MONITOR_ITEM__flags);
    type->state        = spa_type_map_get_id (map, SPA_TYPE_MONITOR_ITEM__state);
    type->name         = spa_type_map_get_id (map, SPA_TYPE_MONITOR_ITEM__name);
    type->klass        = spa_type_map_get_id (map, SPA_TYPE_MONITOR_ITEM__class);
    type->info         = spa_type_map_get_id (map, SPA_TYPE_MONITOR_ITEM__info);
    type->factory      = spa_type_map_get_id (map, SPA_TYPE_MONITOR_ITEM__factory);
  }
}

enum spa_monitor_item_flags {
  SPA_MONITOR_ITEM_FLAG_NONE    = 0,
};

/**
 * spa_monitor_item_state:
 * @SPA_MONITOR_ITEM_STATE_AVAILABLE: The item is available
 * @SPA_MONITOR_ITEM_STATE_DISABLED: the item is disabled
 * @SPA_MONITOR_ITEM_STATE_UNAVAILABLE: the item is unavailable
 */
enum spa_monitor_item_state {
  SPA_MONITOR_ITEM_STATE_AVAILABLE,
  SPA_MONITOR_ITEM_STATE_DISABLED,
  SPA_MONITOR_ITEM_STATE_UNAVAILABLE,
};

/**
 * spa_monitor_callbacks:
 */
struct spa_monitor_callbacks {
   void  (*event)  (struct spa_monitor *monitor,
                    struct spa_event   *event,
                    void               *user_data);
};

/**
 * spa_monitor:
 *
 * The device monitor interface.
 */
struct spa_monitor {
  /**
   * spa_monitor::info
   *
   * Extra information about the monitor
   */
  const struct spa_dict* info;

  /* the total size of this monitor. This can be used to expand this
   * structure in the future */
  size_t size;

  /**
   * spa_monitor::set_callbacks:
   * @monitor: a #spa_monitor
   * @callback: a #callbacks
   * @user_data: extra user data
   *
   * Set callbacks to receive asynchronous notifications from
   * the monitor.
   *
   * Returns: #SPA_RESULT_OK on success
   */
  int   (*set_callbacks)   (struct spa_monitor                 *monitor,
                            const struct spa_monitor_callbacks *callbacks,
                            size_t                              callbacks_size,
                            void                               *user_data);

  int   (*enum_items)      (struct spa_monitor          *monitor,
                            struct spa_monitor_item    **item,
                            uint32_t                     index);

};

#define spa_monitor_set_callbacks(m,...) (m)->set_callbacks((m),__VA_ARGS__)
#define spa_monitor_enum_items(m,...)    (m)->enum_items((m),__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_MONITOR_H__ */
