/* PipeWire
 *
 * Copyright © 2019 Wim Taymans
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

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <spa/pod/parser.h>
#include <spa/monitor/device.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"

struct device_data {
	struct pw_remote *remote;
	struct pw_core *core;

	struct spa_device *device;
	struct spa_hook device_listener;
	struct spa_hook device_methods;

        struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
};

static void device_proxy_destroy(void *_data)
{
	struct device_data *data = _data;
	spa_hook_remove(&data->device_listener);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = device_proxy_destroy,
};

struct pw_proxy *pw_remote_spa_device_export(struct pw_remote *remote,
		uint32_t type, struct pw_properties *props, void *object,
		size_t user_data_size)
{
	struct spa_device *device = object;
	struct spa_interface *iface;
	struct pw_proxy *proxy;
	struct device_data *data;

	proxy = pw_core_proxy_create_object(remote->core_proxy,
					    "client-device",
					    SPA_TYPE_INTERFACE_Device,
					    SPA_VERSION_DEVICE,
					    &props->dict,
					    user_data_size + sizeof(struct device_data));
        if (proxy == NULL)
                return NULL;

	data = pw_proxy_get_user_data(proxy);
	data = SPA_MEMBER(data, user_data_size, struct device_data);
	data->remote = remote;
	data->device = device;
	data->core = pw_remote_get_core(remote);
	data->proxy = proxy;

	iface = (struct spa_interface*)proxy;

	pw_proxy_add_listener(proxy, &data->proxy_listener, &proxy_events, data);

	pw_proxy_add_object_listener(proxy, &data->device_methods,
			device->iface.cb.funcs, device->iface.cb.data);
	spa_device_add_listener(device, &data->device_listener,
			iface->cb.funcs, iface->cb.data);

	return proxy;
}
