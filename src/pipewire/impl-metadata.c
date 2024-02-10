/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <spa/debug/types.h>
#include <spa/utils/string.h>

#include <pipewire/cleanup.h>
#include "pipewire/impl.h"
#include "pipewire/private.h"

#include "pipewire/extensions/metadata.h"

PW_LOG_TOPIC_EXTERN(log_metadata);
#define PW_LOG_TOPIC_DEFAULT log_metadata

#define pw_metadata_emit(hooks,method,version,...)			\
	spa_hook_list_call_simple(hooks, struct pw_metadata_events,	\
				method, version, ##__VA_ARGS__)

#define pw_metadata_emit_property(hooks,...)	pw_metadata_emit(hooks,property, 0, ##__VA_ARGS__)

struct metadata {
	struct spa_interface iface;
	struct pw_array storage;
	struct spa_hook_list hooks;		/**< event listeners */
};

struct item {
	uint32_t subject;
	char *key;
	char *type;
	char *value;
};

static void clear_item(struct item *item)
{
	free(item->key);
	free(item->type);
	free(item->value);
	spa_zero(*item);
}

static void set_item(struct item *item, uint32_t subject, const char *key, const char *type, const char *value)
{
	item->subject = subject;
	item->key = strdup(key);
	item->type = type ? strdup(type) : NULL;
	item->value = strdup(value);
}

static int change_item(struct item *item, const char *type, const char *value)
{
	int changed = 0;
	if (!spa_streq(item->type, type)) {
		free((char*)item->type);
		item->type = type ? strdup(type) : NULL;
		changed++;
	}
	if (!spa_streq(item->value, value)) {
		free((char*)item->value);
		item->value = value ? strdup(value) : NULL;
		changed++;
	}
	return changed;
}

static void emit_properties(struct metadata *this)
{
	struct item *item;
	pw_array_for_each(item, &this->storage) {
		pw_log_debug("metadata %p: %d %s %s %s",
				this, item->subject, item->key, item->type, item->value);
		pw_metadata_emit_property(&this->hooks,
				item->subject,
				item->key,
				item->type,
				item->value);
	}
}

static int impl_add_listener(void *object,
		struct spa_hook *listener,
		const struct pw_metadata_events *events,
		void *data)
{
	struct metadata *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	pw_log_debug("metadata %p:", this);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_properties(this);

	spa_hook_list_join(&this->hooks, &save);

        return 0;
}

static struct item *find_item(struct pw_array *storage, uint32_t subject, const char *key)
{
	struct item *item;

	pw_array_for_each(item, storage) {
		if (item->subject == subject && (key == NULL || spa_streq(item->key, key)))
			return item;
	}
	return NULL;
}

static int clear_subjects(struct metadata *this, struct pw_array *storage, uint32_t subject)
{
	struct item *item;
	uint32_t removed = 0;

	while (true) {
		item = find_item(storage, subject, NULL);
		if (item == NULL)
			break;

		pw_log_debug("%p: remove id:%d key:%s", this, subject, item->key);

		clear_item(item);
		pw_array_remove(storage, item);
		removed++;
	}
	if (removed > 0)
		pw_metadata_emit_property(&this->hooks, subject, NULL, NULL, NULL);

	return 0;
}

static void clear_items(struct metadata *this)
{
	struct item *item;
	struct pw_array tmp;

	/* copy to tmp and reinitialize the storage so that the callbacks
	 * will operate on the new empty metadata. Otherwise, if a callbacks
	 * adds new metadata we just keep on emptying the metadata forever. */
	tmp = this->storage;
	pw_array_init(&this->storage, 4096);

	pw_array_consume(item, &tmp)
		clear_subjects(this, &tmp, item->subject);
	pw_array_clear(&tmp);
}

static int impl_set_property(void *object,
			uint32_t subject,
			const char *key,
			const char *type,
			const char *value)
{
	struct metadata *this = object;
	struct item *item = NULL;
	int changed = 0;

	pw_log_debug("%p: id:%d key:%s type:%s value:%s", this, subject, key, type, value);

	if (key == NULL)
		return clear_subjects(this, &this->storage, subject);

	item = find_item(&this->storage, subject, key);
	if (value == NULL) {
		if (item != NULL) {
			clear_item(item);
			pw_array_remove(&this->storage, item);
			type = NULL;
			changed++;
			pw_log_info("%p: remove id:%d key:%s", this,
					subject, key);
		}
	} else if (item == NULL) {
		item = pw_array_add(&this->storage, sizeof(*item));
		if (item == NULL)
			return -errno;
		set_item(item, subject, key, type, value);
		changed++;
		pw_log_info("%p: add id:%d key:%s type:%s value:%s", this,
				subject, key, type, value);
	} else {
		if (type == NULL)
			type = item->type;
		changed = change_item(item, type, value);
		if (changed)
			pw_log_info("%p: change id:%d key:%s type:%s value:%s", this,
				subject, key, type, value);
	}

	if (changed) {
		pw_metadata_emit_property(&this->hooks,
					subject, key, type, value);
	}
	return 0;
}

static int impl_clear(void *object)
{
	struct metadata *this = object;
	clear_items(this);
	return 0;
}

static const struct pw_metadata_methods impl_metadata = {
	PW_VERSION_METADATA_METHODS,
	.add_listener = impl_add_listener,
	.set_property = impl_set_property,
	.clear = impl_clear,
};

static struct pw_metadata *metadata_init(struct metadata *this)
{
	this->iface = SPA_INTERFACE_INIT(
			PW_TYPE_INTERFACE_Metadata,
			PW_VERSION_METADATA,
			&impl_metadata, this);
	pw_array_init(&this->storage, 4096);
        spa_hook_list_init(&this->hooks);
	return (struct pw_metadata*)&this->iface;
}

static void metadata_reset(struct metadata *this)
{
	spa_hook_list_clean(&this->hooks);
	clear_items(this);
	pw_array_clear(&this->storage);
}

struct impl {
	struct pw_impl_metadata this;

	struct metadata def;
};

struct resource_data {
	struct pw_impl_metadata *impl;

	struct pw_resource *resource;
	struct spa_hook resource_listener;
	struct spa_hook object_listener;
	struct spa_hook metadata_listener;
};


static int metadata_property(void *data, uint32_t subject, const char *key,
		const char *type, const char *value)
{
	struct pw_impl_metadata *this = data;
	pw_impl_metadata_emit_property(this, subject, key, type, value);
	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_property,
};

SPA_EXPORT
struct pw_impl_metadata *pw_context_create_metadata(struct pw_context *context,
		const char *name, struct pw_properties *properties,
		size_t user_data_size)
{
	struct impl *impl;
	struct pw_impl_metadata *this;
	int res;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		return NULL;

	impl = calloc(1, sizeof(*impl) + user_data_size);
	if (impl == NULL) {
		res = -errno;
		goto error_exit;
	};
	this = &impl->this;

	this->context = context;
	this->properties = properties;

	if (name != NULL)
		pw_properties_set(properties, PW_KEY_METADATA_NAME, name);

	spa_hook_list_init(&this->listener_list);

	pw_impl_metadata_set_implementation(this, metadata_init(&impl->def));

	if (user_data_size > 0)
		this->user_data = SPA_PTROFF(this, sizeof(*this), void);

	pw_log_debug("%p: new", this);

	return this;

error_exit:
	pw_properties_free(properties);
	errno = -res;
	return NULL;
}

SPA_EXPORT
const struct pw_properties *pw_impl_metadata_get_properties(struct pw_impl_metadata *metadata)
{
	return metadata->properties;
}

SPA_EXPORT
int pw_impl_metadata_set_implementation(struct pw_impl_metadata *metadata,
		struct pw_metadata *meta)
{
	struct impl *impl = SPA_CONTAINER_OF(metadata, struct impl, this);

	if (metadata->metadata == meta)
		return 0;

	if (metadata->metadata)
		spa_hook_remove(&metadata->metadata_listener);
	if (meta == NULL)
		meta = (struct pw_metadata*)&impl->def.iface;

	metadata->metadata = meta;
	pw_metadata_add_listener(meta, &metadata->metadata_listener,
			&metadata_events, metadata);

	return 0;
}
SPA_EXPORT
struct pw_metadata *pw_impl_metadata_get_implementation(struct pw_impl_metadata *metadata)
{
	return metadata->metadata;
}

SPA_EXPORT
void pw_impl_metadata_destroy(struct pw_impl_metadata *metadata)
{
	struct impl *impl = SPA_CONTAINER_OF(metadata, struct impl, this);

	pw_log_debug("%p: destroy", metadata);
	pw_impl_metadata_emit_destroy(metadata);

	if (metadata->registered)
		spa_list_remove(&metadata->link);

	if (metadata->global) {
		spa_hook_remove(&metadata->global_listener);
		pw_global_destroy(metadata->global);
	}
	spa_hook_remove(&metadata->context_listener);

	pw_impl_metadata_emit_free(metadata);
	pw_log_debug("%p: free", metadata);

	metadata_reset(&impl->def);

	spa_hook_list_clean(&metadata->listener_list);

	pw_properties_free(metadata->properties);

	free(metadata);
}

#define pw_metadata_resource(r,m,v,...)      \
	pw_resource_call_res(r,struct pw_metadata_events,m,v,__VA_ARGS__)

#define pw_metadata_resource_property(r,...)        \
        pw_metadata_resource(r,property,0,__VA_ARGS__)

static int metadata_resource_property(void *data,
			uint32_t subject,
			const char *key,
			const char *type,
			const char *value)
{
	struct resource_data *d = data;
	struct pw_resource *resource = d->resource;
	struct pw_impl_client *client = pw_resource_get_client(resource);

	int res = pw_impl_client_check_permissions(client, subject, PW_PERM_R);
	if (res >= 0 ||
		    (res == -ENOENT && key == NULL && type == NULL && value == NULL))
		pw_metadata_resource_property(d->resource, subject, key, type, value);
	return 0;
}

static const struct pw_metadata_events metadata_resource_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_resource_property,
};

