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


#include "spa/pod-iter.h"

#include "pipewire/client/interfaces.h"
#include "pipewire/server/resource.h"
#include "pipewire/server/protocol-native.h"

/** \cond */
typedef bool(*demarshal_func_t) (void *object, void *data, size_t size);

struct builder {
	struct spa_pod_builder b;
	struct pw_connection *connection;
};
/** \endcond */

static uint32_t write_pod(struct spa_pod_builder *b, uint32_t ref, const void *data, uint32_t size)
{
	if (ref == -1)
		ref = b->offset;

	if (b->size <= b->offset) {
		b->size = SPA_ROUND_UP_N(b->offset + size, 4096);
		b->data = pw_connection_begin_write(((struct builder *) b)->connection, b->size);
	}
	memcpy(b->data + ref, data, size);
	return ref;
}

static void core_update_map(struct pw_client *client)
{
	uint32_t diff, base, i;
	struct pw_core *core = client->core;
	const char **types;

	base = client->n_types;
	diff = spa_type_map_get_size(core->type.map) - base;
	if (diff == 0)
		return;

	types = alloca(diff * sizeof(char *));
	for (i = 0; i < diff; i++, base++)
		types[i] = spa_type_map_get_type(core->type.map, base);

	pw_core_notify_update_types(client->core_resource, client->n_types, diff, types);
	client->n_types += diff;
}

static void core_marshal_info(void *object, struct pw_core_info *info)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;
	uint32_t i, n_items;

	core_update_map(resource->client);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(&b.b,
			    SPA_POD_TYPE_STRUCT, &f,
			    SPA_POD_TYPE_INT, info->id,
			    SPA_POD_TYPE_LONG, info->change_mask,
			    SPA_POD_TYPE_STRING, info->user_name,
			    SPA_POD_TYPE_STRING, info->host_name,
			    SPA_POD_TYPE_STRING, info->version,
			    SPA_POD_TYPE_STRING, info->name,
			    SPA_POD_TYPE_INT, info->cookie, SPA_POD_TYPE_INT, n_items, 0);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(&b.b,
				    SPA_POD_TYPE_STRING, info->props->items[i].key,
				    SPA_POD_TYPE_STRING, info->props->items[i].value, 0);
	}
	spa_pod_builder_add(&b.b, -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_connection_end_write(connection, resource->id, PW_CORE_EVENT_INFO, b.b.offset);
}

static void core_marshal_done(void *object, uint32_t seq)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;

	core_update_map(resource->client);

	spa_pod_builder_struct(&b.b, &f, SPA_POD_TYPE_INT, seq);

	pw_connection_end_write(connection, resource->id, PW_CORE_EVENT_DONE, b.b.offset);
}

static void core_marshal_error(void *object, uint32_t id, int res, const char *error, ...)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	char buffer[128];
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;
	va_list ap;

	core_update_map(resource->client);

	va_start(ap, error);
	vsnprintf(buffer, sizeof(buffer), error, ap);
	va_end(ap);

	spa_pod_builder_struct(&b.b, &f,
			       SPA_POD_TYPE_INT, id,
			       SPA_POD_TYPE_INT, res, SPA_POD_TYPE_STRING, buffer);

	pw_connection_end_write(connection, resource->id, PW_CORE_EVENT_ERROR, b.b.offset);
}

static void core_marshal_remove_id(void *object, uint32_t id)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;

	core_update_map(resource->client);

	spa_pod_builder_struct(&b.b, &f, SPA_POD_TYPE_INT, id);

	pw_connection_end_write(connection, resource->id, PW_CORE_EVENT_REMOVE_ID, b.b.offset);
}

