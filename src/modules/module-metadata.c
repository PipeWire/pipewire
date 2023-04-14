/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/json.h>

#include <pipewire/impl.h>
#include <pipewire/extensions/metadata.h>

/** \page page_module_metadata PipeWire Module: Metadata
 */

#define NAME "metadata"

#define FACTORY_USAGE   "("PW_KEY_METADATA_NAME" = <name> ) "						\
                        "("PW_KEY_METADATA_VALUES" = [ "						\
                        "   { ( id = <int> ) key = <string> ( type = <string> ) value = <json> } "	\
                        "   ..."									\
                        "  ] )"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Allow clients to create metadata store" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};


struct pw_metadata *pw_metadata_new(struct pw_context *context, struct pw_resource *resource,
		   struct pw_properties *properties);

struct pw_proxy *pw_core_metadata_export(struct pw_core *core,
		const char *type, const struct spa_dict *props, void *object, size_t user_data_size);

int pw_protocol_native_ext_metadata_init(struct pw_context *context);

struct factory_data {
	struct pw_impl_factory *factory;
	struct spa_hook factory_listener;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_export_type export_metadata;
};

/*
 * [
 *     { ( "id" = <int>, ) "key" = <string> ("type" = <string>) "value" = <json> }
 *     ....
 * ]
 */
static int fill_metadata(struct pw_metadata *metadata, const char *str)
{
	struct spa_json it[3];

	spa_json_init(&it[0], str, strlen(str));
	if (spa_json_enter_array(&it[0], &it[1]) <= 0)
		return -EINVAL;

	while (spa_json_enter_object(&it[1], &it[2]) > 0) {
		char key[256], *k = NULL, *v = NULL, *t = NULL;
		int id = 0;

		while (spa_json_get_string(&it[2], key, sizeof(key)) > 0) {
			int len;
			const char *val;

			if ((len = spa_json_next(&it[2], &val)) <= 0)
				return -EINVAL;

			if (spa_streq(key, "id")) {
				if (spa_json_parse_int(val, len, &id) <= 0)
					return -EINVAL;
			} else if (spa_streq(key, "key")) {
				if ((k = malloc(len+1)) != NULL)
					spa_json_parse_stringn(val, len, k, len+1);
			} else if (spa_streq(key, "type")) {
				if ((t = malloc(len+1)) != NULL)
					spa_json_parse_stringn(val, len, t, len+1);
			} else if (spa_streq(key, "value")) {
				if (spa_json_is_container(val, len))
					len = spa_json_container_len(&it[2], val, len);
				if ((v = malloc(len+1)) != NULL)
					spa_json_parse_stringn(val, len, v, len+1);
			}
		}
		if (k != NULL && v != NULL)
			pw_metadata_set_property(metadata, id, k, t, v);
		free(k);
		free(v);
		free(t);
	}
	return 0;
}

static void *create_object(void *_data,
			   struct pw_resource *resource,
			   const char *type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	struct factory_data *data = _data;
	struct pw_context *context = pw_impl_module_get_context(data->module);
	struct pw_metadata *result;
	struct pw_resource *metadata_resource = NULL;
	struct pw_impl_client *client = resource ? pw_resource_get_client(resource) : NULL;
	const char *str;
	int res;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		return NULL;

	pw_properties_setf(properties, PW_KEY_FACTORY_ID, "%d",
			pw_impl_factory_get_info(data->factory)->id);
	pw_properties_setf(properties, PW_KEY_MODULE_ID, "%d",
			pw_impl_module_get_info(data->module)->id);

	if (pw_properties_get(properties, PW_KEY_METADATA_NAME) == NULL)
		pw_properties_set(properties, PW_KEY_METADATA_NAME, "default");

	if (client) {
		metadata_resource = pw_resource_new(client, new_id, PW_PERM_ALL, type, version, 0);
		if (metadata_resource == NULL) {
			res = -errno;
			goto error_resource;
		}

		pw_properties_setf(properties, PW_KEY_CLIENT_ID, "%d",
				pw_impl_client_get_info(client)->id);

		result = pw_metadata_new(context, metadata_resource, properties);
		if (result == NULL) {
			properties = NULL;
			res = -errno;
			goto error_node;
		}
	} else {
		struct pw_impl_metadata *impl;

		impl = pw_context_create_metadata(context, NULL, properties, 0);
		if (impl == NULL) {
			properties = NULL;
			res = -errno;
			goto error_node;
		}
		pw_impl_metadata_register(impl, NULL);
		result = pw_impl_metadata_get_implementation(impl);
	}
	if ((str = pw_properties_get(properties, PW_KEY_METADATA_VALUES)) != NULL)
		fill_metadata(result, str);

	return result;

error_resource:
	pw_resource_errorf_id(resource, new_id, res,
				"can't create resource: %s", spa_strerror(res));
	goto error_exit;
error_node:
	pw_resource_errorf_id(resource, new_id, res,
				"can't create metadata: %s", spa_strerror(res));
	goto error_exit_free;

error_exit_free:
	if (metadata_resource)
		pw_resource_remove(metadata_resource);
error_exit:
	pw_properties_free(properties);
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
	spa_list_remove(&d->export_metadata.link);
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

	if ((res = pw_protocol_native_ext_metadata_init(context)) < 0)
		return res;

	factory = pw_context_create_factory(context,
				 "metadata",
				 PW_TYPE_INTERFACE_Metadata,
				 PW_VERSION_METADATA,
				 pw_properties_new(
                                         PW_KEY_FACTORY_USAGE, FACTORY_USAGE,
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

	data->export_metadata.type = PW_TYPE_INTERFACE_Metadata;
	data->export_metadata.func = pw_core_metadata_export;
	if ((res = pw_context_register_export_type(context, &data->export_metadata)) < 0)
		goto error;

	pw_impl_factory_add_listener(factory, &data->factory_listener, &factory_events, data);
	pw_impl_module_add_listener(module, &data->module_listener, &module_events, data);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
error:
	pw_impl_factory_destroy(data->factory);
	return res;
}