static int metadata_set_property(void *object,
			uint32_t subject,
			const char *key,
			const char *type,
			const char *value)
{
	struct resource_data *d = object;
	struct pw_impl_metadata *impl = d->impl;
	struct pw_resource *resource = d->resource;
	struct pw_impl_client *client = pw_resource_get_client(resource);
	int res;

	if ((res = pw_impl_client_check_permissions(client, subject, PW_PERM_R)) < 0)
		goto error;

	pw_metadata_set_property(impl->metadata, subject, key, type, value);
	return 0;

error:
	pw_resource_errorf(resource, res, "set property error for id %d: %s",
		subject, spa_strerror(res));
	return res;
}

static int metadata_clear(void *object)
{
	struct resource_data *d = object;
	struct pw_impl_metadata *impl = d->impl;
	pw_metadata_clear(impl->metadata);
	return 0;
}

static const struct pw_metadata_methods metadata_methods = {
	PW_VERSION_METADATA_METHODS,
	.set_property = metadata_set_property,
	.clear = metadata_clear,
};

static void global_unbind(void *data)
{
	struct resource_data *d = data;
	if (d->resource) {
	        spa_hook_remove(&d->resource_listener);
	        spa_hook_remove(&d->object_listener);
	        spa_hook_remove(&d->metadata_listener);
	}
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = global_unbind,
};