static void
core_marshal_update_types(void *object, uint32_t first_id, uint32_t n_types, const char **types)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;
	uint32_t i;

	spa_pod_builder_add(&b.b,
			    SPA_POD_TYPE_STRUCT, &f,
			    SPA_POD_TYPE_INT, first_id, SPA_POD_TYPE_INT, n_types, 0);

	for (i = 0; i < n_types; i++) {
		spa_pod_builder_add(&b.b, SPA_POD_TYPE_STRING, types[i], 0);
	}
	spa_pod_builder_add(&b.b, -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_connection_end_write(connection, resource->id, PW_CORE_EVENT_UPDATE_TYPES, b.b.offset);
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
	((struct pw_core_methods *) resource->implementation)->client_update(resource, &props);
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

	((struct pw_core_methods *) resource->implementation)->sync(resource, seq);
	return true;
}

static bool core_demarshal_get_registry(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_iter it;
	int32_t new_id;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it, SPA_POD_TYPE_INT, &new_id, 0))
		return false;

	((struct pw_core_methods *) resource->implementation)->get_registry(resource, new_id);
	return true;
}

static bool core_demarshal_create_node(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_iter it;
	uint32_t new_id, i;
	const char *factory_name, *name;
	struct spa_dict props;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_STRING, &factory_name,
			      SPA_POD_TYPE_STRING, &name, SPA_POD_TYPE_INT, &props.n_items, 0))
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

	((struct pw_core_methods *) resource->implementation)->create_node(resource,
									   factory_name,
									   name, &props, new_id);
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
	    !pw_pod_remap_data(SPA_POD_TYPE_STRUCT, data, size, &resource->client->types) ||
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

	((struct pw_core_methods *) resource->implementation)->create_link(resource,
									   output_node_id,
									   output_port_id,
									   input_node_id,
									   input_port_id,
									   filter,
									   &props,
									   new_id);
	return true;
}

static bool core_demarshal_update_types(void *object, void *data, size_t size)
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
	((struct pw_core_methods *) resource->implementation)->update_types(resource, first_id,
									    n_types, types);
	return true;
}

static void registry_marshal_global(void *object, uint32_t id, const char *type, uint32_t version)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;

	core_update_map(resource->client);

	spa_pod_builder_struct(&b.b, &f,
			       SPA_POD_TYPE_INT, id,
			       SPA_POD_TYPE_STRING, type,
			       SPA_POD_TYPE_INT, version);

	pw_connection_end_write(connection, resource->id, PW_REGISTRY_EVENT_GLOBAL, b.b.offset);
}

static void registry_marshal_global_remove(void *object, uint32_t id)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;

	core_update_map(resource->client);

	spa_pod_builder_struct(&b.b, &f, SPA_POD_TYPE_INT, id);

	pw_connection_end_write(connection, resource->id, PW_REGISTRY_EVENT_GLOBAL_REMOVE,
				b.b.offset);
}

static bool registry_demarshal_bind(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_iter it;
	uint32_t id, version, new_id;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &id,
			      SPA_POD_TYPE_INT, &version,
			      SPA_POD_TYPE_INT, &new_id, 0))
		return false;

	((struct pw_registry_methods *) resource->implementation)->bind(resource, id, version, new_id);
	return true;
}

static void module_marshal_info(void *object, struct pw_module_info *info)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;
	uint32_t i, n_items;

	core_update_map(resource->client);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(&b.b,
			    SPA_POD_TYPE_STRUCT, &f,
			    SPA_POD_TYPE_INT, info->id,
			    SPA_POD_TYPE_LONG, info->change_mask,
			    SPA_POD_TYPE_STRING, info->name,
			    SPA_POD_TYPE_STRING, info->filename,
			    SPA_POD_TYPE_STRING, info->args, SPA_POD_TYPE_INT, n_items, 0);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(&b.b,
				    SPA_POD_TYPE_STRING, info->props->items[i].key,
				    SPA_POD_TYPE_STRING, info->props->items[i].value, 0);
	}
	spa_pod_builder_add(&b.b, -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_connection_end_write(connection, resource->id, PW_MODULE_EVENT_INFO, b.b.offset);
}

