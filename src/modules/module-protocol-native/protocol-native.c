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

#include "spa/pod-iter.h"

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
	struct spa_pod_frame f;
	int i, n_items;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_CLIENT_UPDATE);

	n_items = props ? props->n_items : 0;

	spa_pod_builder_add(b, SPA_POD_TYPE_STRUCT, &f, SPA_POD_TYPE_INT, n_items, 0);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    SPA_POD_TYPE_STRING, props->items[i].key,
				    SPA_POD_TYPE_STRING, props->items[i].value, 0);
	}
	spa_pod_builder_add(b, -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_protocol_native_end_proxy(proxy, b);
}

static void core_marshal_sync(void *object, uint32_t seq)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_SYNC);

	spa_pod_builder_struct(b, &f, SPA_POD_TYPE_INT, seq);

	pw_protocol_native_end_proxy(proxy, b);
}

static void core_marshal_get_registry(void *object, uint32_t version, uint32_t new_id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_GET_REGISTRY);

	spa_pod_builder_struct(b, &f,
			       SPA_POD_TYPE_INT, version,
			       SPA_POD_TYPE_INT, new_id);

	pw_protocol_native_end_proxy(proxy, b);
}

static void
core_marshal_create_node(void *object,
			 const char *factory_name, const char *name,
			 uint32_t type, uint32_t version,
			 const struct spa_dict *props, uint32_t new_id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_CREATE_NODE);

	n_items = props ? props->n_items : 0;

	spa_pod_builder_add(b,
			    SPA_POD_TYPE_STRUCT, &f,
			    SPA_POD_TYPE_STRING, factory_name,
			    SPA_POD_TYPE_STRING, name,
			    SPA_POD_TYPE_ID, type,
			    SPA_POD_TYPE_INT, version,
			    SPA_POD_TYPE_INT, n_items, 0);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    SPA_POD_TYPE_STRING, props->items[i].key,
				    SPA_POD_TYPE_STRING, props->items[i].value, 0);
	}
	spa_pod_builder_add(b,
			    SPA_POD_TYPE_INT, new_id,
			    -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_protocol_native_end_proxy(proxy, b);
}

static void
core_marshal_create_link(void *object,
			 uint32_t output_node_id,
			 uint32_t output_port_id,
			 uint32_t input_node_id,
			 uint32_t input_port_id,
			 const struct spa_format *filter,
			 const struct spa_dict *props,
			 uint32_t new_id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_CREATE_LINK);

	n_items = props ? props->n_items : 0;

	spa_pod_builder_add(b,
			    SPA_POD_TYPE_STRUCT, &f,
			    SPA_POD_TYPE_INT, output_node_id,
			    SPA_POD_TYPE_INT, output_port_id,
			    SPA_POD_TYPE_INT, input_node_id,
			    SPA_POD_TYPE_INT, input_port_id,
			    SPA_POD_TYPE_POD, filter,
			    SPA_POD_TYPE_INT, n_items, 0);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    SPA_POD_TYPE_STRING, props->items[i].key,
				    SPA_POD_TYPE_STRING, props->items[i].value, 0);
	}
	spa_pod_builder_add(b,
			    SPA_POD_TYPE_INT, new_id,
			    -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_protocol_native_end_proxy(proxy, b);
}

static void
core_marshal_update_types_client(void *object, uint32_t first_id, uint32_t n_types, const char **types)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;
	uint32_t i;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_UPDATE_TYPES);

	spa_pod_builder_add(b,
			    SPA_POD_TYPE_STRUCT, &f,
			    SPA_POD_TYPE_INT, first_id, SPA_POD_TYPE_INT, n_types, 0);

	for (i = 0; i < n_types; i++) {
		spa_pod_builder_add(b, SPA_POD_TYPE_STRING, types[i], 0);
	}
	spa_pod_builder_add(b, -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_protocol_native_end_proxy(proxy, b);
}

