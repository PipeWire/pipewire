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

#include <stdio.h>
#include <errno.h>

#include <spa/pod/parser.h>

#include <pipewire/pipewire.h>
#include <extensions/protocol-native.h>

#include "connection.h"

static int core_method_marshal_hello(void *object, uint32_t version)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_HELLO, NULL);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(version));

	return pw_protocol_native_end_proxy(proxy, b);
}

static int core_method_marshal_sync(void *object, uint32_t id, uint32_t seq)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	int res;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_SYNC, &res);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(id),
			SPA_POD_Int(res));

	return pw_protocol_native_end_proxy(proxy, b);
}

static int core_method_marshal_done(void *object, uint32_t id, uint32_t seq)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_DONE, NULL);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(id),
			SPA_POD_Int(seq));

	return pw_protocol_native_end_proxy(proxy, b);
}

static int core_method_marshal_error(void *object, uint32_t id, int res, const char *error)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_ERROR, NULL);

	spa_pod_builder_add_struct(b,
			       SPA_POD_Int(id),
			       SPA_POD_Int(res),
			       SPA_POD_String(error));

	return pw_protocol_native_end_proxy(proxy, b);
}

static int core_method_marshal_get_registry(void *object, uint32_t version, uint32_t new_id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_GET_REGISTRY, NULL);

	spa_pod_builder_add_struct(b,
		       SPA_POD_Int(version),
		       SPA_POD_Int(new_id));

	return pw_protocol_native_end_proxy(proxy, b);
}

static void push_dict(struct spa_pod_builder *b, const struct spa_dict *dict)
{
	uint32_t i, n_items;
	struct spa_pod_frame f;

	n_items = dict ? dict->n_items : 0;

	spa_pod_builder_push_struct(b, &f);
	spa_pod_builder_int(b, n_items);
	for (i = 0; i < n_items; i++) {
		spa_pod_builder_string(b, dict->items[i].key);
		spa_pod_builder_string(b, dict->items[i].value);
	}
	spa_pod_builder_pop(b, &f);
}

static int
core_method_marshal_create_object(void *object,
			   const char *factory_name,
			   uint32_t type, uint32_t version,
			   const struct spa_dict *props, uint32_t new_id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_CREATE_OBJECT, NULL);

	spa_pod_builder_push_struct(b, &f);
	spa_pod_builder_add(b,
			SPA_POD_String(factory_name),
			SPA_POD_Id(type),
			SPA_POD_Int(version),
			NULL);
	push_dict(b, props);
	spa_pod_builder_int(b, new_id);
	spa_pod_builder_pop(b, &f);

	return pw_protocol_native_end_proxy(proxy, b);
}

static int
core_method_marshal_destroy(void *object, uint32_t id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CORE_PROXY_METHOD_DESTROY, NULL);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(id));

	return pw_protocol_native_end_proxy(proxy, b);
}

static int core_event_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_dict props;
	struct spa_pod_frame f[2];
	struct pw_core_info info;
	struct spa_pod_parser prs;
	uint32_t i;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0)
		return -EINVAL;
	if (spa_pod_parser_get(&prs,
			 SPA_POD_Int(&info.id),
			 SPA_POD_Long(&info.change_mask),
			 SPA_POD_String(&info.user_name),
			 SPA_POD_String(&info.host_name),
			 SPA_POD_String(&info.version),
			 SPA_POD_String(&info.name),
			 SPA_POD_Int(&info.cookie), NULL) < 0)
		return -EINVAL;

	if (spa_pod_parser_push_struct(&prs, &f[1]) < 0)
		return -EINVAL;
	if (spa_pod_parser_get(&prs,
			 SPA_POD_Int(&props.n_items), NULL) < 0)
		return -EINVAL;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				       SPA_POD_String(&props.items[i].key),
				       SPA_POD_String(&props.items[i].value),
				       NULL) < 0)
			return -EINVAL;
	}
	return pw_proxy_notify(proxy, struct pw_core_proxy_events, info, 0, &info);
}

static int core_event_demarshal_done(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t id, seq;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&id),
				SPA_POD_Int(&seq)) < 0)
		return -EINVAL;

	return pw_proxy_notify(proxy, struct pw_core_proxy_events, done, 0, id, seq);
}

static int core_event_demarshal_sync(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t id, seq;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&id),
				SPA_POD_Int(&seq)) < 0)
		return -EINVAL;

	return pw_proxy_notify(proxy, struct pw_core_proxy_events, sync, 0, id, seq);
}

static int core_event_demarshal_error(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t id, res;
	const char *error;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Int(&id),
			SPA_POD_Int(&res),
			SPA_POD_String(&error)) < 0)
		return -EINVAL;

	return pw_proxy_notify(proxy, struct pw_core_proxy_events, error, 0, id, res, error);
}

static int core_event_demarshal_remove_id(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t id;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs, SPA_POD_Int(&id)) < 0)
		return -EINVAL;

	return pw_proxy_notify(proxy, struct pw_core_proxy_events, remove_id, 0, id);
}

