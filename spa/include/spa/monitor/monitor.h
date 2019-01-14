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

#ifndef SPA_MONITOR_H
#define SPA_MONITOR_H

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
	SPA_MONITOR_ITEM_type,
};

/**
 * spa_monitor_callbacks:
 */
struct spa_monitor_callbacks {
	/** version of the structure */
#define SPA_VERSION_MONITOR_CALLBACKS	0
	uint32_t version;

	/** receive extra information about the monitor */
	void (*info) (void *data, const struct spa_dict *info);

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
	 * Set callbacks to receive asynchronous notifications from
	 * the monitor.
	 *
	 * Setting the callbacks will emit the info
	 *
	 * \param monitor: a #spa_monitor
	 * \param callback: a #callbacks
	 * \return 0 on success
	 *	   < 0 errno on error
	 */
	int (*set_callbacks) (struct spa_monitor *monitor,
			      const struct spa_monitor_callbacks *callbacks,
			      void *data);
};

#define spa_monitor_set_callbacks(m,...)	(m)->set_callbacks((m),__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_MONITOR_H */
