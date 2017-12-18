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

static void core_marshal_permissions(void *object, const struct spa_dict *props)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	int i, n_items;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_PERMISSIONS);

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

static void core_marshal_sync(void *object, uint32_t seq)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_SYNC);

	spa_pod_builder_struct(b, "i", seq);

	pw_protocol_native_end_proxy(proxy, b);
}

static void core_marshal_get_registry(void *object, uint32_t version, uint32_t new_id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_GET_REGISTRY);

	spa_pod_builder_struct(b,
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
core_marshal_create_link(void *object,
			 uint32_t output_node_id,
			 uint32_t output_port_id,
			 uint32_t input_node_id,
			 uint32_t input_port_id,
			 const struct spa_pod *filter,
			 const struct spa_dict *props,
			 uint32_t new_id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_CREATE_LINK);

	n_items = props ? props->n_items : 0;

	spa_pod_builder_add(b,
			    "["
			    "i", output_node_id,
			    "i", output_port_id,
			    "i", input_node_id,
			    "i", input_port_id,
			    "P", filter,
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
core_marshal_update_types_client(void *object, uint32_t first_id, const char **types, uint32_t n_types)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	uint32_t i;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_UPDATE_TYPES);

	spa_pod_builder_add(b,
			    "["
			    " i", first_id,
			    " i", n_types, NULL);

	for (i = 0; i < n_types; i++) {
		spa_pod_builder_add(b, "s", types[i], NULL);
	}
	spa_pod_builder_add(b, "]", NULL);

	pw_protocol_native_end_proxy(proxy, b);
}

static int core_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_dict props;
	struct pw_core_info info;
	struct spa_pod_parser prs;
	int i;

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
	pw_proxy_notify(proxy, struct pw_core_proxy_events, info, &info);
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

	pw_proxy_notify(proxy, struct pw_core_proxy_events, done, seq);
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

	pw_proxy_notify(proxy, struct pw_core_proxy_events, error, id, res, error);
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

	pw_proxy_notify(proxy, struct pw_core_proxy_events, remove_id, id);
	return 0;
}

static int core_demarshal_update_types_client(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t first_id, n_types;
	const char **types;
	int i;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
				"["
				" i", &first_id,
				" i", &n_types, NULL) < 0)
		return -EINVAL;

	types = alloca(n_types * sizeof(char *));
	for (i = 0; i < n_types; i++) {
		if (spa_pod_parser_get(&prs, "s", &types[i], NULL) < 0)
			return -EINVAL;
	}
	pw_proxy_notify(proxy, struct pw_core_proxy_events, update_types, first_id, types, n_types);
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

	spa_pod_builder_struct(b, "i", seq);

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

	spa_pod_builder_struct(b,
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

	spa_pod_builder_struct(b, "i", id);

	pw_protocol_native_end_resource(resource, b);
}

static void
core_marshal_update_types_server(void *object, uint32_t first_id, const char **types, uint32_t n_types)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	uint32_t i;

	b = pw_protocol_native_begin_resource(resource, PW_CORE_PROXY_EVENT_UPDATE_TYPES);

	spa_pod_builder_add(b,
			    "[",
			    "i", first_id,
			    "i", n_types, NULL);

	for (i = 0; i < n_types; i++) {
		spa_pod_builder_add(b, "s", types[i], NULL);
	}
	spa_pod_builder_add(b, "]", NULL);

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
	pw_resource_do(resource, struct pw_core_proxy_methods, client_update, &props);
	return 0;
}

static int core_demarshal_permissions(void *object, void *data, size_t size)
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
	pw_resource_do(resource, struct pw_core_proxy_methods, permissions, &props);
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

	pw_resource_do(resource, struct pw_core_proxy_methods, sync, seq);
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

	pw_resource_do(resource, struct pw_core_proxy_methods, get_registry, version, new_id);
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

	pw_resource_do(resource, struct pw_core_proxy_methods, create_object, factory_name,
								      type, version,
								      &props, new_id);
	return 0;
}

static int core_demarshal_create_link(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t new_id, i;
	uint32_t output_node_id, output_port_id, input_node_id, input_port_id;
	struct spa_pod *filter = NULL;
	struct spa_dict props;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &output_node_id,
			"i", &output_port_id,
			"i", &input_node_id,
			"i", &input_port_id,
			"P", &filter,
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

	pw_resource_do(resource, struct pw_core_proxy_methods, create_link, output_node_id,
								      output_port_id,
								      input_node_id,
								      input_port_id,
								      filter,
								      &props,
								      new_id);
	return 0;
}

