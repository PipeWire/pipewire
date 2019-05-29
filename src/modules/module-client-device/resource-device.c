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

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/eventfd.h>

#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>
#include <spa/debug/types.h>

#include <pipewire/pipewire.h>
#include "pipewire/private.h"

struct impl {
	struct pw_core *core;
	struct pw_device *device;
	struct spa_hook device_listener;

	struct spa_device impl;
	struct spa_hook_list hooks;

	struct pw_resource *resource;
	struct spa_hook resource_listener;
	struct spa_hook implementation_listener;

	struct pw_global *parent;
	unsigned int registered:1;
};

#define pw_device_resource(r,m,v,...)      \
        pw_resource_call_res(r,struct spa_device_methods,m,v,__VA_ARGS__)

#define pw_device_resource_enum_params(r,...)  \
        pw_device_resource(r,enum_params,0,__VA_ARGS__)
#define pw_device_resource_set_param(r,...)  \
        pw_device_resource(r,set_param,0,__VA_ARGS__)

static int device_add_listener(void *object,
                        struct spa_hook *listener,
                        const struct spa_device_events *events,
                        void *data)
{
	struct impl *impl = object;
        struct spa_hook_list save;

        spa_hook_list_isolate(&impl->hooks, &save, listener, events, data);
	pw_log_debug("client-device %p: add listener", impl);

        spa_hook_list_join(&impl->hooks, &save);

        return 0;
}

static int device_sync(void *object, int seq)
{
	struct impl *impl = object;

	pw_log_debug("client-device %p: sync %d", impl, seq);

	return pw_resource_ping(impl->resource, seq);
}

static int device_enum_params(void *object, int seq,
                            uint32_t id, uint32_t index, uint32_t max,
                            const struct spa_pod *filter)
{
	struct impl *impl = object;
	pw_log_debug("client-device %p: enum params", impl);
	return pw_device_resource_enum_params(impl->resource, seq, id, index, max, filter);
}

static int device_set_param(void *object,
                          uint32_t id, uint32_t flags,
                          const struct spa_pod *param)
{
	struct impl *impl = object;
	pw_log_debug("client-device %p: set param", impl);
	return pw_device_resource_set_param(impl->resource, id, flags, param);
}

static const struct spa_device_methods impl_device = {
	SPA_VERSION_DEVICE_METHODS,
	.add_listener = device_add_listener,
	.sync = device_sync,
	.enum_params = device_enum_params,
	.set_param = device_set_param,
};

static void device_info(void *data, const struct spa_device_info *info)
{
	struct impl *impl = data;
	spa_device_emit_info(&impl->hooks, info);

	if (!impl->registered) {
		pw_device_register(impl->device, impl->resource->client, impl->parent, NULL);
		impl->registered = true;
	}
}

static void device_result(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct impl *impl = data;
	pw_log_debug("client-device %p: result %d %d %u", impl, seq, res, type);
	spa_device_emit_result(&impl->hooks, seq, res, type, result);
}

static void device_event(void *data, const struct spa_event *event)
{
	struct impl *impl = data;
	spa_device_emit_event(&impl->hooks, event);
}

static void device_object_info(void *data, uint32_t id,
                const struct spa_device_object_info *info)
{
	struct impl *impl = data;
	spa_device_emit_object_info(&impl->hooks, id, info);
}

static const struct spa_device_events resource_implementation = {
	SPA_VERSION_DEVICE_EVENTS,
	.info = device_info,
	.result = device_result,
	.event = device_event,
	.object_info = device_object_info,
};

static void device_resource_destroy(void *data)
{
	struct impl *impl = data;

	pw_log_debug("client-device %p: destroy", impl);

	impl->resource = NULL;
	spa_hook_remove(&impl->device_listener);
	spa_hook_remove(&impl->resource_listener);
	spa_hook_remove(&impl->implementation_listener);
	pw_device_destroy(impl->device);
}

static void device_resource_pong(void *data, int seq)
{
	struct impl *impl = data;
	spa_device_emit_result(&impl->hooks, seq, 0, 0, NULL);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = device_resource_destroy,
	.pong = device_resource_pong,
};

static void device_destroy(void *data)
{
	struct impl *impl = data;

	pw_log_debug("client-device %p: destroy", impl);

	impl->device = NULL;
	spa_hook_remove(&impl->device_listener);
	spa_hook_remove(&impl->resource_listener);
	spa_hook_remove(&impl->implementation_listener);
	pw_resource_destroy(impl->resource);
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.destroy = device_destroy,
};

struct pw_device *pw_client_device_new(struct pw_resource *resource,
		struct pw_global *parent,
		struct pw_properties *properties)
{
	struct impl *impl;
	struct pw_device *device;
	struct pw_client *client = pw_resource_get_client(resource);
	struct pw_core *core = pw_client_get_core(client);
	const char *name;

	if ((name = pw_properties_get(properties, PW_KEY_DEVICE_NAME)) == NULL)
		name = "client-device";

	device = pw_device_new(core, name, properties, sizeof(struct impl));
	if (device == NULL)
		return NULL;

	impl = pw_device_get_user_data(device);
	impl->device = device;
	impl->core = core;
	impl->resource = resource;
	impl->parent = parent;

	impl->impl.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, impl);
	spa_hook_list_init(&impl->hooks);
	pw_device_set_implementation(device, &impl->impl);

	pw_device_add_listener(impl->device,
			&impl->device_listener,
			&device_events, impl);
	pw_resource_add_listener(impl->resource,
				&impl->resource_listener,
				&resource_events,
				impl);
	pw_resource_add_object_listener(impl->resource,
				&impl->implementation_listener,
				&resource_implementation,
				impl);

	return device;
}