static void node_marshal_info(void *object, struct pw_node_info *info)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;
	uint32_t i, n_items;

	core_update_map(resource->client);

	spa_pod_builder_add(&b.b,
			    SPA_POD_TYPE_STRUCT, &f,
			    SPA_POD_TYPE_INT, info->id,
			    SPA_POD_TYPE_LONG, info->change_mask,
			    SPA_POD_TYPE_STRING, info->name,
			    SPA_POD_TYPE_INT, info->max_input_ports,
			    SPA_POD_TYPE_INT, info->n_input_ports,
			    SPA_POD_TYPE_INT, info->n_input_formats, 0);

	for (i = 0; i < info->n_input_formats; i++)
		spa_pod_builder_add(&b.b, SPA_POD_TYPE_POD, info->input_formats[i], 0);

	spa_pod_builder_add(&b.b,
			    SPA_POD_TYPE_INT, info->max_output_ports,
			    SPA_POD_TYPE_INT, info->n_output_ports,
			    SPA_POD_TYPE_INT, info->n_output_formats, 0);

	for (i = 0; i < info->n_output_formats; i++)
		spa_pod_builder_add(&b.b, SPA_POD_TYPE_POD, info->output_formats[i], 0);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(&b.b,
			    SPA_POD_TYPE_INT, info->state,
			    SPA_POD_TYPE_STRING, info->error, SPA_POD_TYPE_INT, n_items, 0);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(&b.b,
				    SPA_POD_TYPE_STRING, info->props->items[i].key,
				    SPA_POD_TYPE_STRING, info->props->items[i].value, 0);
	}
	spa_pod_builder_add(&b.b, -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_connection_end_write(connection, resource->id, PW_NODE_EVENT_INFO, b.b.offset);
}

static void client_marshal_info(void *object, struct pw_client_info *info)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;
	uint32_t i, n_items;

	core_update_map(resource->client);

	n_items = info->props ? info->props->n_items : 0;

	spa_pod_builder_add(&b.b,
			    SPA_POD_TYPE_STRUCT, &f,
			    SPA_POD_TYPE_INT, info->id,
			    SPA_POD_TYPE_LONG, info->change_mask, SPA_POD_TYPE_INT, n_items, 0);

	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(&b.b,
				    SPA_POD_TYPE_STRING, info->props->items[i].key,
				    SPA_POD_TYPE_STRING, info->props->items[i].value, 0);
	}
	spa_pod_builder_add(&b.b, -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_connection_end_write(connection, resource->id, PW_CLIENT_EVENT_INFO, b.b.offset);
}

static void
client_node_marshal_set_props(void *object, uint32_t seq, const struct spa_props *props)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;

	core_update_map(resource->client);

	spa_pod_builder_struct(&b.b, &f,
			       SPA_POD_TYPE_INT, seq,
			       SPA_POD_TYPE_POD, props);

	pw_connection_end_write(connection, resource->id, PW_CLIENT_NODE_EVENT_SET_PROPS,
				b.b.offset);
}

static void client_node_marshal_event(void *object, const struct spa_event *event)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;

	core_update_map(resource->client);

	spa_pod_builder_struct(&b.b, &f, SPA_POD_TYPE_POD, event);

	pw_connection_end_write(connection, resource->id, PW_CLIENT_NODE_EVENT_EVENT, b.b.offset);
}

static void
client_node_marshal_add_port(void *object,
			     uint32_t seq, enum spa_direction direction, uint32_t port_id)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;

	core_update_map(resource->client);

	spa_pod_builder_struct(&b.b, &f,
			       SPA_POD_TYPE_INT, seq,
			       SPA_POD_TYPE_INT, direction, SPA_POD_TYPE_INT, port_id);

	pw_connection_end_write(connection, resource->id, PW_CLIENT_NODE_EVENT_ADD_PORT,
				b.b.offset);
}

static void
client_node_marshal_remove_port(void *object,
				uint32_t seq, enum spa_direction direction, uint32_t port_id)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;

	core_update_map(resource->client);

	spa_pod_builder_struct(&b.b, &f,
			       SPA_POD_TYPE_INT, seq,
			       SPA_POD_TYPE_INT, direction, SPA_POD_TYPE_INT, port_id);

	pw_connection_end_write(connection, resource->id, PW_CLIENT_NODE_EVENT_REMOVE_PORT,
				b.b.offset);
}