static int
global_bind(void *object, struct pw_impl_client *client, uint32_t permissions,
		  uint32_t version, uint32_t id)
{
	struct pw_impl_metadata *this = object;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto error_resource;

        data = pw_resource_get_user_data(resource);
        data->impl = this;
        data->resource = resource;

	pw_log_debug("%p: %u bound to %d", this, id, resource->id);
	pw_global_add_resource(global, resource);

	/* listen for when the resource goes away */
        pw_resource_add_listener(resource,
                        &data->resource_listener,
                        &resource_events, data);

	/* resource methods -> implementation */
	pw_resource_add_object_listener(resource,
			&data->object_listener,
                        &metadata_methods, data);

	/* implementation events -> resource */
	pw_metadata_add_listener(this->metadata,
			&data->metadata_listener,
			&metadata_resource_events, data);

	return 0;

error_resource:
	pw_log_error("%p: can't create metadata resource: %m", this);
	return -errno;
}

static void context_global_removed(void *data, struct pw_global *global)
{
	struct impl *impl = data;
	uint32_t id = pw_global_get_id(global);
	pw_log_trace("Clearing properties for global %u in %u",
				 id, pw_global_get_id(impl->this.global));
	impl_set_property(&impl->def, id, NULL, NULL, NULL);
}

static const struct pw_context_events context_events = {
	PW_VERSION_CONTEXT_EVENTS,
	.global_removed = context_global_removed,
};