static int core_demarshal_update_types_server(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t first_id, n_types;
	const char **types;
	int i;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &first_id,
			"i", &n_types, NULL) < 0)
		return -EINVAL;

	types = alloca(n_types * sizeof(char *));
	for (i = 0; i < n_types; i++) {
		if (spa_pod_parser_get(&prs, "s", &types[i], NULL) < 0)
			return -EINVAL;
	}
	pw_resource_do(resource, struct pw_core_proxy_methods, update_types, first_id, types, n_types);
	return 0;
}

static void registry_marshal_global(void *object, uint32_t id, uint32_t parent_id, uint32_t permissions,
				    uint32_t type, uint32_t version)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_REGISTRY_PROXY_EVENT_GLOBAL);

	spa_pod_builder_struct(b,
			       "i", id,
			       "i", parent_id,
			       "i", permissions,
			       "I", type,
			       "i", version);

	pw_protocol_native_end_resource(resource, b);
}

static void registry_marshal_global_remove(void *object, uint32_t id)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_REGISTRY_PROXY_EVENT_GLOBAL_REMOVE);

	spa_pod_builder_struct(b, "i", id);

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

	pw_resource_do(resource, struct pw_registry_proxy_methods, bind, id, type, version, new_id);
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
	int i;

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
	pw_proxy_notify(proxy, struct pw_module_proxy_events, info, &info);
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
	int i;

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
	pw_proxy_notify(proxy, struct pw_factory_proxy_events, info, &info);
	return 0;
}

static void node_marshal_info(void *object, struct pw_node_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_resource(resource, PW_NODE_PROXY_EVENT_INFO);

	spa_pod_builder_add(b,
			    "[",
			    "i", info->id,
			    "l", info->change_mask,
			    "s", info->name,
			    "i", info->max_input_ports,
			    "i", info->n_input_ports,
			    "i", info->n_input_params, NULL);

	for (i = 0; i < info->n_input_params; i++)
		spa_pod_builder_add(b, "P", info->input_params[i], NULL);

	spa_pod_builder_add(b,
			    "i", info->max_output_ports,
			    "i", info->n_output_ports,
			    "i", info->n_output_params, 0);

	for (i = 0; i < info->n_output_params; i++)
		spa_pod_builder_add(b, "P", info->output_params[i], NULL);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(b,
			    "i", info->state,
			    "s", info->error, "i", n_items, NULL);

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
	int i;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &info.id,
			"l", &info.change_mask,
			"s", &info.name,
			"i", &info.max_input_ports,
			"i", &info.n_input_ports,
			"i", &info.n_input_params, NULL) < 0)
		return -EINVAL;

	info.input_params = alloca(info.n_input_params * sizeof(struct spa_pod *));
	for (i = 0; i < info.n_input_params; i++)
		if (spa_pod_parser_get(&prs, "P", &info.input_params[i], NULL) < 0)
			return -EINVAL;

	if (spa_pod_parser_get(&prs,
			      "i", &info.max_output_ports,
			      "i", &info.n_output_ports,
			      "i", &info.n_output_params, NULL) < 0)
		return -EINVAL;

	info.output_params = alloca(info.n_output_params * sizeof(struct spa_pod *));
	for (i = 0; i < info.n_output_params; i++)
		if (spa_pod_parser_get(&prs, "P", &info.output_params[i], NULL) < 0)
			return -EINVAL;

	if (spa_pod_parser_get(&prs,
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
	pw_proxy_notify(proxy, struct pw_node_proxy_events, info, &info);
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
	pw_proxy_notify(proxy, struct pw_client_proxy_events, info, &info);
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
	int i;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &info.id,
			"l", &info.change_mask,
			"i", &info.output_node_id,
			"i", &info.output_port_id,
			"i", &info.input_node_id,
			"i", &info.input_port_id,
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
	pw_proxy_notify(proxy, struct pw_link_proxy_events, info, &info);
	return 0;
}

static int registry_demarshal_global(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t id, parent_id, permissions, type, version;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &id,
			"i", &parent_id,
			"i", &permissions,
			"I", &type,
			"i", &version, NULL) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_registry_proxy_events, global, id, parent_id, permissions, type, version);
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

	pw_proxy_notify(proxy, struct pw_registry_proxy_events, global_remove, id);
	return 0;
}

static void registry_marshal_bind(void *object, uint32_t id,
				  uint32_t type, uint32_t version, uint32_t new_id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_REGISTRY_PROXY_METHOD_BIND);

	spa_pod_builder_struct(b,
			       "i", id,
			       "I", type,
			       "i", version,
			       "i", new_id);

	pw_protocol_native_end_proxy(proxy, b);
}

static const struct pw_core_proxy_methods pw_protocol_native_core_method_marshal = {
	PW_VERSION_CORE_PROXY_METHODS,
	&core_marshal_update_types_client,
	&core_marshal_sync,
	&core_marshal_get_registry,
	&core_marshal_client_update,
	&core_marshal_permissions,
	&core_marshal_create_object,
	&core_marshal_create_link
};