static int core_event_marshal_info(void *object, const struct pw_core_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_CORE_PROXY_EVENT_INFO, NULL);

	spa_pod_builder_push_struct(b, &f);
	spa_pod_builder_add(b,
			    SPA_POD_Int(info->id),
			    SPA_POD_Long(info->change_mask),
			    SPA_POD_String(info->user_name),
			    SPA_POD_String(info->host_name),
			    SPA_POD_String(info->version),
			    SPA_POD_String(info->name),
			    SPA_POD_Int(info->cookie),
			    NULL);
	push_dict(b, info->props);
	spa_pod_builder_pop(b, &f);

	return pw_protocol_native_end_resource(resource, b);
}

static int core_event_marshal_done(void *object, uint32_t id, uint32_t seq)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CORE_PROXY_EVENT_DONE, NULL);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(id),
			SPA_POD_Int(seq));

	return pw_protocol_native_end_resource(resource, b);
}

static int core_event_marshal_sync(void *object, uint32_t id, uint32_t seq)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	int res;

	b = pw_protocol_native_begin_resource(resource, PW_CORE_PROXY_EVENT_SYNC, &res);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(id),
			SPA_POD_Int(res));

	return pw_protocol_native_end_resource(resource, b);
}

static int core_event_marshal_error(void *object, uint32_t id, int res, const char *error)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CORE_PROXY_EVENT_ERROR, NULL);

	spa_pod_builder_add_struct(b,
			       SPA_POD_Int(id),
			       SPA_POD_Int(res),
			       SPA_POD_String(error));

	return pw_protocol_native_end_resource(resource, b);
}

static int core_event_marshal_remove_id(void *object, uint32_t id)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CORE_PROXY_EVENT_REMOVE_ID, NULL);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(id));

	return pw_protocol_native_end_resource(resource, b);
}

static int core_method_demarshal_hello(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t version;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&version)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_core_proxy_methods, hello, 0, version);
}

static int core_method_demarshal_sync(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id, seq;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&id),
				SPA_POD_Int(&seq)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_core_proxy_methods, sync, 0, id, seq);
}

static int core_method_demarshal_done(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id, seq;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&id),
				SPA_POD_Int(&seq)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_core_proxy_methods, done, 0, id, seq);
}

static int core_method_demarshal_error(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id, res;
	const char *error;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Int(&id),
			SPA_POD_Int(&res),
			SPA_POD_String(&error)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_core_proxy_methods, error, 0, id, res, error);
}

static int core_method_demarshal_get_registry(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	int32_t version, new_id;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&version),
				SPA_POD_Int(&new_id)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_core_proxy_methods, get_registry, 0, version, new_id);
}

static int core_method_demarshal_create_object(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	struct spa_pod_frame f[2];
	uint32_t version, type, new_id, i;
	const char *factory_name;
	struct spa_dict props;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_String(&factory_name),
			SPA_POD_Id(&type),
			SPA_POD_Int(&version),
			NULL) < 0)
		return -EINVAL;

	if (spa_pod_parser_push_struct(&prs, &f[1]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&props.n_items), NULL) < 0)
		return -EINVAL;

	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				SPA_POD_String(&props.items[i].key),
				SPA_POD_String(&props.items[i].value), NULL) < 0)
			return -EINVAL;
	}
	spa_pod_parser_pop(&prs, &f[1]);

	if (spa_pod_parser_get(&prs,
			SPA_POD_Int(&new_id), NULL) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_core_proxy_methods, create_object, 0, factory_name,
								      type, version,
								      &props, new_id);
}

static int core_method_demarshal_destroy(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Int(&id)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_core_proxy_methods, destroy, 0, id);
}

static int registry_marshal_global(void *object, uint32_t id, uint32_t parent_id, uint32_t permissions,
				    uint32_t type, uint32_t version, const struct spa_dict *props)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_REGISTRY_PROXY_EVENT_GLOBAL, NULL);

	spa_pod_builder_push_struct(b, &f);
	spa_pod_builder_add(b,
			    SPA_POD_Int(id),
			    SPA_POD_Int(parent_id),
			    SPA_POD_Int(permissions),
			    SPA_POD_Id(type),
			    SPA_POD_Int(version),
			    NULL);
	push_dict(b, props);
	spa_pod_builder_pop(b, &f);

	return pw_protocol_native_end_resource(resource, b);
}

static int registry_marshal_global_remove(void *object, uint32_t id)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_REGISTRY_PROXY_EVENT_GLOBAL_REMOVE, NULL);

	spa_pod_builder_add_struct(b, SPA_POD_Int(id));

	return pw_protocol_native_end_resource(resource, b);
}

static int registry_demarshal_bind(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id, version, type, new_id;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Int(&id),
			SPA_POD_Id(&type),
			SPA_POD_Int(&version),
			SPA_POD_Int(&new_id)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_registry_proxy_methods, bind, 0, id, type, version, new_id);
}

static int registry_demarshal_destroy(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Int(&id)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_registry_proxy_methods, destroy, 0, id);
}

