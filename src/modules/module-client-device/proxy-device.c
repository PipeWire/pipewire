/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <spa/pod/parser.h>
#include <spa/monitor/device.h>

#include "pipewire/pipewire.h"

struct device_data {
	struct spa_device *device;
	struct spa_hook device_listener;
	struct spa_hook device_methods;

        struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
};

static void proxy_device_destroy(void *_data)
{
	struct device_data *data = _data;
	spa_hook_remove(&data->device_listener);
	spa_hook_remove(&data->device_methods);
	spa_hook_remove(&data->proxy_listener);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = proxy_device_destroy,
};

struct pw_proxy *pw_core_spa_device_export(struct pw_core *core,
		const char *type, const struct spa_dict *props, void *object,
		size_t user_data_size)
{
	struct spa_device *device = object;
	struct spa_interface *iface, *diface;
	struct pw_proxy *proxy;
	struct device_data *data;

	proxy = pw_core_create_object(core,
				    "client-device",
				    SPA_TYPE_INTERFACE_Device,
				    SPA_VERSION_DEVICE,
				    props,
				    user_data_size + sizeof(struct device_data));
        if (proxy == NULL)
		return NULL;

	data = pw_proxy_get_user_data(proxy);
	data = SPA_PTROFF(data, user_data_size, struct device_data);
	data->device = device;
	data->proxy = proxy;

	iface = (struct spa_interface*)proxy;
	diface = (struct spa_interface*)device;

	pw_proxy_add_listener(proxy, &data->proxy_listener, &proxy_events, data);

	pw_proxy_add_object_listener(proxy, &data->device_methods,
			diface->cb.funcs, diface->cb.data);
	spa_device_add_listener(device, &data->device_listener,
			iface->cb.funcs, iface->cb.data);

	return proxy;
}