static bool core_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_dict props;
	struct pw_core_info info;
	struct spa_pod_iter it;
	int i;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_LONG, &info.change_mask,
			      SPA_POD_TYPE_STRING, &info.user_name,
			      SPA_POD_TYPE_STRING, &info.host_name,
			      SPA_POD_TYPE_STRING, &info.version,
			      SPA_POD_TYPE_STRING, &info.name,
			      SPA_POD_TYPE_INT, &info.cookie, SPA_POD_TYPE_INT, &props.n_items, 0))
		return false;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (!spa_pod_iter_get(&it,
				      SPA_POD_TYPE_STRING, &props.items[i].key,
				      SPA_POD_TYPE_STRING, &props.items[i].value, 0))
			return false;
	}
	pw_proxy_notify(proxy, struct pw_core_proxy_events, info, &info);
	return true;
}

static bool core_demarshal_done(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	uint32_t seq;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it, SPA_POD_TYPE_INT, &seq, 0))
		return false;

	pw_proxy_notify(proxy, struct pw_core_proxy_events, done, seq);
	return true;
}

static bool core_demarshal_error(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	uint32_t id, res;
	const char *error;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &id,
			      SPA_POD_TYPE_INT, &res, SPA_POD_TYPE_STRING, &error, 0))
		return false;

	pw_proxy_notify(proxy, struct pw_core_proxy_events, error, id, res, error);
	return true;
}

static bool core_demarshal_remove_id(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	uint32_t id;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it, SPA_POD_TYPE_INT, &id, 0))
		return false;

	pw_proxy_notify(proxy, struct pw_core_proxy_events, remove_id, id);
	return true;
}

static bool core_demarshal_update_types_client(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	uint32_t first_id, n_types;
	const char **types;
	int i;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it, SPA_POD_TYPE_INT, &first_id, SPA_POD_TYPE_INT, &n_types, 0))
		return false;

	types = alloca(n_types * sizeof(char *));
	for (i = 0; i < n_types; i++) {
		if (!spa_pod_iter_get(&it, SPA_POD_TYPE_STRING, &types[i], 0))
			return false;
	}
	pw_proxy_notify(proxy, struct pw_core_proxy_events, update_types, first_id, n_types, types);
	return true;
}

static void core_marshal_info(void *object, struct pw_core_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_resource(resource, PW_CORE_PROXY_EVENT_INFO);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(b,
			    SPA_POD_TYPE_STRUCT, &f,
			    SPA_POD_TYPE_LONG, info->change_mask,
			    SPA_POD_TYPE_STRING, info->user_name,
			    SPA_POD_TYPE_STRING, info->host_name,
			    SPA_POD_TYPE_STRING, info->version,
			    SPA_POD_TYPE_STRING, info->name,
			    SPA_POD_TYPE_INT, info->cookie,
			    SPA_POD_TYPE_INT, n_items, 0);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    SPA_POD_TYPE_STRING, info->props->items[i].key,
				    SPA_POD_TYPE_STRING, info->props->items[i].value, 0);
	}
	spa_pod_builder_add(b, -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_protocol_native_end_resource(resource, b);
}

static void core_marshal_done(void *object, uint32_t seq)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_CORE_PROXY_EVENT_DONE);

	spa_pod_builder_struct(b, &f, SPA_POD_TYPE_INT, seq);

	pw_protocol_native_end_resource(resource, b);
}

static void core_marshal_error(void *object, uint32_t id, int res, const char *error, ...)
{
	struct pw_resource *resource = object;
	char buffer[128];
	struct spa_pod_builder *b;
	struct spa_pod_frame f;
	va_list ap;

	b = pw_protocol_native_begin_resource(resource, PW_CORE_PROXY_EVENT_ERROR);

	va_start(ap, error);
	vsnprintf(buffer, sizeof(buffer), error, ap);
	va_end(ap);

	spa_pod_builder_struct(b, &f,
			       SPA_POD_TYPE_INT, id,
			       SPA_POD_TYPE_INT, res,
			       SPA_POD_TYPE_STRING, buffer);

	pw_protocol_native_end_resource(resource, b);
}

static void core_marshal_remove_id(void *object, uint32_t id)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_CORE_PROXY_EVENT_REMOVE_ID);

	spa_pod_builder_struct(b, &f, SPA_POD_TYPE_INT, id);

	pw_protocol_native_end_resource(resource, b);
}