static int module_marshal_info(void *object, const struct pw_module_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_MODULE_PROXY_EVENT_INFO, NULL);

	spa_pod_builder_push_struct(b, &f);
	spa_pod_builder_add(b,
			    SPA_POD_Int(info->id),
			    SPA_POD_Long(info->change_mask),
			    SPA_POD_String(info->name),
			    SPA_POD_String(info->filename),
			    SPA_POD_String(info->args),
			    NULL);
	push_dict(b, info->props);
	spa_pod_builder_pop(b, &f);

	return pw_protocol_native_end_resource(resource, b);
}

static int module_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	struct spa_pod_frame f[2];
	struct spa_dict props;
	struct pw_module_info info;
	uint32_t i;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&info.id),
			SPA_POD_Long(&info.change_mask),
			SPA_POD_String(&info.name),
			SPA_POD_String(&info.filename),
			SPA_POD_String(&info.args), NULL) < 0)
		return -EINVAL;

	if (spa_pod_parser_push_struct(&prs, &f[1]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&props.n_items), NULL) < 0)
		return -EINVAL;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				    SPA_POD_String(&props.items[i].key),
				    SPA_POD_String(&props.items[i].value), NULL) < 0)
			return -EINVAL;
	}
	return pw_proxy_notify(proxy, struct pw_module_proxy_events, info, 0, &info);
}

static int device_marshal_info(void *object, const struct pw_device_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_DEVICE_PROXY_EVENT_INFO, NULL);

	spa_pod_builder_push_struct(b, &f);
	spa_pod_builder_add(b,
			    SPA_POD_Int(info->id),
			    SPA_POD_String(info->name),
			    SPA_POD_Long(info->change_mask),
			    NULL);
	push_dict(b, info->props);
	spa_pod_builder_pop(b, &f);

	return pw_protocol_native_end_resource(resource, b);
}

static int device_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	struct spa_pod_frame f[2];
	struct spa_dict props;
	struct pw_device_info info;
	uint32_t i;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&info.id),
			SPA_POD_String(&info.name),
			SPA_POD_Long(&info.change_mask), NULL) < 0)
		return -EINVAL;

	if (spa_pod_parser_push_struct(&prs, &f[1]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&props.n_items), NULL) < 0)
		return -EINVAL;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				    SPA_POD_String(&props.items[i].key),
				    SPA_POD_String(&props.items[i].value), NULL) < 0)
			return -EINVAL;
	}
	return pw_proxy_notify(proxy, struct pw_device_proxy_events, info, 0, &info);
}

static int device_marshal_param(void *object, uint32_t seq, uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_DEVICE_PROXY_EVENT_PARAM, NULL);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(seq),
			SPA_POD_Id(id),
			SPA_POD_Int(index),
			SPA_POD_Int(next),
			SPA_POD_Pod(param));

	return pw_protocol_native_end_resource(resource, b);
}

static int device_demarshal_param(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t seq, id, index, next;
	struct spa_pod *param;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&seq),
				SPA_POD_Id(&id),
				SPA_POD_Int(&index),
				SPA_POD_Int(&next),
				SPA_POD_Pod(&param)) < 0)
		return -EINVAL;

	return pw_proxy_notify(proxy, struct pw_device_proxy_events, param, 0,
			seq, id, index, next, param);
}

static int device_marshal_enum_params(void *object, uint32_t seq,
		uint32_t id, uint32_t index, uint32_t num, const struct spa_pod *filter)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	int res;

	b = pw_protocol_native_begin_proxy(proxy, PW_DEVICE_PROXY_METHOD_ENUM_PARAMS, &res);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(res),
			SPA_POD_Id(id),
			SPA_POD_Int(index),
			SPA_POD_Int(num),
			SPA_POD_Pod(filter));

	return pw_protocol_native_end_proxy(proxy, b);
}

static int device_demarshal_enum_params(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id, index, num, seq;
	struct spa_pod *filter;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&seq),
				SPA_POD_Id(&id),
				SPA_POD_Int(&index),
				SPA_POD_Int(&num),
				SPA_POD_Pod(&filter)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_device_proxy_methods, enum_params, 0,
			seq, id, index, num, filter);
}

static int device_marshal_set_param(void *object, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_DEVICE_PROXY_METHOD_SET_PARAM, NULL);

	spa_pod_builder_add_struct(b,
			SPA_POD_Id(id),
			SPA_POD_Int(flags),
			SPA_POD_Pod(param));
	return pw_protocol_native_end_proxy(proxy, b);
}

static int device_demarshal_set_param(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id, flags;
	struct spa_pod *param;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Id(&id),
				SPA_POD_Int(&flags),
				SPA_POD_Pod(&param)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_device_proxy_methods, set_param, 0, id, flags, param);
}

static int factory_marshal_info(void *object, const struct pw_factory_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_FACTORY_PROXY_EVENT_INFO, NULL);

	spa_pod_builder_push_struct(b, &f);
	spa_pod_builder_add(b,
			    SPA_POD_Int(info->id),
			    SPA_POD_Long(info->change_mask),
			    SPA_POD_String(info->name),
			    SPA_POD_Id(info->type),
			    SPA_POD_Int(info->version),
			    NULL);
	push_dict(b, info->props);
	spa_pod_builder_pop(b, &f);

	return pw_protocol_native_end_resource(resource, b);
}

