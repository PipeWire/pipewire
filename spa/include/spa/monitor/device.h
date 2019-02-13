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

#ifndef SPA_DEVICE_H
#define SPA_DEVICE_H

#ifdef __cplusplus
extern "C" {
#endif

struct spa_device;

#include <spa/utils/defs.h>
#include <spa/utils/dict.h>
#include <spa/support/plugin.h>
#include <spa/pod/builder.h>
#include <spa/pod/event.h>

struct spa_device_info {
#define SPA_VERSION_DEVICE_INFO 0
	uint32_t version;

#define SPA_DEVICE_CHANGE_MASK_INFO		(1<<0)
#define SPA_DEVICE_CHANGE_MASK_PARAMS		(1<<1)
	uint64_t change_mask;
	const struct spa_dict *info;
	uint32_t n_params;
	uint32_t *params;
};

#define SPA_DEVICE_INFO_INIT()	(struct spa_device_info){ SPA_VERSION_DEVICE_INFO, }

struct spa_device_object_info {
#define SPA_VERSION_DEVICE_OBJECT_INFO 0
	uint32_t version;

	uint32_t type;
	const struct spa_handle_factory *factory;

#define SPA_DEVICE_OBJECT_CHANGE_MASK_INFO		(1<<0)
	uint64_t change_mask;
	const struct spa_dict *info;
};

#define SPA_DEVICE_OBJECT_INFO_INIT()	(struct spa_device_object_info){ SPA_VERSION_DEVICE_OBJECT_INFO, }

/**
 * spa_device_callbacks:
 */
struct spa_device_callbacks {
	/** version of the structure */
#define SPA_VERSION_DEVICE_CALLBACKS	0
	uint32_t version;

	/** notify extra information about the device */
	void (*info) (void *data, const struct spa_device_info *info);

	/** a device event */
	void (*event) (void *data, struct spa_event *event);

	/** info changed for an object managed by the device, info is NULL when
	 * the object is removed */
	void (*object_info) (void *data, uint32_t id,
			     const struct spa_device_object_info *info);
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
	 * Set callbacks to receive asynchronous notifications from
	 * the device.
	 *
	 * Setting the callbacks will trigger the info event and an
	 * add event for each managed node.
	 *
	 * \param device: a #spa_device
	 * \param callback: a #callbacks
	 * \return 0 on success
	 *	   < 0 errno on error
	 */
	int (*set_callbacks) (struct spa_device *device,
			      const struct spa_device_callbacks *callbacks,
			      void *data);
	/**
	 * Enumerate the parameters of a device.
	 *
	 * Parameters are identified with an \a id. Some parameters can have
	 * multiple values, see the documentation of the parameter id.
	 *
	 * Parameters can be filtered by passing a non-NULL \a filter.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param device a \ref spa_device
	 * \param id the param id to enumerate
	 * \param index the index of enumeration, pass 0 for the first item and the
	 *	index is updated to retrieve the next item.
	 * \param filter and optional filter to use
	 * \param param result param or NULL
	 * \param builder builder for the param object.
	 * \return 1 on success and \a param contains the result
	 *         0 when there are no more parameters to enumerate
	 *         -EINVAL when invalid arguments are given
	 *         -ENOENT the parameter \a id is unknown
	 *         -ENOTSUP when there are no parameters
	 *                 implemented on \a device
	 */
	int (*enum_params) (struct spa_device *device,
			    uint32_t id, uint32_t *index,
			    const struct spa_pod *filter,
			    struct spa_pod **param,
			    struct spa_pod_builder *builder);
	/**
	 * Set the configurable parameter in \a device.
	 *
	 * Usually, \a param will be obtained from enum_params and then
	 * modified but it is also possible to set another spa_pod
	 * as long as its keys and types match a supported object.
	 *
	 * Objects with property keys that are not known are ignored.
	 *
	 * This function must be called from the main thread.
	 *
	 * \param device a \ref spa_device
	 * \param id the parameter id to configure
	 * \param flags additional flags
	 * \param param the parameter to configure
	 *
	 * \return 0 on success
	 *         -EINVAL when invalid arguments are given
	 *         -ENOTSUP when there are no parameters implemented on \a device
	 *         -ENOENT the parameter is unknown
	 */
	int (*set_param) (struct spa_device *device,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod *param);
};

#define spa_device_set_callbacks(d,...)	(d)->set_callbacks((d),__VA_ARGS__)
#define spa_device_enum_params(d,...)	(d)->enum_params((d),__VA_ARGS__)
#define spa_device_set_param(d,...)	(d)->set_param((d),__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_DEVICE_H */
