/* PipeWire
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/debug/types.h>

#include "spa-device.h"
#include "pipewire/device.h"
#include "pipewire/port.h"
#include "pipewire/log.h"
#include "pipewire/private.h"
#include "pipewire/pipewire.h"

struct impl {
	struct pw_device *this;

	struct pw_client *owner;
	struct pw_global *parent;

	enum pw_spa_device_flags flags;

	void *unload;
        struct spa_handle *handle;
        struct spa_device *device;
	char *factory_name;

	struct spa_hook device_listener;

	void *user_data;
};

static void device_destroy(void *data)
{
	struct impl *impl = data;
	struct pw_device *device = impl->this;

	pw_log_debug("spa-device %p: free", device);

	spa_hook_remove(&impl->device_listener);
	if (impl->handle)
		pw_unload_spa_handle(impl->handle);
	free(impl->factory_name);
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.destroy = device_destroy,
};

struct pw_device *
pw_spa_device_new(struct pw_core *core,
		  struct pw_client *owner,
		  struct pw_global *parent,
		  const char *name,
		  enum pw_spa_device_flags flags,
		  struct spa_device *device,
		  struct spa_handle *handle,
		  struct pw_properties *properties,
		  size_t user_data_size)
{
	struct pw_device *this;
	struct impl *impl;

	this = pw_device_new(core, name, properties, sizeof(struct impl) + user_data_size);
	if (this == NULL)
		return NULL;

	impl = this->user_data;
	impl->this = this;
	impl->owner = owner;
	impl->parent = parent;
	impl->device = device;
	impl->handle = handle;
	impl->flags = flags;

	if (user_data_size > 0)
                impl->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	pw_device_add_listener(this, &impl->device_listener, &device_events, impl);
	pw_device_set_implementation(this, impl->device);

	if (!SPA_FLAG_CHECK(impl->flags, PW_SPA_DEVICE_FLAG_NO_REGISTER))
		pw_device_register(this, impl->owner, impl->parent, NULL);

	return this;
}

void *pw_spa_device_get_user_data(struct pw_device *device)
{
	struct impl *impl = device->user_data;
	return impl->user_data;
}

struct pw_device *pw_spa_device_load(struct pw_core *core,
				 struct pw_client *owner,
				 struct pw_global *parent,
				 const char *factory_name,
				 const char *name,
				 enum pw_spa_device_flags flags,
				 struct pw_properties *properties,
				 size_t user_data_size)
{
	struct pw_device *this;
	struct impl *impl;
	struct spa_handle *handle;
	const struct spa_support *support;
	const char *lib = NULL;
	uint32_t n_support;
	void *iface;
	int res;

	support = pw_core_get_support(core, &n_support);

	if (lib == NULL && properties)
		lib = pw_properties_get(properties, SPA_KEY_LIBRARY_NAME);
	if (lib == NULL)
		lib = pw_core_find_spa_lib(core, factory_name);
	if (lib == NULL)
		goto exit;

	handle = pw_load_spa_handle(lib, factory_name,
			properties ? &properties->dict : NULL, n_support, support);
	if (handle == NULL)
		goto exit;

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Device, &iface)) < 0) {
		pw_log_error("can't get device interface %d\n", res);
		goto exit_unload;
	}

	this = pw_spa_device_new(core, owner, parent, name, flags,
			       iface, handle, properties, user_data_size);
	if (this == NULL)
		goto exit;

	impl = this->user_data;
	impl->factory_name = strdup(factory_name);

	return this;

exit_unload:
	pw_unload_spa_handle(handle);
exit:
	return NULL;
}
