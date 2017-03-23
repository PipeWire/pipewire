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

typedef struct _SpaMonitor SpaMonitor;

#define SPA_MONITOR_URI             "http://spaplug.in/ns/monitor"
#define SPA_MONITOR_PREFIX          SPA_MONITOR_URI "#"

#include <spa/defs.h>
#include <spa/dict.h>
#include <spa/event.h>

typedef SpaEvent     SpaMonitorEvent;
#define SPA_MONITOR_EVENT_URI             "http://spaplug.in/ns/monitor-event"
#define SPA_MONITOR_EVENT_PREFIX          SPA_MONITOR_EVENT_URI "#"

#define SPA_MONITOR_EVENT__Added          SPA_MONITOR_EVENT_PREFIX "Added"
#define SPA_MONITOR_EVENT__Removed        SPA_MONITOR_EVENT_PREFIX "Removed"
#define SPA_MONITOR_EVENT__Changed        SPA_MONITOR_EVENT_PREFIX "Changed"

typedef SpaPODObject SpaMonitorItem;
#define SPA_MONITOR_ITEM_URI              "http://spaplug.in/ns/monitor-item"
#define SPA_MONITOR_ITEM_PREFIX           SPA_MONITOR_ITEM_URI "#"
#define SPA_MONITOR_ITEM__id              SPA_MONITOR_ITEM_PREFIX "id"
#define SPA_MONITOR_ITEM__flags           SPA_MONITOR_ITEM_PREFIX "flags"
#define SPA_MONITOR_ITEM__state           SPA_MONITOR_ITEM_PREFIX "state"
#define SPA_MONITOR_ITEM__name            SPA_MONITOR_ITEM_PREFIX "name"
#define SPA_MONITOR_ITEM__class           SPA_MONITOR_ITEM_PREFIX "class"
#define SPA_MONITOR_ITEM__info            SPA_MONITOR_ITEM_PREFIX "info"
#define SPA_MONITOR_ITEM__factory         SPA_MONITOR_ITEM_PREFIX "factory"

typedef struct {
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
} SpaMonitorTypes;

static inline void
spa_monitor_types_map (SpaIDMap *map, SpaMonitorTypes *types)
{
  if (types->Added == 0) {
    types->Monitor      = spa_id_map_get_id (map, SPA_MONITOR_URI);
    types->Added        = spa_id_map_get_id (map, SPA_MONITOR_EVENT__Added);
    types->Removed      = spa_id_map_get_id (map, SPA_MONITOR_EVENT__Removed);
    types->Changed      = spa_id_map_get_id (map, SPA_MONITOR_EVENT__Changed);
    types->MonitorItem  = spa_id_map_get_id (map, SPA_MONITOR_ITEM_URI);
    types->id           = spa_id_map_get_id (map, SPA_MONITOR_ITEM__id);
    types->flags        = spa_id_map_get_id (map, SPA_MONITOR_ITEM__flags);
    types->state        = spa_id_map_get_id (map, SPA_MONITOR_ITEM__state);
    types->name         = spa_id_map_get_id (map, SPA_MONITOR_ITEM__name);
    types->klass        = spa_id_map_get_id (map, SPA_MONITOR_ITEM__class);
    types->info         = spa_id_map_get_id (map, SPA_MONITOR_ITEM__info);
    types->factory      = spa_id_map_get_id (map, SPA_MONITOR_ITEM__factory);
  }
}

typedef enum {
  SPA_MONITOR_ITEM_FLAG_NONE    = 0,
} SpaMonitorItemFlags;

/**
 * SpaMonitorItemState:
 * @SPA_MONITOR_ITEM_STATE_AVAILABLE: The item is available
 * @SPA_MONITOR_ITEM_STATE_DISABLED: the item is disabled
 * @SPA_MONITOR_ITEM_STATE_UNAVAILABLE: the item is unavailable
 */
typedef enum {
  SPA_MONITOR_ITEM_STATE_AVAILABLE,
  SPA_MONITOR_ITEM_STATE_DISABLED,
  SPA_MONITOR_ITEM_STATE_UNAVAILABLE,
} SpaMonitorItemState;

/**
 * SpaMonitorCallback:
 * @node: a #SpaMonitor emiting the event
 * @event: the event that was emited
 * @user_data: user data provided when registering the callback
 *
 * This will be called when a monitor event is notified
 * on @monitor.
 */
typedef void   (*SpaMonitorEventCallback)  (SpaMonitor       *monitor,
                                            SpaMonitorEvent  *event,
                                            void             *user_data);

/**
 * SpaMonitor:
 *
 * The device monitor interface.
 */
struct _SpaMonitor {
  /**
   * SpaMonitor::info
   *
   * Extra information about the monitor
   */
  const SpaDict * info;

  /* the total size of this monitor. This can be used to expand this
   * structure in the future */
  size_t size;

  /**
   * SpaMonitor::set_event_callback:
   * @monitor: a #SpaMonitor
   * @callback: a #SpaMonitorEventCallback
   * @user_data: extra user data
   *
   * Set an event callback to receive asynchronous notifications from
   * the monitor.
   *
   * Returns: #SPA_RESULT_OK on success
   */
  SpaResult  (*set_event_callback)   (SpaMonitor              *monitor,
                                      SpaMonitorEventCallback  callback,
                                      void                    *user_data);

  SpaResult  (*enum_items)           (SpaMonitor              *monitor,
                                      SpaMonitorItem         **item,
                                      uint32_t                 index);

};

#define spa_monitor_set_event_callback(m,...) (m)->set_event_callback((m),__VA_ARGS__)
#define spa_monitor_enum_items(m,...)         (m)->enum_items((m),__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_MONITOR_H__ */