static void
core_marshal_update_types_server(void *object, uint32_t first_id, uint32_t n_types, const char **types)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;
	uint32_t i;

	b = pw_protocol_native_begin_resource(resource, PW_CORE_PROXY_EVENT_UPDATE_TYPES);

	spa_pod_builder_add(b,
			    SPA_POD_TYPE_STRUCT, &f,
			    SPA_POD_TYPE_INT, first_id, SPA_POD_TYPE_INT, n_types, 0);

	for (i = 0; i < n_types; i++) {
		spa_pod_builder_add(b, SPA_POD_TYPE_STRING, types[i], 0);
	}
	spa_pod_builder_add(b, -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_protocol_native_end_resource(resource, b);
}

static bool core_demarshal_client_update(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_dict props;
	struct spa_pod_iter it;
	uint32_t i;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it, SPA_POD_TYPE_INT, &props.n_items, 0))
		return false;

	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (!spa_pod_iter_get(&it,
				      SPA_POD_TYPE_STRING, &props.items[i].key,
				      SPA_POD_TYPE_STRING, &props.items[i].value, 0))
			return false;
	}
	pw_resource_do(resource, struct pw_core_proxy_methods, client_update, &props);
	return true;
}

static bool core_demarshal_sync(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_iter it;
	uint32_t seq;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it, SPA_POD_TYPE_INT, &seq, 0))
		return false;

	pw_resource_do(resource, struct pw_core_proxy_methods, sync, seq);
	return true;
}

static bool core_demarshal_get_registry(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_iter it;
	int32_t version, new_id;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &version,
			      SPA_POD_TYPE_INT, &new_id, 0))

		return false;

	pw_resource_do(resource, struct pw_core_proxy_methods, get_registry, version, new_id);
	return true;
}

static bool core_demarshal_create_node(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_iter it;
	uint32_t version, type, new_id, i;
	const char *factory_name, *name;
	struct spa_dict props;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_STRING, &factory_name,
			      SPA_POD_TYPE_STRING, &name,
			      SPA_POD_TYPE_ID, &type,
			      SPA_POD_TYPE_INT, &version,
			      SPA_POD_TYPE_INT, &props.n_items, 0))
		return false;

	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (!spa_pod_iter_get(&it,
				      SPA_POD_TYPE_STRING, &props.items[i].key,
				      SPA_POD_TYPE_STRING, &props.items[i].value, 0))
			return false;
	}
	if (!spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &new_id, 0))
		return false;

	pw_resource_do(resource, struct pw_core_proxy_methods, create_node, factory_name, name,
								      type, version,
								      &props, new_id);
	return true;
}

static bool core_demarshal_create_link(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_iter it;
	uint32_t new_id, i;
	uint32_t output_node_id, output_port_id, input_node_id, input_port_id;
	struct spa_format *filter = NULL;
	struct spa_dict props;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &output_node_id,
			      SPA_POD_TYPE_INT, &output_port_id,
			      SPA_POD_TYPE_INT, &input_node_id,
			      SPA_POD_TYPE_INT, &input_port_id,
			      -SPA_POD_TYPE_OBJECT, &filter,
			      SPA_POD_TYPE_INT, &props.n_items, 0))
		return false;

	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (!spa_pod_iter_get(&it,
				      SPA_POD_TYPE_STRING, &props.items[i].key,
				      SPA_POD_TYPE_STRING, &props.items[i].value, 0))
			return false;
	}
	if (!spa_pod_iter_get(&it, SPA_POD_TYPE_INT, &new_id, 0))
		return false;

	pw_resource_do(resource, struct pw_core_proxy_methods, create_link, output_node_id,
								      output_port_id,
								      input_node_id,
								      input_port_id,
								      filter,
								      &props,
								      new_id);
	return true;
}

static bool core_demarshal_update_types_server(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_iter it;
	uint32_t first_id, n_types;
	const char **types;
	int i;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it, SPA_POD_TYPE_INT, &first_id, SPA_POD_TYPE_INT, &n_types, 0))
		return false;

	types = alloca(n_types * sizeof(char *));
	for (i = 0; i < n_types; i++) {
		if (!spa_pod_iter_get(&it, SPA_POD_TYPE_STRING, &types[i], 0))
			return false;
	}
	pw_resource_do(resource, struct pw_core_proxy_methods, update_types, first_id, n_types, types);
	return true;
}