static int factory_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	struct spa_pod_frame f[2];
	struct spa_dict props;
	struct pw_factory_info info;
	uint32_t i;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&info.id),
			SPA_POD_Long(&info.change_mask),
			SPA_POD_String(&info.name),
			SPA_POD_Id(&info.type),
			SPA_POD_Int(&info.version), NULL) < 0)
		return -EINVAL;

	if (spa_pod_parser_push_struct(&prs, &f[1]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&props.n_items), NULL) < 0)
		return -EINVAL;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				       SPA_POD_String(&props.items[i].key),
				       SPA_POD_String(&props.items[i].value), NULL) < 0)
			return -EINVAL;
	}
	return pw_proxy_notify(proxy, struct pw_factory_proxy_events, info, 0, &info);
}

static int node_marshal_info(void *object, const struct pw_node_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_NODE_PROXY_EVENT_INFO, NULL);

	spa_pod_builder_push_struct(b, &f);
	spa_pod_builder_add(b,
			    SPA_POD_Int(info->id),
			    SPA_POD_Long(info->change_mask),
			    SPA_POD_String(info->name),
			    SPA_POD_Int(info->max_input_ports),
			    SPA_POD_Int(info->n_input_ports),
			    SPA_POD_Int(info->max_output_ports),
			    SPA_POD_Int(info->n_output_ports),
			    SPA_POD_Id(info->state),
			    SPA_POD_String(info->error),
			    NULL);
	push_dict(b, info->props);
	spa_pod_builder_pop(b, &f);

	return pw_protocol_native_end_resource(resource, b);
}

static int node_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	struct spa_pod_frame f[2];
	struct spa_dict props;
	struct pw_node_info info;
	uint32_t i;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&info.id),
			SPA_POD_Long(&info.change_mask),
			SPA_POD_String(&info.name),
			SPA_POD_Int(&info.max_input_ports),
			SPA_POD_Int(&info.n_input_ports),
			SPA_POD_Int(&info.max_output_ports),
			SPA_POD_Int(&info.n_output_ports),
			SPA_POD_Id(&info.state),
			SPA_POD_String(&info.error), NULL) < 0)
		return -EINVAL;

	if (spa_pod_parser_push_struct(&prs, &f[1]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&props.n_items), NULL) < 0)
		return -EINVAL;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				       SPA_POD_String(&props.items[i].key),
				       SPA_POD_String(&props.items[i].value), NULL) < 0)
			return -EINVAL;
	}
	return pw_proxy_notify(proxy, struct pw_node_proxy_events, info, 0, &info);
}

static int node_marshal_param(void *object, uint32_t seq, uint32_t id,
		uint32_t index, uint32_t next, const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_NODE_PROXY_EVENT_PARAM, NULL);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(seq),
			SPA_POD_Id(id),
			SPA_POD_Int(index),
			SPA_POD_Int(next),
			SPA_POD_Pod(param));

	return pw_protocol_native_end_resource(resource, b);
}

static int node_demarshal_param(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t seq, id, index, next;
	struct spa_pod *param;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&seq),
				SPA_POD_Id(&id),
				SPA_POD_Int(&index),
				SPA_POD_Int(&next),
				SPA_POD_Pod(&param)) < 0)
		return -EINVAL;

	return pw_proxy_notify(proxy, struct pw_node_proxy_events, param, 0,
			seq, id, index, next, param);
}

static int node_marshal_enum_params(void *object, uint32_t seq, uint32_t id,
		uint32_t index, uint32_t num, const struct spa_pod *filter)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	int res;

	b = pw_protocol_native_begin_proxy(proxy, PW_NODE_PROXY_METHOD_ENUM_PARAMS, &res);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(res),
			SPA_POD_Id(id),
			SPA_POD_Int(index),
			SPA_POD_Int(num),
			SPA_POD_Pod(filter));

	return pw_protocol_native_end_proxy(proxy, b);
}

static int node_demarshal_enum_params(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t seq, id, index, num;
	struct spa_pod *filter;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&seq),
				SPA_POD_Id(&id),
				SPA_POD_Int(&index),
				SPA_POD_Int(&num),
				SPA_POD_Pod(&filter)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_node_proxy_methods, enum_params, 0,
			seq, id, index, num, filter);
}

static int node_marshal_set_param(void *object, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_NODE_PROXY_METHOD_SET_PARAM, NULL);

	spa_pod_builder_add_struct(b,
			SPA_POD_Id(id),
			SPA_POD_Int(flags),
			SPA_POD_Pod(param));
	return pw_protocol_native_end_proxy(proxy, b);
}

static int node_demarshal_set_param(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id, flags;
	struct spa_pod *param;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Id(&id),
				SPA_POD_Int(&flags),
				SPA_POD_Pod(&param)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_node_proxy_methods, set_param, 0, id, flags, param);
}