static const struct pw_protocol_native_demarshal pw_protocol_native_core_method_demarshal[PW_CORE_PROXY_METHOD_NUM] = {
	{ &core_demarshal_update_types_server, 0, },
	{ &core_demarshal_sync, 0, },
	{ &core_demarshal_get_registry, 0, },
	{ &core_demarshal_client_update, 0, },
	{ &core_demarshal_permissions, 0, },
	{ &core_demarshal_create_object, PW_PROTOCOL_NATIVE_REMAP, },
	{ &core_demarshal_create_link, PW_PROTOCOL_NATIVE_REMAP, }
};

static const struct pw_core_proxy_events pw_protocol_native_core_event_marshal = {
	PW_VERSION_CORE_PROXY_EVENTS,
	&core_marshal_update_types_server,
	&core_marshal_done,
	&core_marshal_error,
	&core_marshal_remove_id,
	&core_marshal_info
};

static const struct pw_protocol_native_demarshal pw_protocol_native_core_event_demarshal[PW_CORE_PROXY_EVENT_NUM] = {
	{ &core_demarshal_update_types_client, 0, },
	{ &core_demarshal_done, 0, },
	{ &core_demarshal_error, 0, },
	{ &core_demarshal_remove_id, 0, },
	{ &core_demarshal_info, 0, },
};

static const struct pw_protocol_marshal pw_protocol_native_core_marshal = {
	PW_TYPE_INTERFACE__Core,
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
};

static const struct pw_protocol_native_demarshal pw_protocol_native_registry_method_demarshal[] = {
	{ &registry_demarshal_bind, PW_PROTOCOL_NATIVE_REMAP, },
};

static const struct pw_registry_proxy_events pw_protocol_native_registry_event_marshal = {
	PW_VERSION_REGISTRY_PROXY_EVENTS,
	&registry_marshal_global,
	&registry_marshal_global_remove,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_registry_event_demarshal[] = {
	{ &registry_demarshal_global, PW_PROTOCOL_NATIVE_REMAP, },
	{ &registry_demarshal_global_remove, 0, }
};

const struct pw_protocol_marshal pw_protocol_native_registry_marshal = {
	PW_TYPE_INTERFACE__Registry,
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
	PW_TYPE_INTERFACE__Module,
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
	{ &factory_demarshal_info, PW_PROTOCOL_NATIVE_REMAP, },
};

const struct pw_protocol_marshal pw_protocol_native_factory_marshal = {
	PW_TYPE_INTERFACE__Factory,
	PW_VERSION_FACTORY,
	NULL, NULL, 0,
	&pw_protocol_native_factory_event_marshal,
	pw_protocol_native_factory_event_demarshal,
	PW_FACTORY_PROXY_EVENT_NUM,
};

static const struct pw_node_proxy_events pw_protocol_native_node_event_marshal = {
	PW_VERSION_NODE_PROXY_EVENTS,
	&node_marshal_info,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_node_event_demarshal[] = {
	{ &node_demarshal_info, PW_PROTOCOL_NATIVE_REMAP, }
};

static const struct pw_protocol_marshal pw_protocol_native_node_marshal = {
	PW_TYPE_INTERFACE__Node,
	PW_VERSION_NODE,
	NULL, NULL, 0,
	&pw_protocol_native_node_event_marshal,
	pw_protocol_native_node_event_demarshal,
	PW_NODE_PROXY_EVENT_NUM,
};

static const struct pw_client_proxy_events pw_protocol_native_client_event_marshal = {
	PW_VERSION_CLIENT_PROXY_EVENTS,
	&client_marshal_info,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_client_event_demarshal[] = {
	{ &client_demarshal_info, 0, },
};

static const struct pw_protocol_marshal pw_protocol_native_client_marshal = {
	PW_TYPE_INTERFACE__Client,
	PW_VERSION_CLIENT,
	NULL, NULL, 0,
	&pw_protocol_native_client_event_marshal,
	pw_protocol_native_client_event_demarshal,
	PW_CLIENT_PROXY_EVENT_NUM,
};

static const struct pw_link_proxy_events pw_protocol_native_link_event_marshal = {
	PW_VERSION_LINK_PROXY_EVENTS,
	&link_marshal_info,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_link_event_demarshal[] = {
	{ &link_demarshal_info, PW_PROTOCOL_NATIVE_REMAP, }
};

static const struct pw_protocol_marshal pw_protocol_native_link_marshal = {
	PW_TYPE_INTERFACE__Link,
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
	pw_protocol_add_marshal(protocol, &pw_protocol_native_factory_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_client_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_link_marshal);
}
