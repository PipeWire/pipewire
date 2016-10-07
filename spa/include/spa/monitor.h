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
#include <spa/plugin.h>

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

typedef struct {
  const char              *id;
  SpaMonitorItemFlags      flags;
  SpaMonitorItemState      state;
  const char              *name;
  const char              *klass;
  const SpaDict           *info;
  const SpaHandleFactory  *factory;
} SpaMonitorItem;

/**
 * SpaMonitorEventType:
 * @SPA_MONITOR_EVENT_TYPE_INVALID: invalid event
 * @SPA_MONITOR_EVENT_TYPE_ADDED: an item was added, data points to #SpaMonitorItem
 * @SPA_MONITOR_EVENT_TYPE_REMOVED: an item was removed, data points to #SpaMonitorItem
 * @SPA_MONITOR_EVENT_TYPE_CHANGED: an item was changed, data points to #SpaMonitorItem
 * @SPA_MONITOR_EVENT_TYPE_ADD_POLL: add fd for polling, data points to #SpaPollItem
 * @SPA_MONITOR_EVENT_TYPE_UPDATE_POLL: update fd for polling, data points to #SpaPollItem
 * @SPA_MONITOR_EVENT_TYPE_REMOVE_POLL: remov fd for polling, data points to #SpaPollItem
 */
typedef enum {
  SPA_MONITOR_EVENT_TYPE_INVALID = 0,
  SPA_MONITOR_EVENT_TYPE_ADDED,
  SPA_MONITOR_EVENT_TYPE_REMOVED,
  SPA_MONITOR_EVENT_TYPE_CHANGED,
  SPA_MONITOR_EVENT_TYPE_ADD_POLL,
  SPA_MONITOR_EVENT_TYPE_UPDATE_POLL,
  SPA_MONITOR_EVENT_TYPE_REMOVE_POLL,
} SpaMonitorEventType;

typedef struct {
  SpaMonitorEventType  type;
  void                *data;
  size_t               size;
} SpaMonitorEvent;

/**
 * SpaMonitorCallback:
 * @node: a #SpaMonitor emiting the event
 * @event: the event that was emited
 * @user_data: user data provided when registering the callback
 *
 * This will be called when a monitor event is notified
 * on @monitor.
 */
typedef void   (*SpaMonitorEventCallback)  (SpaMonitor      *monitor,
                                            SpaMonitorEvent *event,
                                            void            *user_data);

/**
 * SpaMonitor:
 *
 * The device monitor interface.
 */
struct _SpaMonitor {
  /* pointer to the handle owning this interface */
  SpaHandle *handle;
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
                                      void                   **state);

};

#define spa_monitor_set_event_callback(m,...) (m)->set_event_callback((m),__VA_ARGS__)
#define spa_monitor_enum_items(m,...)         (m)->enum_items((m),__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_MONITOR_H__ */