static int node_marshal_send_command(void *object, const struct spa_command *command)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_NODE_PROXY_METHOD_SEND_COMMAND, NULL);
	spa_pod_builder_add_struct(b,
			SPA_POD_Pod(command));
	return pw_protocol_native_end_proxy(proxy, b);
}

static int node_demarshal_send_command(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	struct spa_command *command;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Pod(&command)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_node_proxy_methods, send_command, 0, command);
}

static int port_marshal_info(void *object, const struct pw_port_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_PORT_PROXY_EVENT_INFO, NULL);

	spa_pod_builder_push_struct(b, &f);
	spa_pod_builder_add(b,
			    SPA_POD_Int(info->id),
			    SPA_POD_Int(info->direction),
			    SPA_POD_Long(info->change_mask),
			    NULL);
	push_dict(b, info->props);
	spa_pod_builder_pop(b, &f);

	return pw_protocol_native_end_resource(resource, b);
}

static int port_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	struct spa_pod_frame f[2];
	struct spa_dict props;
	struct pw_port_info info;
	uint32_t i;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&info.id),
			SPA_POD_Int(&info.direction),
			SPA_POD_Long(&info.change_mask), NULL) < 0)
		return -EINVAL;

	if (spa_pod_parser_push_struct(&prs, &f[1]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&props.n_items), NULL) < 0)
		return -EINVAL;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				       SPA_POD_String(&props.items[i].key),
				       SPA_POD_String(&props.items[i].value), NULL) < 0)
			return -EINVAL;
	}
	return pw_proxy_notify(proxy, struct pw_port_proxy_events, info, 0, &info);
}

static int port_marshal_param(void *object, uint32_t seq, uint32_t id,
		uint32_t index, uint32_t next, const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_PORT_PROXY_EVENT_PARAM, NULL);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(seq),
			SPA_POD_Id(id),
			SPA_POD_Int(index),
			SPA_POD_Int(next),
			SPA_POD_Pod(param));

	return pw_protocol_native_end_resource(resource, b);
}

static int port_demarshal_param(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t seq, id, index, next;
	struct spa_pod *param;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&seq),
				SPA_POD_Id(&id),
				SPA_POD_Int(&index),
				SPA_POD_Int(&next),
				SPA_POD_Pod(&param)) < 0)
		return -EINVAL;

	return pw_proxy_notify(proxy, struct pw_port_proxy_events, param, 0,
			seq, id, index, next, param);
}

static int port_marshal_enum_params(void *object, uint32_t seq, uint32_t id,
		uint32_t index, uint32_t num, const struct spa_pod *filter)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	int res;

	b = pw_protocol_native_begin_proxy(proxy, PW_PORT_PROXY_METHOD_ENUM_PARAMS, &res);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(res),
			SPA_POD_Id(id),
			SPA_POD_Int(index),
			SPA_POD_Int(num),
			SPA_POD_Pod(filter));

	return pw_protocol_native_end_proxy(proxy, b);
}

static int port_demarshal_enum_params(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t seq, id, index, num;
	struct spa_pod *filter;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&seq),
				SPA_POD_Id(&id),
				SPA_POD_Int(&index),
				SPA_POD_Int(&num),
				SPA_POD_Pod(&filter)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_port_proxy_methods, enum_params, 0,
			seq, id, index, num, filter);
}

static int client_marshal_info(void *object, const struct pw_client_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_PROXY_EVENT_INFO, NULL);

	spa_pod_builder_push_struct(b, &f);
	spa_pod_builder_add(b,
			    SPA_POD_Int(info->id),
			    SPA_POD_Long(info->change_mask),
			    NULL);
	push_dict(b, info->props);
	spa_pod_builder_pop(b, &f);

	return pw_protocol_native_end_resource(resource, b);
}

static int client_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	struct spa_pod_frame f[2];
	struct spa_dict props;
	struct pw_client_info info;
	uint32_t i;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&info.id),
			SPA_POD_Long(&info.change_mask), NULL) < 0)
		return -EINVAL;

	if (spa_pod_parser_push_struct(&prs, &f[1]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&props.n_items), NULL) < 0)
		return -EINVAL;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				       SPA_POD_String(&props.items[i].key),
				       SPA_POD_String(&props.items[i].value), NULL) < 0)
			return -EINVAL;
	}
	return pw_proxy_notify(proxy, struct pw_client_proxy_events, info, 0, &info);
}

static int client_marshal_permissions(void *object, uint32_t index, uint32_t n_permissions,
		const struct pw_permission *permissions)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f[2];
	uint32_t i, n = 0;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_PROXY_EVENT_PERMISSIONS, NULL);

	for (i = 0; i < n_permissions; i++) {
		if (permissions[i].permissions != SPA_ID_INVALID)
			n++;
	}

	spa_pod_builder_push_struct(b, &f[0]);
	spa_pod_builder_int(b, index);
	spa_pod_builder_push_struct(b, &f[1]);
	spa_pod_builder_int(b, n);

	for (i = 0; i < n_permissions; i++) {
		if (permissions[i].permissions == SPA_ID_INVALID)
			continue;
		spa_pod_builder_int(b, permissions[i].id);
		spa_pod_builder_int(b, permissions[i].permissions);
	}
	spa_pod_builder_pop(b, &f[1]);
	spa_pod_builder_pop(b, &f[0]);

	return pw_protocol_native_end_resource(resource, b);
}

