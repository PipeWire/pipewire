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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#include "config.h"

#include <spa/node/node.h>
#include <spa/utils/hook.h>
#include <spa/utils/names.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/dict.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"

#define NAME "media-session"

struct impl;
struct object;

struct node {
	struct impl *impl;
	struct object *object;
	struct spa_list link;
	uint32_t id;

	struct spa_handle *handle;
	struct pw_proxy *proxy;
	struct spa_node *node;
};

struct object {
	struct impl *impl;
	struct spa_list link;
	uint32_t id;

	struct spa_handle *handle;
	struct pw_proxy *proxy;
	struct spa_device *device;
	struct spa_hook device_listener;

	struct spa_list node_list;
};

struct impl {
	struct timespec now;

	struct pw_main_loop *loop;
	struct pw_core *core;

	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct spa_handle *monitor_handle;
	struct spa_monitor *monitor;
	struct spa_hook monitor_listener;

	struct spa_list object_list;
};

static struct node *find_node(struct object *obj, uint32_t id)
{
	struct node *node;

	spa_list_for_each(node, &obj->node_list, link) {
		if (node->id == id)
			return node;
	}
	return NULL;
}

static void update_node(struct object *obj, struct node *node,
		const struct spa_device_object_info *info)
{
	pw_log_debug("update node %u", node->id);
	spa_debug_dict(0, info->props);
}

static struct node *create_node(struct object *obj, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct node *node;
	struct impl *impl = obj->impl;
	struct pw_core *core = impl->core;
	struct spa_handle *handle;
	int res;
	void *iface;

	pw_log_debug("new node %u", id);

	if (info->type != SPA_TYPE_INTERFACE_Node)
		return NULL;

	handle = pw_core_load_spa_handle(core,
			info->factory_name,
			info->props);
	if (handle == NULL) {
		pw_log_error("can't make factory instance: %m");
		goto exit;
	}

	if ((res = spa_handle_get_interface(handle, info->type, &iface)) < 0) {
		pw_log_error("can't get %d interface: %d", info->type, res);
		goto unload_handle;
	}

	node = calloc(1, sizeof(*node));
	if (node == NULL)
		goto unload_handle;

	node->impl = impl;
	node->object = obj;
	node->id = id;
	node->handle = handle;
	node->node = iface;
	node->proxy = pw_remote_export(impl->remote,
			info->type, pw_properties_new_dict(info->props), node->node, 0);
	if (node->proxy == NULL)
		goto clean_node;

	spa_list_append(&obj->node_list, &node->link);

	update_node(obj, node, info);

	return node;

clean_node:
	free(node);
unload_handle:
	pw_unload_spa_handle(handle);
exit:
	return NULL;
}

static void remove_node(struct object *obj, struct node *node)
{
	pw_log_debug("remove node %u", node->id);
	spa_list_remove(&node->link);
	pw_proxy_destroy(node->proxy);
	free(node->handle);
	free(node);
}

static void device_object_info(void *data, uint32_t id,
                const struct spa_device_object_info *info)
{
	struct object *obj = data;
	struct node *node;

	node = find_node(obj, id);

	if (info == NULL) {
		if (node == NULL) {
			pw_log_warn("object %p: unknown node %u", obj, id);
			return;
		}
		remove_node(obj, node);
	} else if (node == NULL) {
		create_node(obj, id, info);
	} else {
		update_node(obj, node, info);
	}

}

static const struct spa_device_events device_events = {
	SPA_VERSION_DEVICE_EVENTS,
	.object_info = device_object_info
};

static struct object *find_object(struct impl *impl, uint32_t id)
{
	struct object *obj;

	spa_list_for_each(obj, &impl->object_list, link) {
		if (obj->id == id)
			return obj;
	}
	return NULL;
}

static void update_object(struct impl *impl, struct object *obj,
		const struct spa_monitor_object_info *info)
{
	pw_log_debug("update object %u", obj->id);
	spa_debug_dict(0, info->props);
}