static void registry_marshal_global(void *object, uint32_t id, uint32_t parent_id, uint32_t permissions,
				    uint32_t type, uint32_t version)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_REGISTRY_PROXY_EVENT_GLOBAL);

	spa_pod_builder_struct(b, &f,
			       SPA_POD_TYPE_INT, id,
			       SPA_POD_TYPE_INT, parent_id,
			       SPA_POD_TYPE_INT, permissions,
			       SPA_POD_TYPE_ID, type,
			       SPA_POD_TYPE_INT, version);

	pw_protocol_native_end_resource(resource, b);
}

static void registry_marshal_global_remove(void *object, uint32_t id)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_REGISTRY_PROXY_EVENT_GLOBAL_REMOVE);

	spa_pod_builder_struct(b, &f, SPA_POD_TYPE_INT, id);

	pw_protocol_native_end_resource(resource, b);
}

static bool registry_demarshal_bind(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_iter it;
	uint32_t id, version, type, new_id;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &id,
			      SPA_POD_TYPE_ID, &type,
			      SPA_POD_TYPE_INT, &version,
			      SPA_POD_TYPE_INT, &new_id, 0))
		return false;

	pw_resource_do(resource, struct pw_registry_proxy_methods, bind, id, type, version, new_id);
	return true;
}

static void module_marshal_info(void *object, struct pw_module_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_resource(resource, PW_MODULE_PROXY_EVENT_INFO);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(b,
			    SPA_POD_TYPE_STRUCT, &f,
			    SPA_POD_TYPE_LONG, info->change_mask,
			    SPA_POD_TYPE_STRING, info->name,
			    SPA_POD_TYPE_STRING, info->filename,
			    SPA_POD_TYPE_STRING, info->args, SPA_POD_TYPE_INT, n_items, 0);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    SPA_POD_TYPE_STRING, info->props->items[i].key,
				    SPA_POD_TYPE_STRING, info->props->items[i].value, 0);
	}
	spa_pod_builder_add(b, -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_protocol_native_end_resource(resource, b);
}

static bool module_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	struct spa_dict props;
	struct pw_module_info info;
	int i;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_LONG, &info.change_mask,
			      SPA_POD_TYPE_STRING, &info.name,
			      SPA_POD_TYPE_STRING, &info.filename,
			      SPA_POD_TYPE_STRING, &info.args, SPA_POD_TYPE_INT, &props.n_items, 0))
		return false;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (!spa_pod_iter_get(&it,
				      SPA_POD_TYPE_STRING, &props.items[i].key,
				      SPA_POD_TYPE_STRING, &props.items[i].value, 0))
			return false;
	}
	pw_proxy_notify(proxy, struct pw_module_proxy_events, info, &info);
	return true;
}

static void node_marshal_info(void *object, struct pw_node_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_resource(resource, PW_NODE_PROXY_EVENT_INFO);

	spa_pod_builder_add(b,
			    SPA_POD_TYPE_STRUCT, &f,
			    SPA_POD_TYPE_LONG, info->change_mask,
			    SPA_POD_TYPE_STRING, info->name,
			    SPA_POD_TYPE_INT, info->max_input_ports,
			    SPA_POD_TYPE_INT, info->n_input_ports,
			    SPA_POD_TYPE_INT, info->n_input_formats, 0);

	for (i = 0; i < info->n_input_formats; i++)
		spa_pod_builder_add(b, SPA_POD_TYPE_POD, info->input_formats[i], 0);

	spa_pod_builder_add(b,
			    SPA_POD_TYPE_INT, info->max_output_ports,
			    SPA_POD_TYPE_INT, info->n_output_ports,
			    SPA_POD_TYPE_INT, info->n_output_formats, 0);

	for (i = 0; i < info->n_output_formats; i++)
		spa_pod_builder_add(b, SPA_POD_TYPE_POD, info->output_formats[i], 0);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(b,
			    SPA_POD_TYPE_INT, info->state,
			    SPA_POD_TYPE_STRING, info->error, SPA_POD_TYPE_INT, n_items, 0);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    SPA_POD_TYPE_STRING, info->props->items[i].key,
				    SPA_POD_TYPE_STRING, info->props->items[i].value, 0);
	}
	spa_pod_builder_add(b, -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_protocol_native_end_resource(resource, b);
}