static int client_demarshal_permissions(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct pw_permission *permissions;
	struct spa_pod_parser prs;
	struct spa_pod_frame f[2];
	uint32_t i, index, n_permissions;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
	    spa_pod_parser_get(&prs,
		    SPA_POD_Int(&index), NULL) < 0)
		return -EINVAL;

	if (spa_pod_parser_push_struct(&prs, &f[1]) < 0 ||
	    spa_pod_parser_get(&prs,
		    SPA_POD_Int(&n_permissions), NULL) < 0)
		return -EINVAL;

	permissions = alloca(n_permissions * sizeof(struct pw_permission));
	for (i = 0; i < n_permissions; i++) {
		if (spa_pod_parser_get(&prs,
				SPA_POD_Int(&permissions[i].id),
				SPA_POD_Int(&permissions[i].permissions), NULL) < 0)
			return -EINVAL;
	}
	return pw_proxy_notify(proxy, struct pw_client_proxy_events, permissions, 0, index, n_permissions, permissions);
}

static int client_marshal_error(void *object, uint32_t id, int res, const char *error)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_PROXY_METHOD_ERROR, NULL);
	spa_pod_builder_add_struct(b,
			       SPA_POD_Int(id),
			       SPA_POD_Int(res),
			       SPA_POD_String(error));
	return pw_protocol_native_end_proxy(proxy, b);
}

static int client_demarshal_error(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t id, res;
	const char *error;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&id),
				SPA_POD_Int(&res),
				SPA_POD_String(&error)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_client_proxy_methods, error, 0, id, res, error);
}

static int client_marshal_get_permissions(void *object, uint32_t index, uint32_t num)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_PROXY_METHOD_GET_PERMISSIONS, NULL);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(index),
			SPA_POD_Int(num));

	return pw_protocol_native_end_proxy(proxy, b);
}

static int client_marshal_update_properties(void *object, const struct spa_dict *props)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_PROXY_METHOD_UPDATE_PROPERTIES, NULL);

	spa_pod_builder_push_struct(b, &f);
	push_dict(b, props);
	spa_pod_builder_pop(b, &f);

	return pw_protocol_native_end_proxy(proxy, b);
}

static int client_demarshal_update_properties(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_dict props;
	struct spa_pod_parser prs;
	struct spa_pod_frame f[2];
	uint32_t i;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
	    spa_pod_parser_push_struct(&prs, &f[1]) < 0 ||
	    spa_pod_parser_get(&prs,
		    SPA_POD_Int(&props.n_items), NULL) < 0)
		return -EINVAL;

	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				SPA_POD_String(&props.items[i].key),
				SPA_POD_String(&props.items[i].value), NULL) < 0)
			return -EINVAL;
	}
	return pw_resource_do(resource, struct pw_client_proxy_methods, update_properties, 0,
			&props);
}

static int client_demarshal_get_permissions(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t index, num;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&index),
				SPA_POD_Int(&num)) < 0)
		return -EINVAL;

	return pw_resource_do(resource, struct pw_client_proxy_methods, get_permissions, 0, index, num);
}

static int client_marshal_update_permissions(void *object, uint32_t n_permissions,
		const struct pw_permission *permissions)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;
	uint32_t i;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_PROXY_METHOD_UPDATE_PERMISSIONS, NULL);

	spa_pod_builder_push_struct(b, &f);
	spa_pod_builder_int(b, n_permissions);
	for (i = 0; i < n_permissions; i++) {
		spa_pod_builder_int(b, permissions[i].id);
		spa_pod_builder_int(b, permissions[i].permissions);
	}
	spa_pod_builder_pop(b, &f);

	return pw_protocol_native_end_proxy(proxy, b);
}

static int client_demarshal_update_permissions(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct pw_permission *permissions;
	struct spa_pod_parser prs;
	struct spa_pod_frame f[1];
	uint32_t i, n_permissions;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
	    spa_pod_parser_get(&prs,
				SPA_POD_Int(&n_permissions), NULL) < 0)
		return -EINVAL;

	permissions = alloca(n_permissions * sizeof(struct pw_permission));
	for (i = 0; i < n_permissions; i++) {
		if (spa_pod_parser_get(&prs,
				SPA_POD_Int(&permissions[i].id),
				SPA_POD_Int(&permissions[i].permissions), NULL) < 0)
			return -EINVAL;
	}
	return pw_resource_do(resource, struct pw_client_proxy_methods, update_permissions, 0,
			n_permissions, permissions);
}

