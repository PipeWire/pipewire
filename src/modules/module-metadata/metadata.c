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

#include <pipewire/pipewire.h>
#include "pipewire/private.h"

#include <extensions/metadata.h>

struct impl {
	struct pw_global *global;

	struct pw_metadata *metadata;
	struct pw_resource *resource;
	struct spa_hook resource_listener;
};

struct resource_data {
	struct impl *impl;

	struct pw_resource *resource;
	struct spa_hook resource_listener;
	struct spa_hook object_listener;
	struct spa_hook metadata_listener;
};

static int metadata_set_property(void *object,
			uint32_t subject,
			const char *key,
			const char *type,
			const char *value)
{
        struct resource_data *d = object;
	struct impl *impl = d->impl;
	pw_log_debug("%p", impl->metadata);
	pw_metadata_set_property(impl->metadata, subject, key, type, value);
	return 0;
}


static int metadata_clear(void *object)
{
        struct resource_data *d = object;
	struct impl *impl = d->impl;
	pw_log_debug("%p", impl->metadata);
	pw_metadata_clear(impl->metadata);
	return 0;
}

static const struct pw_metadata_methods metadata_methods = {
	PW_VERSION_METADATA_METHODS,
	.set_property = metadata_set_property,
	.clear = metadata_clear,
};

#define pw_metadata_resource(r,m,v,...)      \
	pw_resource_call_res(r,struct pw_metadata_events,m,v,__VA_ARGS__)

#define pw_metadata_resource_property(r,...)        \
        pw_metadata_resource(r,property,0,__VA_ARGS__)

static int metadata_property(void *object,
			uint32_t subject,
			const char *key,
			const char *type,
			const char *value)
{
        struct resource_data *d = object;
	pw_log_debug("%p", d->resource);
	pw_metadata_resource_property(d->resource, subject, key, type, value);
	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_property,
};

static void global_unbind(void *data)
{
        struct resource_data *d = data;
	if (d->resource)
	        spa_hook_remove(&d->metadata_listener);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = global_unbind,
};

static int
global_bind(void *_data, struct pw_client *client, uint32_t permissions,
            uint32_t version, uint32_t id)
{
	struct impl *impl = _data;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, PW_TYPE_INTERFACE_Metadata, version, sizeof(*data));
        if (resource == NULL)
                return -errno;

        data = pw_resource_get_user_data(resource);
        data->impl = impl;
        data->resource = resource;

	pw_log_debug(".");
//	pw_resource_install_marshal(resource, true);

	/* listen for when the resource goes away */
        pw_resource_add_listener(resource,
                        &data->resource_listener,
                        &resource_events, data);

	/* resource methods -> implemention */
	pw_log_debug(".");
	pw_resource_add_object_listener(resource,
			&data->object_listener,
                        &metadata_methods, data);
	/* implementation events -> resource */
	pw_log_debug(". %p", impl->metadata);
	pw_metadata_add_listener(impl->metadata,
			&data->metadata_listener,
			&metadata_events, data);
	pw_log_debug(".");

	return 0;
}

void *
pw_metadata_new(struct pw_core *core, struct pw_resource *resource,
		   struct pw_properties *properties)
{
	struct impl *impl;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		return NULL;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL) {
		pw_properties_free(properties);
		return NULL;
	}

	pw_properties_set(properties, PW_KEY_METADATA_NAME, "default");

	pw_resource_install_marshal(resource, true);

	impl->global = pw_global_new(core,
			PW_TYPE_INTERFACE_Metadata,
			PW_VERSION_METADATA,
			properties,
			global_bind, impl);
	if (impl->global == NULL) {
		free(impl);
		return NULL;
	}
	impl->resource = resource;
	impl->metadata = (struct pw_metadata*)resource;

	pw_global_register(impl->global);

	return impl;
}