static bool node_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	struct spa_dict props;
	struct pw_node_info info;
	int i;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_LONG, &info.change_mask,
			      SPA_POD_TYPE_STRING, &info.name,
			      SPA_POD_TYPE_INT, &info.max_input_ports,
			      SPA_POD_TYPE_INT, &info.n_input_ports,
			      SPA_POD_TYPE_INT, &info.n_input_formats, 0))
		return false;

	info.input_formats = alloca(info.n_input_formats * sizeof(struct spa_format *));
	for (i = 0; i < info.n_input_formats; i++)
		if (!spa_pod_iter_get(&it, SPA_POD_TYPE_OBJECT, &info.input_formats[i], 0))
			return false;

	if (!spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &info.max_output_ports,
			      SPA_POD_TYPE_INT, &info.n_output_ports,
			      SPA_POD_TYPE_INT, &info.n_output_formats, 0))
		return false;

	info.output_formats = alloca(info.n_output_formats * sizeof(struct spa_format *));
	for (i = 0; i < info.n_output_formats; i++)
		if (!spa_pod_iter_get(&it, SPA_POD_TYPE_OBJECT, &info.output_formats[i], 0))
			return false;

	if (!spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &info.state,
			      SPA_POD_TYPE_STRING, &info.error,
			      SPA_POD_TYPE_INT, &props.n_items, 0))
		return false;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (!spa_pod_iter_get(&it,
				      SPA_POD_TYPE_STRING, &props.items[i].key,
				      SPA_POD_TYPE_STRING, &props.items[i].value, 0))
			return false;
	}
	pw_proxy_notify(proxy, struct pw_node_proxy_events, info, &info);
	return true;
}

static void client_marshal_info(void *object, struct pw_client_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_PROXY_EVENT_INFO);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(b,
			    SPA_POD_TYPE_STRUCT, &f,
			    SPA_POD_TYPE_LONG, info->change_mask,
			    SPA_POD_TYPE_INT, n_items, 0);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    SPA_POD_TYPE_STRING, info->props->items[i].key,
				    SPA_POD_TYPE_STRING, info->props->items[i].value, 0);
	}
	spa_pod_builder_add(b, -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_protocol_native_end_resource(resource, b);
}

static bool client_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	struct spa_dict props;
	struct pw_client_info info;
	uint32_t i;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_LONG, &info.change_mask,
			      SPA_POD_TYPE_INT, &props.n_items, 0))
		return false;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (!spa_pod_iter_get(&it,
				      SPA_POD_TYPE_STRING, &props.items[i].key,
				      SPA_POD_TYPE_STRING, &props.items[i].value, 0))
			return false;
	}
	pw_proxy_notify(proxy, struct pw_client_proxy_events, info, &info);
	return true;
}

static void link_marshal_info(void *object, struct pw_link_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_resource(resource, PW_LINK_PROXY_EVENT_INFO);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(b,
			    SPA_POD_TYPE_STRUCT, &f,
			    SPA_POD_TYPE_LONG, info->change_mask,
			    SPA_POD_TYPE_INT, info->output_node_id,
			    SPA_POD_TYPE_INT, info->output_port_id,
			    SPA_POD_TYPE_INT, info->input_node_id,
			    SPA_POD_TYPE_INT, info->input_port_id,
			    SPA_POD_TYPE_POD, info->format,
			    SPA_POD_TYPE_INT, n_items, 0);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
				    SPA_POD_TYPE_STRING, info->props->items[i].key,
				    SPA_POD_TYPE_STRING, info->props->items[i].value, 0);
	}
	spa_pod_builder_add(b, -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_protocol_native_end_resource(resource, b);
}

static bool link_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	struct spa_dict props;
	struct pw_link_info info = { 0, };
	int i;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_LONG, &info.change_mask,
			      SPA_POD_TYPE_INT, &info.output_node_id,
			      SPA_POD_TYPE_INT, &info.output_port_id,
			      SPA_POD_TYPE_INT, &info.input_node_id,
			      SPA_POD_TYPE_INT, &info.input_port_id,
			      -SPA_POD_TYPE_OBJECT, &info.format,
			      SPA_POD_TYPE_INT, &props.n_items, 0))
		return false;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (!spa_pod_iter_get(&it,
				      SPA_POD_TYPE_STRING, &props.items[i].key,
				      SPA_POD_TYPE_STRING, &props.items[i].value, 0))
			return false;
	}
	pw_proxy_notify(proxy, struct pw_link_proxy_events, info, &info);
	return true;
}