static int link_marshal_info(void *object, const struct pw_link_info *info)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_LINK_PROXY_EVENT_INFO, NULL);

	spa_pod_builder_push_struct(b, &f);
	spa_pod_builder_add(b,
			    SPA_POD_Int(info->id),
			    SPA_POD_Long(info->change_mask),
			    SPA_POD_Int(info->output_node_id),
			    SPA_POD_Int(info->output_port_id),
			    SPA_POD_Int(info->input_node_id),
			    SPA_POD_Int(info->input_port_id),
			    SPA_POD_Int(info->state),
			    SPA_POD_String(info->error),
			    SPA_POD_Pod(info->format),
			    NULL);
	push_dict(b, info->props);
	spa_pod_builder_pop(b, &f);

	return pw_protocol_native_end_resource(resource, b);
}

static int link_demarshal_info(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	struct spa_pod_frame f[2];
	struct spa_dict props;
	struct pw_link_info info = { 0, };
	uint32_t i;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&info.id),
			SPA_POD_Long(&info.change_mask),
			SPA_POD_Int(&info.output_node_id),
			SPA_POD_Int(&info.output_port_id),
			SPA_POD_Int(&info.input_node_id),
			SPA_POD_Int(&info.input_port_id),
			SPA_POD_Int(&info.state),
			SPA_POD_String(&info.error),
			SPA_POD_Pod(&info.format), NULL) < 0)
		return -EINVAL;

	if (spa_pod_parser_push_struct(&prs, &f[1]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&props.n_items), NULL) < 0)
		return -EINVAL;

	info.props = &props;
	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				       SPA_POD_String(&props.items[i].key),
				       SPA_POD_String(&props.items[i].value), NULL) < 0)
			return -EINVAL;
	}
	return pw_proxy_notify(proxy, struct pw_link_proxy_events, info, 0, &info);
}

static int registry_demarshal_global(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	struct spa_pod_frame f[2];
	uint32_t id, parent_id, permissions, type, version, i;
	struct spa_dict props;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&id),
			SPA_POD_Int(&parent_id),
			SPA_POD_Int(&permissions),
			SPA_POD_Id(&type),
			SPA_POD_Int(&version), NULL) < 0)
		return -EINVAL;

	if (spa_pod_parser_push_struct(&prs, &f[1]) < 0 ||
	    spa_pod_parser_get(&prs,
			SPA_POD_Int(&props.n_items), NULL) < 0)
		return -EINVAL;

	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				       SPA_POD_String(&props.items[i].key),
				       SPA_POD_String(&props.items[i].value), NULL) < 0)
			return -EINVAL;
	}

	return pw_proxy_notify(proxy, struct pw_registry_proxy_events,
			global, 0, id, parent_id, permissions, type, version,
			props.n_items > 0 ? &props : NULL);
}

static int registry_demarshal_global_remove(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t id;

	spa_pod_parser_init(&prs, data, size);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Int(&id)) < 0)
		return -EINVAL;

	return pw_proxy_notify(proxy, struct pw_registry_proxy_events, global_remove, 0, id);
}

static int registry_marshal_bind(void *object, uint32_t id,
				  uint32_t type, uint32_t version, uint32_t new_id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_REGISTRY_PROXY_METHOD_BIND, NULL);

	spa_pod_builder_add_struct(b,
			       SPA_POD_Int(id),
			       SPA_POD_Id(type),
			       SPA_POD_Int(version),
			       SPA_POD_Int(new_id));

	return pw_protocol_native_end_proxy(proxy, b);
}

static int registry_marshal_destroy(void *object, uint32_t id)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_REGISTRY_PROXY_METHOD_DESTROY, NULL);
	spa_pod_builder_add_struct(b,
			       SPA_POD_Int(id));
	return pw_protocol_native_end_proxy(proxy, b);
}

