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
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include "config.h"

#include <spa/utils/result.h>

#include "pipewire/impl.h"

#include "spa-device.h"

#define NAME "spa-device-factory"

#define FACTORY_USAGE	SPA_KEY_FACTORY_NAME"=<factory-name> " \
			"["SPA_KEY_LIBRARY_NAME"=<library-name>]"

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Provide a factory to make SPA devices" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct factory_data {
	struct pw_context *context;
	struct pw_impl_module *module;
	struct pw_impl_factory *this;

	struct spa_hook factory_listener;
	struct spa_hook module_listener;

	struct spa_list device_list;
};

struct device_data {
	struct spa_list link;
	struct pw_impl_device *device;
	struct spa_hook device_listener;
};

static void device_destroy(void *data)
{
	struct device_data *nd = data;
	spa_list_remove(&nd->link);
	spa_hook_remove(&nd->device_listener);
}

static const struct pw_impl_device_events device_events = {
	PW_VERSION_IMPL_DEVICE_EVENTS,
	.destroy = device_destroy,
};

static void *create_object(void *_data,
			   struct pw_resource *resource,
			   const char *type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	struct factory_data *data = _data;
	struct pw_context *context = data->context;
	struct pw_impl_device *device;
	const char *factory_name;
	struct device_data *nd;
	struct pw_impl_client *client;
	int res;

	if (properties == NULL)
		goto error_properties;

	factory_name = pw_properties_get(properties, SPA_KEY_FACTORY_NAME);
	if (factory_name == NULL)
		goto error_properties;

	pw_properties_setf(properties, PW_KEY_FACTORY_ID, "%d",
			pw_global_get_id(pw_impl_factory_get_global(data->this)));

	client = resource ? pw_resource_get_client(resource) : NULL;

	if (client) {
		pw_properties_setf(properties, PW_KEY_CLIENT_ID, "%d",
			pw_global_get_id(pw_impl_client_get_global(client)));
	}

	device = pw_spa_device_load(context,
				factory_name,
				0,
				properties,
				sizeof(struct device_data));
	if (device == NULL) {
		res = -errno;
		goto error_device;
	}

	nd = pw_spa_device_get_user_data(device);
	nd->device = device;
	spa_list_append(&data->device_list, &nd->link);

	pw_impl_device_add_listener(device, &nd->device_listener, &device_events, nd);

	if (client) {
		pw_global_bind(pw_impl_device_get_global(device),
				client,
				PW_PERM_RWX, version,
				new_id);
	}
	return device;

error_properties:
	res = -EINVAL;
	pw_log_error("factory %p: usage: " FACTORY_USAGE, data->this);
	if (resource)
		pw_resource_errorf_id(resource, new_id, res,
				"usage: "FACTORY_USAGE);
	goto error_exit;
error_device:
	pw_log_error("can't create device: %s", spa_strerror(res));
	if (resource)
		pw_resource_errorf_id(resource, new_id, res,
				"can't create device: %s", spa_strerror(res));
	goto error_exit;
error_exit:
	errno = -res;
	return NULL;
}

static const struct pw_impl_factory_implementation factory_impl = {
	PW_VERSION_IMPL_FACTORY_IMPLEMENTATION,
	.create_object = create_object,
};

static void factory_destroy(void *_data)
{
	struct factory_data *data = _data;
	struct device_data *nd;

	spa_hook_remove(&data->module_listener);

	spa_list_consume(nd, &data->device_list, link)
		pw_impl_device_destroy(nd->device);
}

static const struct pw_impl_factory_events factory_events = {
	PW_VERSION_IMPL_FACTORY_EVENTS,
	.destroy = factory_destroy,
};

static void module_destroy(void *_data)
{
	struct factory_data *data = _data;
	pw_impl_factory_destroy(data->this);
}

static void module_registered(void *data)
{
	struct factory_data *d = data;
	struct pw_impl_module *module = d->module;
	struct pw_impl_factory *factory = d->this;
	struct spa_dict_item items[1];
	char id[16];
	int res;

	snprintf(id, sizeof(id), "%d", pw_global_get_id(pw_impl_module_get_global(module)));
	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_MODULE_ID, id);
	pw_impl_factory_update_properties(factory, &SPA_DICT_INIT(items, 1));

	if ((res = pw_impl_factory_register(factory, NULL)) < 0) {
		pw_log_error(NAME" %p: can't register factory: %s", factory, spa_strerror(res));
	}
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
	.registered = module_registered,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_impl_factory *factory;
	struct factory_data *data;

	factory = pw_context_create_factory(context,
				 "spa-device-factory",
				 PW_TYPE_INTERFACE_Device,
				 PW_VERSION_DEVICE,
				 NULL,
				 sizeof(*data));
	if (factory == NULL)
		return -errno;

	data = pw_impl_factory_get_user_data(factory);
	data->this = factory;
	data->module = module;
	data->context = context;
	spa_list_init(&data->device_list);

	pw_impl_factory_add_listener(factory, &data->factory_listener, &factory_events, data);
	pw_impl_factory_set_implementation(factory, &factory_impl, data);

	pw_log_debug("module %p: new", module);
	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	pw_impl_module_add_listener(module, &data->module_listener, &module_events, data);

	return 0;
}