static bool registry_demarshal_global(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	uint32_t id, parent_id, permissions, type, version;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &id,
			      SPA_POD_TYPE_INT, &parent_id,
			      SPA_POD_TYPE_INT, &permissions,
			      SPA_POD_TYPE_ID, &type,
			      SPA_POD_TYPE_INT, &version, 0))
		return false;

	pw_proxy_notify(proxy, struct pw_registry_proxy_events, global, id, parent_id, permissions, type, version);
	return true;
}

static bool registry_demarshal_global_remove(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	uint32_t id;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it, SPA_POD_TYPE_INT, &id, 0))
		return false;

	pw_proxy_notify(proxy, struct pw_registry_proxy_events, global_remove, id);
	return true;
}

static void registry_marshal_bind(void *object, uint32_t id,
				  uint32_t type, uint32_t version, uint32_t new_id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_proxy(proxy, PW_REGISTRY_PROXY_METHOD_BIND);

	spa_pod_builder_struct(b, &f,
			       SPA_POD_TYPE_INT, id,
			       SPA_POD_TYPE_ID, type,
			       SPA_POD_TYPE_INT, version,
			       SPA_POD_TYPE_INT, new_id);

	pw_protocol_native_end_proxy(proxy, b);
}

static const struct pw_core_proxy_methods pw_protocol_native_core_method_marshal = {
	PW_VERSION_CORE_PROXY_METHODS,
	&core_marshal_update_types_client,
	&core_marshal_sync,
	&core_marshal_get_registry,
	&core_marshal_client_update,
	&core_marshal_create_node,
	&core_marshal_create_link
};

static const struct pw_protocol_native_demarshal pw_protocol_native_core_method_demarshal[PW_CORE_PROXY_METHOD_NUM] = {
	{ &core_demarshal_update_types_server, 0, },
	{ &core_demarshal_sync, 0, },
	{ &core_demarshal_get_registry, 0, },
	{ &core_demarshal_client_update, 0, },
	{ &core_demarshal_create_node, PW_PROTOCOL_NATIVE_REMAP, },
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
	PW_CORE_PROXY_METHOD_NUM,
	&pw_protocol_native_core_method_marshal,
	pw_protocol_native_core_method_demarshal,
	PW_CORE_PROXY_EVENT_NUM,
	&pw_protocol_native_core_event_marshal,
	pw_protocol_native_core_event_demarshal
};

static const struct pw_registry_proxy_methods pw_protocol_native_registry_method_marshal = {
	PW_VERSION_REGISTRY_PROXY_METHODS,
	&registry_marshal_bind
};

static const struct pw_protocol_native_demarshal pw_protocol_native_registry_method_demarshal[] = {
	{ &registry_demarshal_bind, PW_PROTOCOL_NATIVE_REMAP, }
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
	PW_REGISTRY_PROXY_METHOD_NUM,
	&pw_protocol_native_registry_method_marshal,
	pw_protocol_native_registry_method_demarshal,
	PW_REGISTRY_PROXY_EVENT_NUM,
	&pw_protocol_native_registry_event_marshal,
	pw_protocol_native_registry_event_demarshal,
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
	0, NULL, NULL,
	PW_MODULE_PROXY_EVENT_NUM,
	&pw_protocol_native_module_event_marshal,
	pw_protocol_native_module_event_demarshal,
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
	0, NULL, NULL,
	PW_NODE_PROXY_EVENT_NUM,
	&pw_protocol_native_node_event_marshal,
	pw_protocol_native_node_event_demarshal,
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
	0, NULL, NULL,
	PW_CLIENT_PROXY_EVENT_NUM,
	&pw_protocol_native_client_event_marshal,
	pw_protocol_native_client_event_demarshal,
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
	0, NULL, NULL,
	PW_LINK_PROXY_EVENT_NUM,
	&pw_protocol_native_link_event_marshal,
	pw_protocol_native_link_event_demarshal,
};

void pw_protocol_native_init(struct pw_protocol *protocol)
{
	pw_protocol_add_marshal(protocol, &pw_protocol_native_core_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_registry_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_module_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_node_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_client_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_link_marshal);
}