static const struct pw_core_proxy_methods pw_protocol_native_core_method_marshal = {
	PW_VERSION_CORE_PROXY_METHODS,
	&core_method_marshal_hello,
	&core_method_marshal_sync,
	&core_method_marshal_done,
	&core_method_marshal_error,
	&core_method_marshal_get_registry,
	&core_method_marshal_create_object,
	&core_method_marshal_destroy,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_core_method_demarshal[PW_CORE_PROXY_METHOD_NUM] = {
	{ &core_method_demarshal_hello, 0, },
	{ &core_method_demarshal_sync, 0, },
	{ &core_method_demarshal_done, 0, },
	{ &core_method_demarshal_error, 0, },
	{ &core_method_demarshal_get_registry, 0, },
	{ &core_method_demarshal_create_object, 0, },
	{ &core_method_demarshal_destroy, 0, }
};

static const struct pw_core_proxy_events pw_protocol_native_core_event_marshal = {
	PW_VERSION_CORE_PROXY_EVENTS,
	&core_event_marshal_info,
	&core_event_marshal_done,
	&core_event_marshal_sync,
	&core_event_marshal_error,
	&core_event_marshal_remove_id,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_core_event_demarshal[PW_CORE_PROXY_EVENT_NUM] = {
	{ &core_event_demarshal_info, 0, },
	{ &core_event_demarshal_done, 0, },
	{ &core_event_demarshal_sync, 0, },
	{ &core_event_demarshal_error, 0, },
	{ &core_event_demarshal_remove_id, 0, },
};

static const struct pw_protocol_marshal pw_protocol_native_core_marshal = {
	PW_TYPE_INTERFACE_Core,
	PW_VERSION_CORE,
	PW_CORE_PROXY_METHOD_NUM,
	PW_CORE_PROXY_EVENT_NUM,
	&pw_protocol_native_core_method_marshal,
	pw_protocol_native_core_method_demarshal,
	&pw_protocol_native_core_event_marshal,
	pw_protocol_native_core_event_demarshal,
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
	PW_REGISTRY_PROXY_METHOD_NUM,
	PW_REGISTRY_PROXY_EVENT_NUM,
	&pw_protocol_native_registry_method_marshal,
	pw_protocol_native_registry_method_demarshal,
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
	PW_TYPE_INTERFACE_Module,
	PW_VERSION_MODULE,
	0,
	PW_MODULE_PROXY_EVENT_NUM,
	NULL, NULL,
	&pw_protocol_native_module_event_marshal,
	pw_protocol_native_module_event_demarshal,
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
	0,
	PW_FACTORY_PROXY_EVENT_NUM,
	NULL, NULL,
	&pw_protocol_native_factory_event_marshal,
	pw_protocol_native_factory_event_demarshal,
};

static const struct pw_device_proxy_methods pw_protocol_native_device_method_marshal = {
	PW_VERSION_DEVICE_PROXY_METHODS,
	&device_marshal_enum_params,
	&device_marshal_set_param,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_device_method_demarshal[] = {
	{ &device_demarshal_enum_params, 0, },
	{ &device_demarshal_set_param, PW_PERM_W, },
};

static const struct pw_device_proxy_events pw_protocol_native_device_event_marshal = {
	PW_VERSION_DEVICE_PROXY_EVENTS,
	&device_marshal_info,
	&device_marshal_param,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_device_event_demarshal[] = {
	{ &device_demarshal_info, 0, },
	{ &device_demarshal_param, 0, }
};

static const struct pw_protocol_marshal pw_protocol_native_device_marshal = {
	PW_TYPE_INTERFACE_Device,
	PW_VERSION_DEVICE,
	PW_DEVICE_PROXY_METHOD_NUM,
	PW_DEVICE_PROXY_EVENT_NUM,
	&pw_protocol_native_device_method_marshal,
	pw_protocol_native_device_method_demarshal,
	&pw_protocol_native_device_event_marshal,
	pw_protocol_native_device_event_demarshal,
};

static const struct pw_node_proxy_methods pw_protocol_native_node_method_marshal = {
	PW_VERSION_NODE_PROXY_METHODS,
	&node_marshal_enum_params,
	&node_marshal_set_param,
	&node_marshal_send_command,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_node_method_demarshal[] = {
	{ &node_demarshal_enum_params, 0, },
	{ &node_demarshal_set_param, PW_PERM_W, },
	{ &node_demarshal_send_command, PW_PERM_W, },
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
	PW_NODE_PROXY_METHOD_NUM,
	PW_NODE_PROXY_EVENT_NUM,
	&pw_protocol_native_node_method_marshal,
	pw_protocol_native_node_method_demarshal,
	&pw_protocol_native_node_event_marshal,
	pw_protocol_native_node_event_demarshal,
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
	PW_PORT_PROXY_METHOD_NUM,
	PW_PORT_PROXY_EVENT_NUM,
	&pw_protocol_native_port_method_marshal,
	pw_protocol_native_port_method_demarshal,
	&pw_protocol_native_port_event_marshal,
	pw_protocol_native_port_event_demarshal,
};

static const struct pw_client_proxy_methods pw_protocol_native_client_method_marshal = {
	PW_VERSION_CLIENT_PROXY_METHODS,
	&client_marshal_error,
	&client_marshal_update_properties,
	&client_marshal_get_permissions,
	&client_marshal_update_permissions,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_client_method_demarshal[] = {
	{ &client_demarshal_error, PW_PERM_W, },
	{ &client_demarshal_update_properties, PW_PERM_W, },
	{ &client_demarshal_get_permissions, 0, },
	{ &client_demarshal_update_permissions, PW_PERM_W, },
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
	PW_CLIENT_PROXY_METHOD_NUM,
	PW_CLIENT_PROXY_EVENT_NUM,
	&pw_protocol_native_client_method_marshal,
	pw_protocol_native_client_method_demarshal,
	&pw_protocol_native_client_event_marshal,
	pw_protocol_native_client_event_demarshal,
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
	0,
	PW_LINK_PROXY_EVENT_NUM,
	NULL, NULL,
	&pw_protocol_native_link_event_marshal,
	pw_protocol_native_link_event_demarshal,
};

void pw_protocol_native_init(struct pw_protocol *protocol)
{
	pw_protocol_add_marshal(protocol, &pw_protocol_native_core_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_registry_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_module_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_device_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_node_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_port_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_factory_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_client_marshal);
	pw_protocol_add_marshal(protocol, &pw_protocol_native_link_marshal);
}
