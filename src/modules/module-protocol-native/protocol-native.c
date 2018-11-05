/* PipeWire
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <errno.h>

#include "spa/pod/parser.h"

#include "pipewire/pipewire.h"
#include "pipewire/protocol.h"
#include "pipewire/interfaces.h"
#include "pipewire/resource.h"
#include "extensions/protocol-native.h"

#include "connection.h"

static void core_marshal_hello(void *object, uint32_t version)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_HELLO);

	spa_pod_builder_add_struct(b, "i", version);

	pw_protocol_native_end_proxy(proxy, b);
}

static void core_marshal_client_update(void *object, const struct spa_dict *props)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	int i, n_items;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_CLIENT_UPDATE);

	n_items = props ? props->n_items : 0;

	spa_pod_builder_add(b, "[ i", n_items, NULL);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    "s", props->items[i].key,
				    "s", props->items[i].value, NULL);
	}
	spa_pod_builder_add(b, "]", NULL);

	pw_protocol_native_end_proxy(proxy, b);
}

static void core_marshal_permissions(void *object, uint32_t n_permissions,
		const struct pw_permission *permissions)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	int i;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_PERMISSIONS);

	spa_pod_builder_add(b, "[",
				"i", n_permissions,
				NULL);

	for (i = 0; i < n_permissions; i++) {
		spa_pod_builder_add(b,
				    "i", permissions[i].id,
				    "i", permissions[i].permissions, NULL);
	}
	spa_pod_builder_add(b, "]", NULL);

	pw_protocol_native_end_proxy(proxy, b);
}

static void core_marshal_sync(void *object, uint32_t seq)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_SYNC);

	spa_pod_builder_add_struct(b, "i", seq);

	pw_protocol_native_end_proxy(proxy, b);
}

static void core_marshal_get_registry(void *object, uint32_t version, uint32_t new_id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_GET_REGISTRY);

	spa_pod_builder_add_struct(b,
			       "i", version,
			       "i", new_id);

	pw_protocol_native_end_proxy(proxy, b);
}

static void
core_marshal_create_object(void *object,
			   const char *factory_name,
			   uint32_t type, uint32_t version,
			   const struct spa_dict *props, uint32_t new_id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_CREATE_OBJECT);

	n_items = props ? props->n_items : 0;

	spa_pod_builder_add(b,
			    "["
			    "s", factory_name,
			    "I", type,
			    "i", version,
			    "i", n_items, NULL);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    "s", props->items[i].key,
				    "s", props->items[i].value, NULL);
	}
	spa_pod_builder_add(b,
			    "i", new_id,
			    "]", NULL);

	pw_protocol_native_end_proxy(proxy, b);
}

static void
core_marshal_destroy(void *object, uint32_t id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_DESTROY);

	spa_pod_builder_add_struct(b, "i", id);

	pw_protocol_native_end_proxy(proxy, b);
}

static int core_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_dict props;
	struct pw_core_info info;
	struct spa_pod_parser prs;
	uint32_t i;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			 "i", &info.id,
			 "l", &info.change_mask,
			 "s", &info.user_name,
			 "s", &info.host_name,
			 "s", &info.version,
			 "s", &info.name,
			 "i", &info.cookie,
			 "i", &props.n_items, NULL) < 0)
		return -EINVAL;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				       "s", &props.items[i].key,
				       "s", &props.items[i].value,
				       NULL) < 0)
			return -EINVAL;
	}
	pw_proxy_notify(proxy, struct pw_core_proxy_events, info, 0, &info);
	return 0;
}

static int core_demarshal_done(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t seq;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs, "[ i", &seq, NULL) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_core_proxy_events, done, 0, seq);
	return 0;
}

static int core_demarshal_error(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t id, res;
	const char *error;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"[ i", &id,
			  "i", &res,
			  "s", &error, NULL) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_core_proxy_events, error, 0, id, res, error);
	return 0;
}

static int core_demarshal_remove_id(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t id;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs, "[ i", &id, NULL) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_core_proxy_events, remove_id, 0, id);
	return 0;
}

static void core_marshal_info(void *object, struct pw_core_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_resource(resource, PW_CORE_PROXY_EVENT_INFO);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(b,
			    "[",
			    "i", info->id,
			    "l", info->change_mask,
			    "s", info->user_name,
			    "s", info->host_name,
			    "s", info->version,
			    "s", info->name,
			    "i", info->cookie,
			    "i", n_items, NULL);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    "s", info->props->items[i].key,
				    "s", info->props->items[i].value, NULL);
	}
	spa_pod_builder_add(b, "]", NULL);

	pw_protocol_native_end_resource(resource, b);
}

static void core_marshal_done(void *object, uint32_t seq)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CORE_PROXY_EVENT_DONE);

	spa_pod_builder_add_struct(b, "i", seq);

	pw_protocol_native_end_resource(resource, b);
}

static void core_marshal_error(void *object, uint32_t id, int res, const char *error, ...)
{
	struct pw_resource *resource = object;
	char buffer[128];
	struct spa_pod_builder *b;
	va_list ap;

	b = pw_protocol_native_begin_resource(resource, PW_CORE_PROXY_EVENT_ERROR);

	va_start(ap, error);
	vsnprintf(buffer, sizeof(buffer), error, ap);
	va_end(ap);

	spa_pod_builder_add_struct(b,
			       "i", id,
			       "i", res,
			       "s", buffer);

	pw_protocol_native_end_resource(resource, b);
}

static void core_marshal_remove_id(void *object, uint32_t id)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CORE_PROXY_EVENT_REMOVE_ID);

	spa_pod_builder_add_struct(b, "i", id);

	pw_protocol_native_end_resource(resource, b);
}

static int core_demarshal_client_update(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_dict props;
	struct spa_pod_parser prs;
	uint32_t i;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs, "[ i", &props.n_items, NULL) < 0)
		return -EINVAL;

	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				"s", &props.items[i].key,
				"s", &props.items[i].value,
				NULL) < 0)
			return -EINVAL;
	}
	pw_resource_do(resource, struct pw_core_proxy_methods, client_update, 0, &props);
	return 0;
}

static int core_demarshal_permissions(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	struct pw_permission *permissions;
	uint32_t i, n_permissions;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs, "[",
				"i", &n_permissions, NULL) < 0)
		return -EINVAL;

	permissions = alloca(n_permissions * sizeof(struct pw_permission));
	for (i = 0; i < n_permissions; i++) {
		if (spa_pod_parser_get(&prs,
				"i", &permissions[i].id,
				"i", &permissions[i].permissions,
				NULL) < 0)
			return -EINVAL;
	}
	pw_resource_do(resource, struct pw_core_proxy_methods, permissions, 0,
			n_permissions, permissions);
	return 0;
}

static int core_demarshal_hello(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t version;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs, "[ i", &version,
					NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_core_proxy_methods, hello, 0, version);
	return 0;
}

static int core_demarshal_sync(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t seq;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs, "[i]", &seq, NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_core_proxy_methods, sync, 0, seq);
	return 0;
}

static int core_demarshal_get_registry(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	int32_t version, new_id;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs, "[ii]", &version, &new_id, NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_core_proxy_methods, get_registry, 0, version, new_id);
	return 0;
}

static int core_demarshal_create_object(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t version, type, new_id, i;
	const char *factory_name;
	struct spa_dict props;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"s", &factory_name,
			"I", &type,
			"i", &version,
			"i", &props.n_items, NULL) < 0)
		return -EINVAL;

	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs, "ss",
					&props.items[i].key, &props.items[i].value, NULL) < 0)
			return -EINVAL;
	}
	if (spa_pod_parser_get(&prs, "i", &new_id, NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_core_proxy_methods, create_object, 0, factory_name,
								      type, version,
								      &props, new_id);
	return 0;
}

static int core_demarshal_destroy(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs, "[i]", &id, NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_core_proxy_methods, destroy, 0, id);
	return 0;
}

static void registry_marshal_global(void *object, uint32_t id, uint32_t parent_id, uint32_t permissions,
				    uint32_t type, uint32_t version, const struct spa_dict *props)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_resource(resource, PW_REGISTRY_PROXY_EVENT_GLOBAL);

	n_items = props ? props->n_items : 0;

	spa_pod_builder_add(b,
			    "[",
			    "i", id,
			    "i", parent_id,
			    "i", permissions,
			    "I", type,
			    "i", version,
			    "i", n_items, NULL);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    "s", props->items[i].key,
				    "s", props->items[i].value, NULL);
	}
	spa_pod_builder_add(b, "]", NULL);

	pw_protocol_native_end_resource(resource, b);
}

static void registry_marshal_global_remove(void *object, uint32_t id)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_REGISTRY_PROXY_EVENT_GLOBAL_REMOVE);

	spa_pod_builder_add_struct(b, "i", id);

	pw_protocol_native_end_resource(resource, b);
}

static int registry_demarshal_bind(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id, version, type, new_id;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &id,
			"I", &type,
			"i", &version,
			"i", &new_id, NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_registry_proxy_methods, bind, 0, id, type, version, new_id);
	return 0;
}

static int registry_demarshal_destroy(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &id, NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_registry_proxy_methods, destroy, 0, id);
	return 0;
}

static void module_marshal_info(void *object, struct pw_module_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_resource(resource, PW_MODULE_PROXY_EVENT_INFO);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(b,
			    "[",
			    "i", info->id,
			    "l", info->change_mask,
			    "s", info->name,
			    "s", info->filename,
			    "s", info->args,
			    "i", n_items, NULL);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    "s", info->props->items[i].key,
				    "s", info->props->items[i].value, NULL);
	}
	spa_pod_builder_add(b, "]", NULL);

	pw_protocol_native_end_resource(resource, b);
}

static int module_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	struct spa_dict props;
	struct pw_module_info info;
	uint32_t i;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &info.id,
			"l", &info.change_mask,
			"s", &info.name,
			"s", &info.filename,
			"s", &info.args,
			"i", &props.n_items, NULL) < 0)
		return -EINVAL;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs, "ss",
					&props.items[i].key, &props.items[i].value, NULL) < 0)
			return -EINVAL;
	}
	pw_proxy_notify(proxy, struct pw_module_proxy_events, info, 0, &info);
	return 0;
}

static void factory_marshal_info(void *object, struct pw_factory_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_resource(resource, PW_FACTORY_PROXY_EVENT_INFO);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(b,
			    "[",
			    "i", info->id,
			    "l", info->change_mask,
			    "s", info->name,
			    "I", info->type,
			    "i", info->version,
			    "i", n_items, NULL);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    "s", info->props->items[i].key,
				    "s", info->props->items[i].value, NULL);
	}
	spa_pod_builder_add(b, "]", NULL);

	pw_protocol_native_end_resource(resource, b);
}

static int factory_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	struct spa_dict props;
	struct pw_factory_info info;
	uint32_t i;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			 "["
			"i", &info.id,
			"l", &info.change_mask,
			"s", &info.name,
			"I", &info.type,
			"i", &info.version,
			"i", &props.n_items, NULL) < 0)
		return -EINVAL;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				       "s", &props.items[i].key,
				       "s", &props.items[i].value, NULL) < 0)
			return -EINVAL;
	}
	pw_proxy_notify(proxy, struct pw_factory_proxy_events, info, 0, &info);
	return 0;
}

static void node_marshal_info(void *object, struct pw_node_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_resource(resource, PW_NODE_PROXY_EVENT_INFO);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(b,
			    "[",
			    "i", info->id,
			    "l", info->change_mask,
			    "s", info->name,
			    "i", info->max_input_ports,
			    "i", info->n_input_ports,
			    "i", info->max_output_ports,
			    "i", info->n_output_ports,
			    "i", info->state,
			    "s", info->error,
			    "i", n_items, NULL);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    "s", info->props->items[i].key,
				    "s", info->props->items[i].value, NULL);
	}
	spa_pod_builder_add(b, "]", NULL);

	pw_protocol_native_end_resource(resource, b);
}

static int node_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	struct spa_dict props;
	struct pw_node_info info;
	uint32_t i;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &info.id,
			"l", &info.change_mask,
			"s", &info.name,
			"i", &info.max_input_ports,
			"i", &info.n_input_ports,
			"i", &info.max_output_ports,
			"i", &info.n_output_ports,
			"i", &info.state,
			"s", &info.error,
			"i", &props.n_items, NULL) < 0)
		return -EINVAL;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				       "s", &props.items[i].key,
				       "s", &props.items[i].value, NULL) < 0)
			return -EINVAL;
	}
	pw_proxy_notify(proxy, struct pw_node_proxy_events, info, 0, &info);
	return 0;
}

static void node_marshal_param(void *object, uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_NODE_PROXY_EVENT_PARAM);

	spa_pod_builder_add_struct(b, "I", id, "i", index, "i", next, "P", param);

	pw_protocol_native_end_resource(resource, b);
}

static int node_demarshal_param(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t id, index, next;
	struct spa_pod *param;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
				"[ I", &id,
				"i", &index,
				"i", &next,
				"P", &param, NULL) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_node_proxy_events, param, 0, id, index, next, param);
	return 0;
}

static void node_marshal_enum_params(void *object, uint32_t id, uint32_t index, uint32_t num,
		const struct spa_pod *filter)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_NODE_PROXY_METHOD_ENUM_PARAMS);

	spa_pod_builder_add_struct(b,
			"I", id,
			"i", index,
			"i", num,
			"P", filter);

	pw_protocol_native_end_proxy(proxy, b);
}

static int node_demarshal_enum_params(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id, index, num;
	struct spa_pod *filter;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
				"[ I", &id,
				"i", &index,
				"i", &num,
				"P", &filter, NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_node_proxy_methods, enum_params, 0, id, index, num, filter);
	return 0;
}

static void node_marshal_set_param(void *object, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_NODE_PROXY_METHOD_SET_PARAM);

	spa_pod_builder_add_struct(b,
			"I", id,
			"i", flags,
			"P", param);
	pw_protocol_native_end_proxy(proxy, b);
}

static int node_demarshal_set_param(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id, flags;
	struct spa_pod *param;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
				"[ I", &id,
				"i", &flags,
				"P", &param, NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_node_proxy_methods, set_param, 0, id, flags, param);
	return 0;
}

static void node_marshal_send_command(void *object, const struct spa_command *command)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_NODE_PROXY_METHOD_SEND_COMMAND);
	spa_pod_builder_add_struct(b, "P", command);
	pw_protocol_native_end_proxy(proxy, b);
}

static int node_demarshal_send_command(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	struct spa_command *command;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
				"[ P", &command, NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_node_proxy_methods, send_command, 0, command);
	return 0;
}

static void port_marshal_info(void *object, struct pw_port_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_resource(resource, PW_PORT_PROXY_EVENT_INFO);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(b,
			    "[",
			    "i", info->id,
			    "i", info->direction,
			    "l", info->change_mask,
			    "i", n_items, NULL);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    "s", info->props->items[i].key,
				    "s", info->props->items[i].value, NULL);
	}
	spa_pod_builder_add(b, "]", NULL);

	pw_protocol_native_end_resource(resource, b);
}

static int port_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	struct spa_dict props;
	struct pw_port_info info;
	uint32_t i;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &info.id,
			"i", &info.direction,
			"l", &info.change_mask,
			"i", &props.n_items, NULL) < 0)
		return -EINVAL;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				       "s", &props.items[i].key,
				       "s", &props.items[i].value, NULL) < 0)
			return -EINVAL;
	}
	pw_proxy_notify(proxy, struct pw_port_proxy_events, info, 0, &info);
	return 0;
}

static void port_marshal_param(void *object, uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_PORT_PROXY_EVENT_PARAM);

	spa_pod_builder_add_struct(b, "I", id, "i", index, "i", next, "P", param);

	pw_protocol_native_end_resource(resource, b);
}

static int port_demarshal_param(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t id, index, next;
	struct spa_pod *param;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
				"[ I", &id,
				"i", &index,
				"i", &next,
				"P", &param, NULL) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_port_proxy_events, param, 0, id, index, next, param);
	return 0;
}

static void port_marshal_enum_params(void *object, uint32_t id, uint32_t index, uint32_t num,
		const struct spa_pod *filter)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_PORT_PROXY_METHOD_ENUM_PARAMS);

	spa_pod_builder_add_struct(b,
			"I", id,
			"i", index,
			"i", num,
			"P", filter);

	pw_protocol_native_end_proxy(proxy, b);
}

static int port_demarshal_enum_params(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id, index, num;
	struct spa_pod *filter;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
				"[ I", &id,
				"i", &index,
				"i", &num,
				"P", &filter, NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_port_proxy_methods, enum_params, 0, id, index, num, filter);
	return 0;
}

static void client_marshal_info(void *object, struct pw_client_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_PROXY_EVENT_INFO);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(b,
			    "[",
			    "i", info->id,
			    "l", info->change_mask,
			    "i", n_items, NULL);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    "s", info->props->items[i].key,
				    "s", info->props->items[i].value, NULL);
	}
	spa_pod_builder_add(b, "]", NULL);

	pw_protocol_native_end_resource(resource, b);
}

static int client_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	struct spa_dict props;
	struct pw_client_info info;
	uint32_t i;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &info.id,
			"l", &info.change_mask,
			"i", &props.n_items, NULL) < 0)
		return -EINVAL;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				       "s", &props.items[i].key,
				       "s", &props.items[i].value, NULL) < 0)
			return -EINVAL;
	}
	pw_proxy_notify(proxy, struct pw_client_proxy_events, info, 0, &info);
	return 0;
}

static void client_marshal_permissions(void *object, uint32_t index, uint32_t n_permissions,
		struct pw_permission *permissions)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	uint32_t i;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_PROXY_EVENT_PERMISSIONS);

	spa_pod_builder_add(b,
			    "[ i", index,
			    "i", n_permissions, NULL);

	for (i = 0; i < n_permissions; i++) {
		spa_pod_builder_add(b,
				    "s", permissions[i].id,
				    "s", permissions[i].permissions, NULL);
	}
	spa_pod_builder_add(b, "]", NULL);

	pw_protocol_native_end_resource(resource, b);
}

static int client_demarshal_permissions(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct pw_permission *permissions;
	struct spa_pod_parser prs;
	uint32_t i, index, n_permissions;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs, "[ i", &index,
				"i", &n_permissions, NULL) < 0)
		return -EINVAL;

	permissions = alloca(n_permissions * sizeof(struct pw_permission));
	for (i = 0; i < n_permissions; i++) {
		if (spa_pod_parser_get(&prs,
				"i", &permissions[i].id,
				"i", &permissions[i].permissions,
				NULL) < 0)
			return -EINVAL;
	}
	pw_proxy_notify(proxy, struct pw_client_proxy_events, permissions, 0, index, n_permissions, permissions);
	return 0;
}

static void client_marshal_error(void *object, uint32_t id, int res, const char *error)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_PROXY_METHOD_ERROR);
	spa_pod_builder_add_struct(b,
			       "i", id,
			       "i", res,
			       "s", error);
	pw_protocol_native_end_proxy(proxy, b);
}

static int client_demarshal_error(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id, res;
	const char *error;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"[ i", &id,
			  "i", &res,
			  "s", &error, NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_client_proxy_methods, error, 0, id, res, error);
	return 0;
}

static void client_marshal_get_permissions(void *object, uint32_t index, uint32_t num)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_PROXY_METHOD_GET_PERMISSIONS);

	spa_pod_builder_add_struct(b,
			"i", index,
			"i", num, NULL);

	pw_protocol_native_end_proxy(proxy, b);
}

static int client_demarshal_get_permissions(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t index, num;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
				"[i", &index,
				 "i", &num, NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_client_proxy_methods, get_permissions, 0, index, num);
	return 0;
}

static void client_marshal_update_permissions(void *object, uint32_t n_permissions,
		const struct pw_permission *permissions)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	int i;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_PROXY_METHOD_UPDATE_PERMISSIONS);

	spa_pod_builder_add(b, "[ i", n_permissions, NULL);

	for (i = 0; i < n_permissions; i++) {
		spa_pod_builder_add(b,
				    "i", permissions[i].id,
				    "i", permissions[i].permissions, NULL);
	}
	spa_pod_builder_add(b, "]", NULL);

	pw_protocol_native_end_proxy(proxy, b);
}

static int client_demarshal_update_permissions(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct pw_permission *permissions;
	struct spa_pod_parser prs;
	uint32_t i, n_permissions;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs, "[ i", &n_permissions, NULL) < 0)
		return -EINVAL;

	permissions = alloca(n_permissions * sizeof(struct pw_permission));
	for (i = 0; i < n_permissions; i++) {
		if (spa_pod_parser_get(&prs,
				"i", &permissions[i].id,
				"i", &permissions[i].permissions,
				NULL) < 0)
			return -EINVAL;
	}
	pw_resource_do(resource, struct pw_client_proxy_methods, update_permissions, 0,
			n_permissions, permissions);
	return 0;
}

static void link_marshal_info(void *object, struct pw_link_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_resource(resource, PW_LINK_PROXY_EVENT_INFO);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(b,
			    "[",
			    "i", info->id,
			    "l", info->change_mask,
			    "i", info->output_node_id,
			    "i", info->output_port_id,
			    "i", info->input_node_id,
			    "i", info->input_port_id,
			    "i", info->state,
			    "s", info->error,
			    "P", info->format,
			    "i", n_items, NULL);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    "s", info->props->items[i].key,
				    "s", info->props->items[i].value, NULL);
	}
	spa_pod_builder_add(b, "]", NULL);

	pw_protocol_native_end_resource(resource, b);
}

static int link_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	struct spa_dict props;
	struct pw_link_info info = { 0, };
	uint32_t i;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &info.id,
			"l", &info.change_mask,
			"i", &info.output_node_id,
			"i", &info.output_port_id,
			"i", &info.input_node_id,
			"i", &info.input_port_id,
			"i", &info.state,
			"s", &info.error,
			"P", &info.format,
			"i", &props.n_items, NULL) < 0)
		return -EINVAL;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				       "s", &props.items[i].key,
				       "s", &props.items[i].value, NULL) < 0)
			return -EINVAL;
	}
	pw_proxy_notify(proxy, struct pw_link_proxy_events, info, 0, &info);
	return 0;
}

static int registry_demarshal_global(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t id, parent_id, permissions, type, version, i;
	struct spa_dict props;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &id,
			"i", &parent_id,
			"i", &permissions,
			"I", &type,
			"i", &version,
			"i", &props.n_items, NULL) < 0)
		return -EINVAL;

	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				       "s", &props.items[i].key,
				       "s", &props.items[i].value, NULL) < 0)
			return -EINVAL;
	}

	pw_proxy_notify(proxy, struct pw_registry_proxy_events,
			global, 0, id, parent_id, permissions, type, version,
			props.n_items > 0 ? &props : NULL);
	return 0;
}

static int registry_demarshal_global_remove(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t id;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs, "[ i", &id, NULL) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_registry_proxy_events, global_remove, 0, id);
	return 0;
}

static void registry_marshal_bind(void *object, uint32_t id,
				  uint32_t type, uint32_t version, uint32_t new_id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_REGISTRY_PROXY_METHOD_BIND);

	spa_pod_builder_add_struct(b,
			       "i", id,
			       "I", type,
			       "i", version,
			       "i", new_id);

	pw_protocol_native_end_proxy(proxy, b);
}

static void registry_marshal_destroy(void *object, uint32_t id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_REGISTRY_PROXY_METHOD_DESTROY);
	spa_pod_builder_add_struct(b,
			       "i", id);
	pw_protocol_native_end_proxy(proxy, b);
}

static const struct pw_core_proxy_methods pw_protocol_native_core_method_marshal = {
	PW_VERSION_CORE_PROXY_METHODS,
	&core_marshal_hello,
	&core_marshal_sync,
	&core_marshal_get_registry,
	&core_marshal_client_update,
	&core_marshal_permissions,
	&core_marshal_create_object,
	&core_marshal_destroy,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_core_method_demarshal[PW_CORE_PROXY_METHOD_NUM] = {
	{ &core_demarshal_hello, 0, },
	{ &core_demarshal_sync, 0, },
	{ &core_demarshal_get_registry, 0, },
	{ &core_demarshal_client_update, 0, },
	{ &core_demarshal_permissions, 0, },
	{ &core_demarshal_create_object, 0, },
	{ &core_demarshal_destroy, 0, }
};

static const struct pw_core_proxy_events pw_protocol_native_core_event_marshal = {
	PW_VERSION_CORE_PROXY_EVENTS,
	&core_marshal_done,
	&core_marshal_error,
	&core_marshal_remove_id,
	&core_marshal_info
};

static const struct pw_protocol_native_demarshal pw_protocol_native_core_event_demarshal[PW_CORE_PROXY_EVENT_NUM] = {
	{ &core_demarshal_done, 0, },
	{ &core_demarshal_error, 0, },
	{ &core_demarshal_remove_id, 0, },
	{ &core_demarshal_info, 0, },
};

static const struct pw_protocol_marshal pw_protocol_native_core_marshal = {
	PW_TYPE_INTERFACE_Core,
	PW_VERSION_CORE,
	&pw_protocol_native_core_method_marshal,
	pw_protocol_native_core_method_demarshal,
	PW_CORE_PROXY_METHOD_NUM,
	&pw_protocol_native_core_event_marshal,
	pw_protocol_native_core_event_demarshal,
	PW_CORE_PROXY_EVENT_NUM
};

static const struct pw_registry_proxy_methods pw_protocol_native_registry_method_marshal = {
	PW_VERSION_REGISTRY_PROXY_METHODS,
	&registry_marshal_bind,
	&registry_marshal_destroy,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_registry_method_demarshal[] = {
	{ &registry_demarshal_bind, 0, },
	{ &registry_demarshal_destroy, 0, },
};

static const struct pw_registry_proxy_events pw_protocol_native_registry_event_marshal = {
	PW_VERSION_REGISTRY_PROXY_EVENTS,
	&registry_marshal_global,
	&registry_marshal_global_remove,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_registry_event_demarshal[] = {
	{ &registry_demarshal_global, 0, },
	{ &registry_demarshal_global_remove, 0, }
};

const struct pw_protocol_marshal pw_protocol_native_registry_marshal = {
	PW_TYPE_INTERFACE_Registry,
	PW_VERSION_REGISTRY,
	&pw_protocol_native_registry_method_marshal,
	pw_protocol_native_registry_method_demarshal,
	PW_REGISTRY_PROXY_METHOD_NUM,
	&pw_protocol_native_registry_event_marshal,
	pw_protocol_native_registry_event_demarshal,
	PW_REGISTRY_PROXY_EVENT_NUM,
};

static const struct pw_module_proxy_events pw_protocol_native_module_event_marshal = {
	PW_VERSION_MODULE_PROXY_EVENTS,
	&module_marshal_info,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_module_event_demarshal[] = {
	{ &module_demarshal_info, 0, },
};

const struct pw_protocol_marshal pw_protocol_native_module_marshal = {
	PW_TYPE_INTERFACE_Module,
	PW_VERSION_MODULE,
	NULL, NULL, 0,
	&pw_protocol_native_module_event_marshal,
	pw_protocol_native_module_event_demarshal,
	PW_MODULE_PROXY_EVENT_NUM,
};

static const struct pw_factory_proxy_events pw_protocol_native_factory_event_marshal = {
	PW_VERSION_FACTORY_PROXY_EVENTS,
	&factory_marshal_info,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_factory_event_demarshal[] = {
	{ &factory_demarshal_info, 0, },
};

const struct pw_protocol_marshal pw_protocol_native_factory_marshal = {
	PW_TYPE_INTERFACE_Factory,
	PW_VERSION_FACTORY,
	NULL, NULL, 0,
	&pw_protocol_native_factory_event_marshal,
	pw_protocol_native_factory_event_demarshal,
	PW_FACTORY_PROXY_EVENT_NUM,
};

static const struct pw_node_proxy_methods pw_protocol_native_node_method_marshal = {
	PW_VERSION_NODE_PROXY_METHODS,
	&node_marshal_enum_params,
	&node_marshal_set_param,
	&node_marshal_send_command,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_node_method_demarshal[] = {
	{ &node_demarshal_enum_params, 0, },
	{ &node_demarshal_set_param, PW_PROTOCOL_NATIVE_PERM_W, },
	{ &node_demarshal_send_command, PW_PROTOCOL_NATIVE_PERM_W, },
};

static const struct pw_node_proxy_events pw_protocol_native_node_event_marshal = {
	PW_VERSION_NODE_PROXY_EVENTS,
	&node_marshal_info,
	&node_marshal_param,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_node_event_demarshal[] = {
	{ &node_demarshal_info, 0, },
	{ &node_demarshal_param, 0, }
};

static const struct pw_protocol_marshal pw_protocol_native_node_marshal = {
	PW_TYPE_INTERFACE_Node,
	PW_VERSION_NODE,
	&pw_protocol_native_node_method_marshal,
	pw_protocol_native_node_method_demarshal,
	PW_NODE_PROXY_METHOD_NUM,
	&pw_protocol_native_node_event_marshal,
	pw_protocol_native_node_event_demarshal,
	PW_NODE_PROXY_EVENT_NUM,
};


static const struct pw_port_proxy_methods pw_protocol_native_port_method_marshal = {
	PW_VERSION_PORT_PROXY_METHODS,
	&port_marshal_enum_params,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_port_method_demarshal[] = {
	{ &port_demarshal_enum_params, 0, },
};

static const struct pw_port_proxy_events pw_protocol_native_port_event_marshal = {
	PW_VERSION_PORT_PROXY_EVENTS,
	&port_marshal_info,
	&port_marshal_param,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_port_event_demarshal[] = {
	{ &port_demarshal_info, 0, },
	{ &port_demarshal_param, 0, }
};

static const struct pw_protocol_marshal pw_protocol_native_port_marshal = {
	PW_TYPE_INTERFACE_Port,
	PW_VERSION_PORT,
	&pw_protocol_native_port_method_marshal,
	pw_protocol_native_port_method_demarshal,
	PW_PORT_PROXY_METHOD_NUM,
	&pw_protocol_native_port_event_marshal,
	pw_protocol_native_port_event_demarshal,
	PW_PORT_PROXY_EVENT_NUM,
};

static const struct pw_client_proxy_methods pw_protocol_native_client_method_marshal = {
	PW_VERSION_CLIENT_PROXY_METHODS,
	&client_marshal_error,
	&client_marshal_get_permissions,
	&client_marshal_update_permissions,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_client_method_demarshal[] = {
	{ &client_demarshal_error, PW_PROTOCOL_NATIVE_PERM_W, },
	{ &client_demarshal_get_permissions, 0, },
	{ &client_demarshal_update_permissions, PW_PROTOCOL_NATIVE_PERM_W, },
};

static const struct pw_client_proxy_events pw_protocol_native_client_event_marshal = {
	PW_VERSION_CLIENT_PROXY_EVENTS,
	&client_marshal_info,
	&client_marshal_permissions,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_client_event_demarshal[] = {
	{ &client_demarshal_info, 0, },
	{ &client_demarshal_permissions, 0, }
};

static const struct pw_protocol_marshal pw_protocol_native_client_marshal = {
	PW_TYPE_INTERFACE_Client,
	PW_VERSION_CLIENT,
	&pw_protocol_native_client_method_marshal,
	pw_protocol_native_client_method_demarshal,
	PW_CLIENT_PROXY_METHOD_NUM,
	&pw_protocol_native_client_event_marshal,
	pw_protocol_native_client_event_demarshal,
	PW_CLIENT_PROXY_EVENT_NUM,
};

static const struct pw_link_proxy_events pw_protocol_native_link_event_marshal = {
	PW_VERSION_LINK_PROXY_EVENTS,
	&link_marshal_info,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_link_event_demarshal[] = {
	{ &link_demarshal_info, 0, }
};

static const struct pw_protocol_marshal pw_protocol_native_link_marshal = {
	PW_TYPE_INTERFACE_Link,
	PW_VERSION_LINK,
	NULL, NULL, 0,
	&pw_protocol_native_link_event_marshal,
	pw_protocol_native_link_event_demarshal,
	PW_LINK_PROXY_EVENT_NUM,
};

void pw_protocol_native_init(struct pw_protocol *protocol)
{
	pw_protocol_add_marshal(protocol, &pw_protocol_native_core_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_registry_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_module_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_node_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_port_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_factory_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_client_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_link_marshal);
}
