/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include "config.h"

#include <spa/utils/result.h>

#include <pipewire/impl.h>

#include "module-client-device/client-device.h"

/** \page page_module_client_device PipeWire Module: Client Device
 */

#define NAME "client-device"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Allow clients to create and control remote devices" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct pw_proxy *pw_core_spa_device_export(struct pw_core *core,
		const char *type, const struct spa_dict *props, void *object,
		size_t user_data_size);

struct pw_protocol *pw_protocol_native_ext_client_device_init(struct pw_context *context);

struct factory_data {
	struct pw_impl_factory *factory;
	struct spa_hook factory_listener;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_export_type export_spadevice;
};

static void *create_object(void *_data,
			   struct pw_resource *resource,
			   const char *type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	struct factory_data *data = _data;
	struct pw_impl_factory *factory = data->factory;
	void *result;
	struct pw_resource *device_resource;
	struct pw_impl_client *client;
	int res;

	if (resource == NULL) {
		res = -EINVAL;
		goto error_exit;
	}

	client = pw_resource_get_client(resource);
	device_resource = pw_resource_new(client, new_id, PW_PERM_ALL, type, version, 0);
	if (device_resource == NULL) {
		res = -errno;
		goto error_resource;
	}

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL) {
		res = -errno;
		goto error_properties;
	}

	pw_properties_setf(properties, PW_KEY_FACTORY_ID, "%d",
			pw_global_get_id(pw_impl_factory_get_global(factory)));
	pw_properties_setf(properties, PW_KEY_CLIENT_ID, "%d",
			pw_global_get_id(pw_impl_client_get_global(client)));

	result = pw_client_device_new(device_resource, properties);
	if (result == NULL) {
		res = -errno;
		goto error_device;
	}
	return result;

error_resource:
	pw_log_error("can't create resource: %s", spa_strerror(res));
	pw_resource_errorf_id(resource, new_id, res, "can't create resource: %s", spa_strerror(res));
	goto error_exit;
error_properties:
	pw_log_error("can't create properties: %s", spa_strerror(res));
	pw_resource_errorf_id(resource, new_id, res, "can't create properties: %s", spa_strerror(res));
	goto error_exit_free;
error_device:
	pw_log_error("can't create device: %s", spa_strerror(res));
	pw_resource_errorf_id(resource, new_id, res, "can't create device: %s", spa_strerror(res));
	goto error_exit_free;

error_exit_free:
	pw_resource_remove(device_resource);
error_exit:
	errno = -res;
	return NULL;
}

static const struct pw_impl_factory_implementation impl_factory = {
	PW_VERSION_IMPL_FACTORY_IMPLEMENTATION,
	.create_object = create_object,
};

static void factory_destroy(void *data)
{
	struct factory_data *d = data;
	spa_hook_remove(&d->factory_listener);
	d->factory = NULL;
	if (d->module)
		pw_impl_module_destroy(d->module);
}

static const struct pw_impl_factory_events factory_events = {
	PW_VERSION_IMPL_FACTORY_EVENTS,
	.destroy = factory_destroy,
};

static void module_destroy(void *data)
{
	struct factory_data *d = data;
	spa_hook_remove(&d->module_listener);
	spa_list_remove(&d->export_spadevice.link);
	d->module = NULL;
	if (d->factory)
		pw_impl_factory_destroy(d->factory);
}

static void module_registered(void *data)
{
	struct factory_data *d = data;
	struct pw_impl_module *module = d->module;
	struct pw_impl_factory *factory = d->factory;
	struct spa_dict_item items[1];
	char id[16];
	int res;

	snprintf(id, sizeof(id), "%d", pw_global_get_id(pw_impl_module_get_global(module)));
	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_MODULE_ID, id);
	pw_impl_factory_update_properties(factory, &SPA_DICT_INIT(items, 1));

	if ((res = pw_impl_factory_register(factory, NULL)) < 0) {
		pw_log_error("%p: can't register factory: %s", factory, spa_strerror(res));
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
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);
	factory = pw_context_create_factory(context,
				 "client-device",
				 SPA_TYPE_INTERFACE_Device,
				 SPA_VERSION_DEVICE,
				 pw_properties_new(
					 PW_KEY_FACTORY_USAGE, CLIENT_DEVICE_USAGE,
					 NULL),
				 sizeof(*data));
	if (factory == NULL)
		return -errno;

	data = pw_impl_factory_get_user_data(factory);
	data->factory = factory;
	data->module = module;

	pw_log_debug("module %p: new", module);

	pw_impl_factory_set_implementation(factory,
				      &impl_factory,
				      data);

	data->export_spadevice.type = SPA_TYPE_INTERFACE_Device;
	data->export_spadevice.func = pw_core_spa_device_export;
	if ((res = pw_context_register_export_type(context, &data->export_spadevice)) < 0)
		goto error;

	pw_protocol_native_ext_client_device_init(context);

	pw_impl_factory_add_listener(factory, &data->factory_listener, &factory_events, data);
	pw_impl_module_add_listener(module, &data->module_listener, &module_events, data);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
error:
	pw_impl_factory_destroy(data->factory);
	return res;
}