static void
client_node_marshal_set_format(void *object,
			       uint32_t seq,
			       enum spa_direction direction,
			       uint32_t port_id,
			       uint32_t flags,
			       const struct spa_format *format)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;

	core_update_map(resource->client);

	spa_pod_builder_struct(&b.b, &f,
			       SPA_POD_TYPE_INT, seq,
			       SPA_POD_TYPE_INT, direction,
			       SPA_POD_TYPE_INT, port_id,
			       SPA_POD_TYPE_INT, flags,
			       SPA_POD_TYPE_POD, format);

	pw_connection_end_write(connection, resource->id, PW_CLIENT_NODE_EVENT_SET_FORMAT,
				b.b.offset);
}

static void
client_node_marshal_set_param(void *object,
			      uint32_t seq,
			      enum spa_direction direction,
			      uint32_t port_id,
			      const struct spa_param *param)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;

	core_update_map(resource->client);

	spa_pod_builder_struct(&b.b, &f,
			       SPA_POD_TYPE_INT, seq,
			       SPA_POD_TYPE_INT, direction,
			       SPA_POD_TYPE_INT, port_id,
			       SPA_POD_TYPE_POD, param);

	pw_connection_end_write(connection, resource->id, PW_CLIENT_NODE_EVENT_SET_PARAM,
				b.b.offset);
}

static void
client_node_marshal_add_mem(void *object,
			    enum spa_direction direction,
			    uint32_t port_id,
			    uint32_t mem_id,
			    uint32_t type,
			    int memfd, uint32_t flags, uint32_t offset, uint32_t size)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;

	core_update_map(resource->client);

	spa_pod_builder_struct(&b.b, &f,
			       SPA_POD_TYPE_INT, direction,
			       SPA_POD_TYPE_INT, port_id,
			       SPA_POD_TYPE_INT, mem_id,
			       SPA_POD_TYPE_ID, type,
			       SPA_POD_TYPE_INT, pw_connection_add_fd(connection, memfd),
			       SPA_POD_TYPE_INT, flags,
			       SPA_POD_TYPE_INT, offset, SPA_POD_TYPE_INT, size);

	pw_connection_end_write(connection, resource->id, PW_CLIENT_NODE_EVENT_ADD_MEM, b.b.offset);
}

static void
client_node_marshal_use_buffers(void *object,
				uint32_t seq,
				enum spa_direction direction,
				uint32_t port_id,
				uint32_t n_buffers, struct pw_client_node_buffer *buffers)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;
	uint32_t i, j;

	core_update_map(resource->client);

	spa_pod_builder_add(&b.b,
			    SPA_POD_TYPE_STRUCT, &f,
			    SPA_POD_TYPE_INT, seq,
			    SPA_POD_TYPE_INT, direction,
			    SPA_POD_TYPE_INT, port_id, SPA_POD_TYPE_INT, n_buffers, 0);

	for (i = 0; i < n_buffers; i++) {
		struct spa_buffer *buf = buffers[i].buffer;

		spa_pod_builder_add(&b.b,
				    SPA_POD_TYPE_INT, buffers[i].mem_id,
				    SPA_POD_TYPE_INT, buffers[i].offset,
				    SPA_POD_TYPE_INT, buffers[i].size,
				    SPA_POD_TYPE_INT, buf->id, SPA_POD_TYPE_INT, buf->n_metas, 0);

		for (j = 0; j < buf->n_metas; j++) {
			struct spa_meta *m = &buf->metas[j];
			spa_pod_builder_add(&b.b,
					    SPA_POD_TYPE_ID, m->type, SPA_POD_TYPE_INT, m->size, 0);
		}
		spa_pod_builder_add(&b.b, SPA_POD_TYPE_INT, buf->n_datas, 0);
		for (j = 0; j < buf->n_datas; j++) {
			struct spa_data *d = &buf->datas[j];
			spa_pod_builder_add(&b.b,
					    SPA_POD_TYPE_ID, d->type,
					    SPA_POD_TYPE_INT, SPA_PTR_TO_UINT32(d->data),
					    SPA_POD_TYPE_INT, d->flags,
					    SPA_POD_TYPE_INT, d->mapoffset,
					    SPA_POD_TYPE_INT, d->maxsize, 0);
		}
	}
	spa_pod_builder_add(&b.b, -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_connection_end_write(connection, resource->id, PW_CLIENT_NODE_EVENT_USE_BUFFERS,
				b.b.offset);
}

