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

#ifndef __SPA_DEVICE_H__
#define __SPA_DEVICE_H__

#ifdef __cplusplus
extern "C" {
#endif

struct spa_device;

#include <spa/utils/defs.h>
#include <spa/utils/dict.h>
#include <spa/support/plugin.h>

/**
 * spa_device_callbacks:
 */
struct spa_device_callbacks {
	/** version of the structure */
#define SPA_VERSION_DEVICE_CALLBACKS	0
	uint32_t version;

	/**< add a new object managed by the device */
	void (*add) (void *data, uint32_t id,
		     const struct spa_handle_factory *factory, uint32_t type,
		     const struct spa_dict *info);
	/**< remove an object */
	void (*remove) (void *data, uint32_t id);
};

/**
 * spa_device:
 *
 * The device interface.
 */
struct spa_device {
	/* the version of this device. This can be used to expand this
	 * structure in the future */
#define SPA_VERSION_DEVICE	0
	uint32_t version;

	/**
	 * Extra information about the device
	 */
	const struct spa_dict *info;

	/**
	 * Set callbacks to receive asynchronous notifications from
	 * the device.
	 *
	 * \param device: a #spa_device
	 * \param callback: a #callbacks
	 * \return 0 on success
	 *	   < 0 errno on error
	 */
	int (*set_callbacks) (struct spa_device *device,
			      const struct spa_device_callbacks *callbacks,
			      void *data);
};

#define spa_device_set_callbacks(m,...)	(m)->set_callbacks((m),__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_DEVICE_H__ */