static void global_destroy(void *data)
{
	struct pw_impl_metadata *metadata = data;
	spa_hook_remove(&metadata->global_listener);
	metadata->global = NULL;
	pw_impl_metadata_destroy(metadata);
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy,
};

SPA_EXPORT
int pw_impl_metadata_register(struct pw_impl_metadata *metadata,
			 struct pw_properties *properties)
{
	struct pw_context *context = metadata->context;
	static const char * const keys[] = {
		PW_KEY_OBJECT_SERIAL,
		PW_KEY_MODULE_ID,
		PW_KEY_FACTORY_ID,
		PW_KEY_METADATA_NAME,
		NULL
	};

	if (metadata->registered)
		goto error_existed;

        metadata->global = pw_global_new(context,
					PW_TYPE_INTERFACE_Metadata,
					PW_VERSION_METADATA,
					PW_METADATA_PERM_MASK,
					properties,
					global_bind,
					metadata);
	if (metadata->global == NULL)
		return -errno;

	spa_list_append(&context->metadata_list, &metadata->link);
	metadata->registered = true;

	pw_properties_setf(metadata->properties, PW_KEY_OBJECT_SERIAL, "%"PRIu64,
			pw_global_get_serial(metadata->global));

	pw_global_update_keys(metadata->global, &metadata->properties->dict, keys);

	pw_context_add_listener(context, &metadata->context_listener, &context_events,
							SPA_CONTAINER_OF(metadata, struct impl, this));
	pw_global_add_listener(metadata->global, &metadata->global_listener, &global_events, metadata);
	pw_global_register(metadata->global);

	return 0;

error_existed:
	pw_properties_free(properties);
	return -EEXIST;
}

SPA_EXPORT
void *pw_impl_metadata_get_user_data(struct pw_impl_metadata *metadata)
{
	return metadata->user_data;
}

SPA_EXPORT
struct pw_global *pw_impl_metadata_get_global(struct pw_impl_metadata *metadata)
{
	return metadata->global;
}

SPA_EXPORT
void pw_impl_metadata_add_listener(struct pw_impl_metadata *metadata,
			     struct spa_hook *listener,
			     const struct pw_impl_metadata_events *events,
			     void *data)
{
	spa_hook_list_append(&metadata->listener_list, listener, events, data);
}

SPA_EXPORT
int pw_impl_metadata_set_property(struct pw_impl_metadata *metadata,
			uint32_t subject, const char *key, const char *type,
			const char *value)
{
	return pw_metadata_set_property(metadata->metadata, subject, key, type, value);
}

SPA_EXPORT
int pw_impl_metadata_set_propertyf(struct pw_impl_metadata *metadata,
			uint32_t subject, const char *key, const char *type,
			const char *fmt, ...)
{
	spa_autofree char *value = NULL;
	va_list args;
	int res;

	va_start(args, fmt);
	res = vasprintf(&value, fmt, args);
	va_end(args);
	if (res < 0)
		return -errno;

	return pw_impl_metadata_set_property(metadata, subject, key, type, value);
}