static void
client_node_marshal_node_command(void *object, uint32_t seq, const struct spa_command *command)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;

	core_update_map(resource->client);

	spa_pod_builder_struct(&b.b, &f, SPA_POD_TYPE_INT, seq, SPA_POD_TYPE_POD, command);

	pw_connection_end_write(connection, resource->id, PW_CLIENT_NODE_EVENT_NODE_COMMAND,
				b.b.offset);
}

static void
client_node_marshal_port_command(void *object,
				 uint32_t direction,
				 uint32_t port_id,
				 const struct spa_command *command)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;

	core_update_map(resource->client);

	spa_pod_builder_struct(&b.b, &f,
			       SPA_POD_TYPE_INT, direction,
			       SPA_POD_TYPE_INT, port_id,
			       SPA_POD_TYPE_POD, command);

	pw_connection_end_write(connection, resource->id, PW_CLIENT_NODE_EVENT_PORT_COMMAND,
				b.b.offset);
}

static void client_node_marshal_transport(void *object, int readfd, int writefd,
					  int memfd, uint32_t offset, uint32_t size)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;

	core_update_map(resource->client);

	spa_pod_builder_struct(&b.b, &f,
			       SPA_POD_TYPE_INT, pw_connection_add_fd(connection, readfd),
			       SPA_POD_TYPE_INT, pw_connection_add_fd(connection, writefd),
			       SPA_POD_TYPE_INT, pw_connection_add_fd(connection, memfd),
			       SPA_POD_TYPE_INT, offset, SPA_POD_TYPE_INT, size);

	pw_connection_end_write(connection, resource->id, PW_CLIENT_NODE_EVENT_TRANSPORT,
				b.b.offset);
}


static bool client_node_demarshal_done(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_iter it;
	uint32_t seq, res;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &seq,
			      SPA_POD_TYPE_INT, &res, 0))
		return false;

	((struct pw_client_node_methods *) resource->implementation)->done(resource, seq, res);
	return true;
}

static bool client_node_demarshal_update(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_iter it;
	uint32_t change_mask, max_input_ports, max_output_ports;
	const struct spa_props *props;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !pw_pod_remap_data(SPA_POD_TYPE_STRUCT, data, size, &resource->client->types) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &change_mask,
			      SPA_POD_TYPE_INT, &max_input_ports,
			      SPA_POD_TYPE_INT, &max_output_ports, -SPA_POD_TYPE_OBJECT, &props, 0))
		return false;

	((struct pw_client_node_methods *) resource->implementation)->update(resource, change_mask,
									     max_input_ports,
									     max_output_ports,
									     props);
	return true;
}

