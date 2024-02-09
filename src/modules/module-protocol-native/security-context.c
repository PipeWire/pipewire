/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Wim Taymans */
/* SPDX-License-Identifier: MIT */


#include <pipewire/pipewire.h>
#include <pipewire/impl.h>
#include <pipewire/private.h>
#include <pipewire/extensions/security-context.h>

PW_LOG_TOPIC_EXTERN(mod_topic);
#define PW_LOG_TOPIC_DEFAULT mod_topic
PW_LOG_TOPIC_EXTERN(mod_topic_connection);

struct impl {
	struct pw_context *context;
	struct pw_global *global;

	struct pw_protocol *protocol;
};

struct resource_data {
	struct impl *impl;

	struct pw_resource *resource;
	struct spa_hook resource_listener;
	struct spa_hook object_listener;
};

static int security_context_create(void *object,
			int listen_fd,
			int close_fd,
			const struct spa_dict *props)
{
	struct resource_data *d = object;
	struct impl *impl = d->impl;
	struct pw_impl_client *client;
	const struct pw_properties *p;
	int res = 0;

	if ((client = impl->context->current_client) == NULL)
		goto invalid_state;
	if (client->protocol != impl->protocol)
		goto invalid_state;

	/* we can't make a nested security context */
	p = pw_impl_client_get_properties(client);
	if (pw_properties_get(p, PW_KEY_SEC_ENGINE) != NULL)
		goto not_allowed;

	if (pw_protocol_add_fd_server(impl->protocol, impl->context->core,
			listen_fd, close_fd, props) == NULL) {
		res = -errno;
		pw_resource_errorf(d->resource, res, "can't add fd server: %m");
	}
	return res;

invalid_state:
	pw_resource_errorf(d->resource, -EIO, "invalid client protocol");
	return -EIO;
not_allowed:
	pw_resource_errorf(d->resource, -EPERM, "Nested security context is not allowed");
	return -EPERM;
}

static const struct pw_security_context_methods security_context_methods = {
	PW_VERSION_SECURITY_CONTEXT_METHODS,
	.create = security_context_create,
};

static void global_unbind(void *data)
{
	struct resource_data *d = data;
	if (d->resource) {
	        spa_hook_remove(&d->resource_listener);
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
	struct impl *impl = object;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions,
			PW_TYPE_INTERFACE_SecurityContext,
			version, sizeof(*data));
	if (resource == NULL)
		return -errno;

	data = pw_resource_get_user_data(resource);
	data->impl = impl;
	data->resource = resource;

	pw_global_add_resource(impl->global, resource);

	/* listen for when the resource goes away */
	pw_resource_add_listener(resource,
			&data->resource_listener,
			&resource_events, data);

	/* resource methods -> implementation */
	pw_resource_add_object_listener(resource,
			&data->object_listener,
			&security_context_methods, data);

	return 0;
}

int protocol_native_security_context_init(struct pw_impl_module *module, struct pw_protocol *protocol)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	char serial_str[32];
	struct spa_dict_item items[1] = {
		SPA_DICT_ITEM_INIT(PW_KEY_OBJECT_SERIAL, serial_str),
	};
	struct spa_dict extra_props = SPA_DICT_INIT_ARRAY(items);
	static const char * const keys[] = {
		PW_KEY_OBJECT_SERIAL,
		NULL
	};

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->context = context;
	impl->protocol = protocol;

	impl->global = pw_global_new(context,
			PW_TYPE_INTERFACE_SecurityContext,
			PW_VERSION_SECURITY_CONTEXT,
			PW_SECURITY_CONTEXT_PERM_MASK,
			NULL,
			global_bind, impl);
	if (impl->global == NULL) {
		free(impl);
		return -errno;
	}
	spa_scnprintf(serial_str, sizeof(serial_str), "%"PRIu64,
			pw_global_get_serial(impl->global));
	pw_global_update_keys(impl->global, &extra_props, keys);

	pw_global_register(impl->global);

	return 0;
}