static struct object *create_object(struct impl *impl, uint32_t id,
		const struct spa_monitor_object_info *info)
{
	struct pw_core *core = impl->core;
	struct object *obj;
	struct spa_handle *handle;
	int res;
	void *iface;

	pw_log_debug("new object %u", id);

	if (info->type != SPA_TYPE_INTERFACE_Device)
		return NULL;

	handle = pw_core_load_spa_handle(core,
			info->factory_name,
			info->props);
	if (handle == NULL) {
		pw_log_error("can't make factory instance: %m");
		goto exit;
	}

	if ((res = spa_handle_get_interface(handle, info->type, &iface)) < 0) {
		pw_log_error("can't get %d interface: %d", info->type, res);
		goto unload_handle;
	}

	obj = calloc(1, sizeof(*obj));
	if (obj == NULL)
		goto unload_handle;

	obj->impl = impl;
	obj->id = id;
	obj->handle = handle;
	obj->device = iface;
	obj->proxy = pw_remote_export(impl->remote,
			info->type, pw_properties_new_dict(info->props), obj->device, 0);
	if (obj->proxy == NULL)
		goto clean_object;

	spa_list_init(&obj->node_list);

	spa_device_add_listener(obj->device,
			&obj->device_listener, &device_events, obj);

	spa_list_append(&impl->object_list, &obj->link);

	update_object(impl, obj, info);

	return obj;

clean_object:
	free(obj);
unload_handle:
	pw_unload_spa_handle(handle);
exit:
	return NULL;
}

static void remove_object(struct impl *impl, struct object *obj)
{
	pw_log_debug("remove object %u", obj->id);
	spa_list_remove(&obj->link);
	spa_hook_remove(&obj->device_listener);
	pw_proxy_destroy(obj->proxy);
	free(obj->handle);
	free(obj);
}

static int monitor_object_info(void *data, uint32_t id,
                const struct spa_monitor_object_info *info)
{
	struct impl *impl = data;
	struct object *obj;

	obj = find_object(impl, id);

	if (info == NULL) {
		if (obj == NULL)
			return -ENODEV;
		remove_object(impl, obj);
	} else if (obj == NULL) {
		if ((obj = create_object(impl, id, info)) == NULL)
			return -ENOMEM;
	} else {
		update_object(impl, obj, info);
	}
	return 0;
}

static const struct spa_monitor_callbacks monitor_callbacks =
{
	SPA_VERSION_MONITOR_CALLBACKS,
	.object_info = monitor_object_info,
};

static int start_monitor(struct impl *impl)
{
	struct spa_handle *handle;
	int res;
	void *iface;

	handle = pw_core_load_spa_handle(impl->core, SPA_NAME_API_BLUEZ5_MONITOR, NULL);
	if (handle == NULL) {
		res = -errno;
		goto out;
	}

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Monitor, &iface)) < 0) {
		pw_log_error("can't get MONITOR interface: %d", res);
		goto out_unload;
	}

	impl->monitor_handle = handle;
	impl->monitor = iface;

	spa_monitor_set_callbacks(impl->monitor, &monitor_callbacks, impl);

	return 0;

      out_unload:
	pw_unload_spa_handle(handle);
      out:
	return res;
}

static void on_state_changed(void *_data, enum pw_remote_state old, enum pw_remote_state state, const char *error)
{
	struct impl *impl = _data;
	int res;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		pw_log_error(NAME" %p: remote error: %s", impl, error);
		pw_main_loop_quit(impl->loop);
		break;

	case PW_REMOTE_STATE_CONNECTED:
		pw_log_info(NAME" %p: connected", impl);
		if ((res = start_monitor(impl)) < 0) {
			pw_log_debug("error starting monitor: %s", spa_strerror(res));
			pw_main_loop_quit(impl->loop);
		}
		break;

	case PW_REMOTE_STATE_UNCONNECTED:
		pw_log_info(NAME" %p: disconnected", impl);
		pw_main_loop_quit(impl->loop);
		break;

	default:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));
		break;
	}
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_state_changed,
};

int main(int argc, char *argv[])
{
	struct impl impl = { 0, };

	pw_init(&argc, &argv);

	impl.loop = pw_main_loop_new(NULL);
	impl.core = pw_core_new(pw_main_loop_get_loop(impl.loop), NULL, 0);
        impl.remote = pw_remote_new(impl.core, NULL, 0);

	pw_core_add_spa_lib(impl.core, "api.bluez5.*", "bluez5/libspa-bluez5");

	pw_module_load(impl.core, "libpipewire-module-client-device", NULL, NULL);

	clock_gettime(CLOCK_MONOTONIC, &impl.now);

	spa_list_init(&impl.object_list);

	pw_remote_add_listener(impl.remote,
			&impl.remote_listener,
			&remote_events, &impl);

	if (pw_remote_connect(impl.remote) < 0)
		return -1;

	pw_main_loop_run(impl.loop);

	pw_core_destroy(impl.core);
	pw_main_loop_destroy(impl.loop);

	return 0;
}