static bool client_node_demarshal_port_update(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_iter it;
	uint32_t i, direction, port_id, change_mask, n_possible_formats, n_params;
	const struct spa_param **params = NULL;
	const struct spa_format **possible_formats = NULL, *format = NULL;
	struct spa_port_info info, *infop = NULL;
	struct spa_pod *ipod;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !pw_pod_remap_data(SPA_POD_TYPE_STRUCT, data, size, &resource->client->types) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &direction,
			      SPA_POD_TYPE_INT, &port_id,
			      SPA_POD_TYPE_INT, &change_mask,
			      SPA_POD_TYPE_INT, &n_possible_formats, 0))
		return false;

	possible_formats = alloca(n_possible_formats * sizeof(struct spa_format *));
	for (i = 0; i < n_possible_formats; i++)
		if (!spa_pod_iter_get(&it, SPA_POD_TYPE_OBJECT, &possible_formats[i], 0))
			return false;

	if (!spa_pod_iter_get(&it, -SPA_POD_TYPE_OBJECT, &format, SPA_POD_TYPE_INT, &n_params, 0))
		return false;

	params = alloca(n_params * sizeof(struct spa_param *));
	for (i = 0; i < n_params; i++)
		if (!spa_pod_iter_get(&it, SPA_POD_TYPE_OBJECT, &params[i], 0))
			return false;

	if (!spa_pod_iter_get(&it, -SPA_POD_TYPE_STRUCT, &ipod, 0))
		return false;

	if (ipod) {
		struct spa_pod_iter it2;
		infop = &info;

		if (!spa_pod_iter_pod(&it2, ipod) ||
		    !spa_pod_iter_get(&it2,
				      SPA_POD_TYPE_INT, &info.flags,
				      SPA_POD_TYPE_INT, &info.rate, 0))
			return false;
	}

	((struct pw_client_node_methods *) resource->implementation)->port_update(resource,
										  direction,
										  port_id,
										  change_mask,
										  n_possible_formats,
										  possible_formats,
										  format,
										  n_params,
										  params, infop);
	return true;
}

static bool client_node_demarshal_event(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_iter it;
	struct spa_event *event;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !pw_pod_remap_data(SPA_POD_TYPE_STRUCT, data, size, &resource->client->types) ||
	    !spa_pod_iter_get(&it, SPA_POD_TYPE_OBJECT, &event, 0))
		return false;

	((struct pw_client_node_methods *) resource->implementation)->event(resource, event);
	return true;
}

static bool client_node_demarshal_destroy(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_iter it;

	if (!spa_pod_iter_struct(&it, data, size))
		return false;

	((struct pw_client_node_methods *) resource->implementation)->destroy(resource);
	return true;
}

static void link_marshal_info(void *object, struct pw_link_info *info)
{
	struct pw_resource *resource = object;
	struct pw_connection *connection = resource->client->protocol_private;
	struct builder b = { {NULL, 0, 0, NULL, write_pod}, connection };
	struct spa_pod_frame f;

	core_update_map(resource->client);

	spa_pod_builder_struct(&b.b, &f,
			       SPA_POD_TYPE_INT, info->id,
			       SPA_POD_TYPE_LONG, info->change_mask,
			       SPA_POD_TYPE_INT, info->output_node_id,
			       SPA_POD_TYPE_INT, info->output_port_id,
			       SPA_POD_TYPE_INT, info->input_node_id,
			       SPA_POD_TYPE_INT, info->input_port_id,
			       SPA_POD_TYPE_POD, info->format);

	pw_connection_end_write(connection, resource->id, PW_LINK_EVENT_INFO, b.b.offset);
}

static const demarshal_func_t pw_protocol_native_server_core_demarshal[PW_CORE_METHOD_NUM] = {
	&core_demarshal_update_types,
	&core_demarshal_sync,
	&core_demarshal_get_registry,
	&core_demarshal_client_update,
	&core_demarshal_create_node,
	&core_demarshal_create_link
};

static const struct pw_core_events pw_protocol_native_server_core_events = {
	&core_marshal_update_types,
	&core_marshal_done,
	&core_marshal_error,
	&core_marshal_remove_id,
	&core_marshal_info
};

const struct pw_interface pw_protocol_native_server_core_interface = {
	PIPEWIRE_TYPE__Core,
	PW_VERSION_CORE,
	PW_CORE_METHOD_NUM, pw_protocol_native_server_core_demarshal,
	PW_CORE_EVENT_NUM, &pw_protocol_native_server_core_events,
};

static const demarshal_func_t pw_protocol_native_server_registry_demarshal[] = {
	&registry_demarshal_bind,
};

static const struct pw_registry_events pw_protocol_native_server_registry_events = {
	&registry_marshal_global,
	&registry_marshal_global_remove,
};

