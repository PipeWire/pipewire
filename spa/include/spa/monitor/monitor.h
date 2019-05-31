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

#include <spa/utils/defs.h>
#include <spa/utils/dict.h>
#include <spa/pod/event.h>
#include <spa/pod/builder.h>

#define SPA_VERSION_MONITOR	0
struct spa_monitor { struct spa_interface iface; };

struct spa_monitor_info {
#define SPA_VERSION_MONITOR_INFO 0
	uint32_t version;

#define SPA_MONITOR_CHANGE_MASK_FLAGS		(1u<<0)
#define SPA_MONITOR_CHANGE_MASK_PROPS		(1u<<1)
	uint64_t change_mask;
	uint64_t flags;
	const struct spa_dict *props;
};

#define SPA_MONITOR_INFO_INIT()	(struct spa_monitor_info){ SPA_VERSION_MONITOR_INFO, }

struct spa_monitor_object_info {
#define SPA_VERSION_MONITOR_OBJECT_INFO 0
	uint32_t version;

	uint32_t type;
	const char *factory_name;

#define SPA_MONITOR_OBJECT_CHANGE_MASK_FLAGS	(1u<<0)
#define SPA_MONITOR_OBJECT_CHANGE_MASK_PROPS	(1u<<1)
	uint64_t change_mask;
	uint64_t flags;
	const struct spa_dict *props;
};

#define SPA_MONITOR_OBJECT_INFO_INIT()	(struct spa_monitor_object_info){ SPA_VERSION_MONITOR_OBJECT_INFO, }
/**
 * spa_monitor_callbacks:
 */
struct spa_monitor_callbacks {
	/** version of the structure */
#define SPA_VERSION_MONITOR_CALLBACKS	0
	uint32_t version;

	/** receive extra information about the monitor */
	int (*info) (void *data, const struct spa_monitor_info *info);

	/** an item is added/removed/changed on the monitor */
	int (*event) (void *data, const struct spa_event *event);

	/** info changed for an object managed by the monitor, info is NULL when
	 * the object is removed */
        int (*object_info) (void *data, uint32_t id,
                const struct spa_monitor_object_info *info);
};

/**
 * spa_monitor_methods:
 *
 * The device monitor methods.
 */
struct spa_monitor_methods {
	/* the version of this monitor. This can be used to expand this
	 * structure in the future */
#define SPA_VERSION_MONITOR_METHODS	0
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
	int (*set_callbacks) (void *object,
			      const struct spa_monitor_callbacks *callbacks,
			      void *data);
};

static inline int spa_monitor_set_callbacks(struct spa_monitor *m,
		const struct spa_monitor_callbacks *callbacks, void *data)
{
	int res = -ENOTSUP;
	spa_interface_call_res(&m->iface,
			struct spa_monitor_methods, res, set_callbacks, 0,
			callbacks, data);
	return res;

}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_MONITOR_H */