const struct pw_interface pw_protocol_native_server_registry_interface = {
	PIPEWIRE_TYPE__Registry,
	PW_VERSION_REGISTRY,
	PW_REGISTRY_METHOD_NUM, pw_protocol_native_server_registry_demarshal,
	PW_REGISTRY_EVENT_NUM, &pw_protocol_native_server_registry_events,
};

static const struct pw_module_events pw_protocol_native_server_module_events = {
	&module_marshal_info,
};

const struct pw_interface pw_protocol_native_server_module_interface = {
	PIPEWIRE_TYPE__Module,
	PW_VERSION_MODULE,
	0, NULL,
	PW_MODULE_EVENT_NUM, &pw_protocol_native_server_module_events,
};

static const struct pw_node_events pw_protocol_native_server_node_events = {
	&node_marshal_info,
};

const struct pw_interface pw_protocol_native_server_node_interface = {
	PIPEWIRE_TYPE__Node,
	PW_VERSION_NODE,
	0, NULL,
	PW_NODE_EVENT_NUM, &pw_protocol_native_server_node_events,
};

static const struct pw_client_events pw_protocol_native_server_client_events = {
	&client_marshal_info,
};

const struct pw_interface pw_protocol_native_server_client_interface = {
	PIPEWIRE_TYPE__Client,
	PW_VERSION_CLIENT,
	0, NULL,
	PW_CLIENT_EVENT_NUM, &pw_protocol_native_server_client_events,
};

static const demarshal_func_t pw_protocol_native_server_client_node_demarshal[] = {
	&client_node_demarshal_done,
	&client_node_demarshal_update,
	&client_node_demarshal_port_update,
	&client_node_demarshal_event,
	&client_node_demarshal_destroy,
};

static const struct pw_client_node_events pw_protocol_native_server_client_node_events = {
	&client_node_marshal_set_props,
	&client_node_marshal_event,
	&client_node_marshal_add_port,
	&client_node_marshal_remove_port,
	&client_node_marshal_set_format,
	&client_node_marshal_set_param,
	&client_node_marshal_add_mem,
	&client_node_marshal_use_buffers,
	&client_node_marshal_node_command,
	&client_node_marshal_port_command,
	&client_node_marshal_transport,
};

const struct pw_interface pw_protocol_native_server_client_node_interface = {
	PIPEWIRE_TYPE_NODE_BASE "Client",
	PW_VERSION_CLIENT_NODE,
	PW_CLIENT_NODE_METHOD_NUM, &pw_protocol_native_server_client_node_demarshal,
	PW_CLIENT_NODE_EVENT_NUM, &pw_protocol_native_server_client_node_events,
};

static const struct pw_link_events pw_protocol_native_server_link_events = {
	&link_marshal_info,
};

const struct pw_interface pw_protocol_native_server_link_interface = {
	PIPEWIRE_TYPE__Link,
	PW_VERSION_LINK,
	0, NULL,
	PW_LINK_EVENT_NUM, &pw_protocol_native_server_link_events,
};

void pw_protocol_native_server_init(void)
{
	static bool init = false;
	struct pw_protocol *protocol;

	if (init)
		return;

	protocol = pw_protocol_get(PW_TYPE_PROTOCOL__Native);

	pw_protocol_add_interfaces(protocol,
				   NULL,
				   &pw_protocol_native_server_core_interface);
	pw_protocol_add_interfaces(protocol,
				   NULL,
				   &pw_protocol_native_server_registry_interface);
	pw_protocol_add_interfaces(protocol,
				   NULL,
				   &pw_protocol_native_server_module_interface);
	pw_protocol_add_interfaces(protocol,
				   NULL,
				   &pw_protocol_native_server_node_interface);
	pw_protocol_add_interfaces(protocol,
				   NULL,
				   &pw_protocol_native_server_client_node_interface);
	pw_protocol_add_interfaces(protocol,
				   NULL,
				   &pw_protocol_native_server_client_interface);
	pw_protocol_add_interfaces(protocol,
				   NULL,
				   &pw_protocol_native_server_link_interface);

	init = true;
}
