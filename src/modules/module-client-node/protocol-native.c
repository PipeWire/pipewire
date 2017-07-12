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

#include <errno.h>

#include "spa/pod-iter.h"

#include "pipewire/pipewire.h"
#include "pipewire/interfaces.h"
#include "pipewire/protocol.h"
#include "pipewire/client.h"

#include "extensions/protocol-native.h"
#include "extensions/client-node.h"

/** \cond */

typedef bool(*demarshal_func_t) (void *object, void *data, size_t size);

/** \endcond */

static void
client_node_marshal_done(void *object, int seq, int res)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_NODE_METHOD_DONE);

	spa_pod_builder_struct(b, &f,
			       SPA_POD_TYPE_INT, seq,
			       SPA_POD_TYPE_INT, res);

	pw_protocol_native_end_proxy(proxy, b);
}


static void
client_node_marshal_update(void *object,
			   uint32_t change_mask,
			   uint32_t max_input_ports,
			   uint32_t max_output_ports, const struct spa_props *props)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_NODE_METHOD_UPDATE);

	spa_pod_builder_struct(b, &f,
			       SPA_POD_TYPE_INT, change_mask,
			       SPA_POD_TYPE_INT, max_input_ports,
			       SPA_POD_TYPE_INT, max_output_ports, SPA_POD_TYPE_POD, props);

	pw_protocol_native_end_proxy(proxy, b);
}

static void
client_node_marshal_port_update(void *object,
				enum spa_direction direction,
				uint32_t port_id,
				uint32_t change_mask,
				uint32_t n_possible_formats,
				const struct spa_format **possible_formats,
				const struct spa_format *format,
				uint32_t n_params,
				const struct spa_param **params, const struct spa_port_info *info)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f[2];
	int i;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_NODE_METHOD_PORT_UPDATE);

	spa_pod_builder_add(b,
			    SPA_POD_TYPE_STRUCT, &f[0],
			    SPA_POD_TYPE_INT, direction,
			    SPA_POD_TYPE_INT, port_id,
			    SPA_POD_TYPE_INT, change_mask, SPA_POD_TYPE_INT, n_possible_formats, 0);

	for (i = 0; i < n_possible_formats; i++)
		spa_pod_builder_add(b, SPA_POD_TYPE_POD, possible_formats[i], 0);

	spa_pod_builder_add(b, SPA_POD_TYPE_POD, format, SPA_POD_TYPE_INT, n_params, 0);

	for (i = 0; i < n_params; i++) {
		const struct spa_param *p = params[i];
		spa_pod_builder_add(b, SPA_POD_TYPE_POD, p, 0);
	}

	if (info) {
		spa_pod_builder_add(b,
				    SPA_POD_TYPE_STRUCT, &f[1],
				    SPA_POD_TYPE_INT, info->flags, SPA_POD_TYPE_INT, info->rate, 0);
		spa_pod_builder_add(b, -SPA_POD_TYPE_STRUCT, &f[1], 0);
	} else {
		spa_pod_builder_add(b, SPA_POD_TYPE_POD, NULL, 0);
	}
	spa_pod_builder_add(b, -SPA_POD_TYPE_STRUCT, &f[0], 0);

	pw_protocol_native_end_proxy(proxy, b);
}

static void client_node_marshal_event_method(void *object, struct spa_event *event)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_NODE_METHOD_EVENT);

	spa_pod_builder_struct(b, &f, SPA_POD_TYPE_POD, event);

	pw_protocol_native_end_proxy(proxy, b);
}

static void client_node_marshal_destroy(void *object)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_NODE_METHOD_DESTROY);

	spa_pod_builder_struct(b, &f, 0);

	pw_protocol_native_end_proxy(proxy, b);
}

static bool client_node_demarshal_set_props(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	uint32_t seq;
	const struct spa_props *props = NULL;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &seq,
			      -SPA_POD_TYPE_OBJECT, &props, 0))
		return false;

	((struct pw_client_node_events *) proxy->implementation)->set_props(proxy, seq, props);
	return true;
}

static bool client_node_demarshal_event_event(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	const struct spa_event *event;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !pw_pod_remap_data(SPA_POD_TYPE_STRUCT, data, size, &proxy->remote->types) ||
	    !spa_pod_iter_get(&it, SPA_POD_TYPE_OBJECT, &event, 0))
		return false;

	((struct pw_client_node_events *) proxy->implementation)->event(proxy, event);
	return true;
}

static bool client_node_demarshal_add_port(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	int32_t seq, direction, port_id;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &seq,
			      SPA_POD_TYPE_INT, &direction, SPA_POD_TYPE_INT, &port_id, 0))
		return false;

	((struct pw_client_node_events *) proxy->implementation)->add_port(proxy, seq, direction,
									   port_id);
	return true;
}

static bool client_node_demarshal_remove_port(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	int32_t seq, direction, port_id;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &seq,
			      SPA_POD_TYPE_INT, &direction, SPA_POD_TYPE_INT, &port_id, 0))
		return false;

	((struct pw_client_node_events *) proxy->implementation)->remove_port(proxy, seq, direction,
									      port_id);
	return true;
}

static bool client_node_demarshal_set_format(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	uint32_t seq, direction, port_id, flags;
	const struct spa_format *format = NULL;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !pw_pod_remap_data(SPA_POD_TYPE_STRUCT, data, size, &proxy->remote->types) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &seq,
			      SPA_POD_TYPE_INT, &direction,
			      SPA_POD_TYPE_INT, &port_id,
			      SPA_POD_TYPE_INT, &flags,
			      -SPA_POD_TYPE_OBJECT, &format, 0))
		return false;

	((struct pw_client_node_events *) proxy->implementation)->set_format(proxy, seq, direction,
									     port_id, flags,
									     format);
	return true;
}

static bool client_node_demarshal_set_param(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	uint32_t seq, direction, port_id;
	const struct spa_param *param = NULL;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !pw_pod_remap_data(SPA_POD_TYPE_STRUCT, data, size, &proxy->remote->types) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &seq,
			      SPA_POD_TYPE_INT, &direction,
			      SPA_POD_TYPE_INT, &port_id,
			      -SPA_POD_TYPE_OBJECT, &param, 0))
		return false;

	((struct pw_client_node_events *) proxy->implementation)->set_param(proxy, seq, direction,
									     port_id, param);
	return true;
}

static bool client_node_demarshal_add_mem(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	uint32_t direction, port_id, mem_id, type, memfd_idx, flags, offset, sz;
	int memfd;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !pw_pod_remap_data(SPA_POD_TYPE_STRUCT, data, size, &proxy->remote->types) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &direction,
			      SPA_POD_TYPE_INT, &port_id,
			      SPA_POD_TYPE_INT, &mem_id,
			      SPA_POD_TYPE_ID, &type,
			      SPA_POD_TYPE_INT, &memfd_idx,
			      SPA_POD_TYPE_INT, &flags,
			      SPA_POD_TYPE_INT, &offset, SPA_POD_TYPE_INT, &sz, 0))
		return false;

	memfd = pw_protocol_native_get_proxy_fd(proxy, memfd_idx);

	((struct pw_client_node_events *) proxy->implementation)->add_mem(proxy,
									  direction,
									  port_id,
									  mem_id,
									  type,
									  memfd, flags, offset, sz);
	return true;
}

static bool client_node_demarshal_use_buffers(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	uint32_t seq, direction, port_id, n_buffers, data_id;
	struct pw_client_node_buffer *buffers;
	int i, j;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !pw_pod_remap_data(SPA_POD_TYPE_STRUCT, data, size, &proxy->remote->types) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &seq,
			      SPA_POD_TYPE_INT, &direction,
			      SPA_POD_TYPE_INT, &port_id, SPA_POD_TYPE_INT, &n_buffers, 0))
		return false;

	buffers = alloca(sizeof(struct pw_client_node_buffer) * n_buffers);
	for (i = 0; i < n_buffers; i++) {
		struct spa_buffer *buf = buffers[i].buffer = alloca(sizeof(struct spa_buffer));

		if (!spa_pod_iter_get(&it,
				      SPA_POD_TYPE_INT, &buffers[i].mem_id,
				      SPA_POD_TYPE_INT, &buffers[i].offset,
				      SPA_POD_TYPE_INT, &buffers[i].size,
				      SPA_POD_TYPE_INT, &buf->id,
				      SPA_POD_TYPE_INT, &buf->n_metas, 0))
			return false;

		buf->metas = alloca(sizeof(struct spa_meta) * buf->n_metas);
		for (j = 0; j < buf->n_metas; j++) {
			struct spa_meta *m = &buf->metas[j];

			if (!spa_pod_iter_get(&it,
					      SPA_POD_TYPE_ID, &m->type,
					      SPA_POD_TYPE_INT, &m->size, 0))
				return false;
		}
		if (!spa_pod_iter_get(&it, SPA_POD_TYPE_INT, &buf->n_datas, 0))
			return false;

		buf->datas = alloca(sizeof(struct spa_data) * buf->n_datas);
		for (j = 0; j < buf->n_datas; j++) {
			struct spa_data *d = &buf->datas[j];

			if (!spa_pod_iter_get(&it,
					      SPA_POD_TYPE_ID, &d->type,
					      SPA_POD_TYPE_INT, &data_id,
					      SPA_POD_TYPE_INT, &d->flags,
					      SPA_POD_TYPE_INT, &d->mapoffset,
					      SPA_POD_TYPE_INT, &d->maxsize, 0))
				return false;

			d->data = SPA_UINT32_TO_PTR(data_id);
		}
	}
	((struct pw_client_node_events *) proxy->implementation)->use_buffers(proxy,
									      seq,
									      direction,
									      port_id,
									      n_buffers, buffers);
	return true;
}

static bool client_node_demarshal_node_command(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	const struct spa_command *command;
	uint32_t seq;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !pw_pod_remap_data(SPA_POD_TYPE_STRUCT, data, size, &proxy->remote->types) ||
	    !spa_pod_iter_get(&it, SPA_POD_TYPE_INT, &seq, SPA_POD_TYPE_OBJECT, &command, 0))
		return false;

	((struct pw_client_node_events *) proxy->implementation)->node_command(proxy, seq, command);
	return true;
}

static bool client_node_demarshal_port_command(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	const struct spa_command *command;
	uint32_t direction, port_id;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !pw_pod_remap_data(SPA_POD_TYPE_STRUCT, data, size, &proxy->remote->types) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &direction,
			      SPA_POD_TYPE_INT, &port_id,
			      SPA_POD_TYPE_OBJECT, &command, 0))
		return false;

	((struct pw_client_node_events *) proxy->implementation)->port_command(proxy,
									       direction,
									       port_id,
									       command);
	return true;
}

static bool client_node_demarshal_transport(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_iter it;
	uint32_t node_id, ridx, widx, memfd_idx, offset, sz;
	int readfd, writefd, memfd;

	if (!spa_pod_iter_struct(&it, data, size) ||
	    !spa_pod_iter_get(&it,
			      SPA_POD_TYPE_INT, &node_id,
			      SPA_POD_TYPE_INT, &ridx,
			      SPA_POD_TYPE_INT, &widx,
			      SPA_POD_TYPE_INT, &memfd_idx,
			      SPA_POD_TYPE_INT, &offset,
			      SPA_POD_TYPE_INT, &sz, 0))
		return false;

	readfd = pw_protocol_native_get_proxy_fd(proxy, ridx);
	writefd = pw_protocol_native_get_proxy_fd(proxy, widx);
	memfd = pw_protocol_native_get_proxy_fd(proxy, memfd_idx);
	if (readfd == -1 || writefd == -1 || memfd_idx == -1)
		return false;

	((struct pw_client_node_events *) proxy->implementation)->transport(proxy, node_id,
									    readfd, writefd,
									    memfd, offset, sz);
	return true;
}

static void
client_node_marshal_set_props(void *object, uint32_t seq, const struct spa_props *props)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_EVENT_SET_PROPS);

	spa_pod_builder_struct(b, &f,
			       SPA_POD_TYPE_INT, seq,
			       SPA_POD_TYPE_POD, props);

	pw_protocol_native_end_resource(resource, b);
}

static void client_node_marshal_event_event(void *object, const struct spa_event *event)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_EVENT_EVENT);

	spa_pod_builder_struct(b, &f, SPA_POD_TYPE_POD, event);

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_add_port(void *object,
			     uint32_t seq, enum spa_direction direction, uint32_t port_id)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_EVENT_ADD_PORT);

	spa_pod_builder_struct(b, &f,
			       SPA_POD_TYPE_INT, seq,
			       SPA_POD_TYPE_INT, direction, SPA_POD_TYPE_INT, port_id);

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_remove_port(void *object,
				uint32_t seq, enum spa_direction direction, uint32_t port_id)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_EVENT_REMOVE_PORT);

	spa_pod_builder_struct(b, &f,
			       SPA_POD_TYPE_INT, seq,
			       SPA_POD_TYPE_INT, direction, SPA_POD_TYPE_INT, port_id);

	pw_protocol_native_end_resource(resource, b);
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
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_EVENT_SET_FORMAT);

	spa_pod_builder_struct(b, &f,
			       SPA_POD_TYPE_INT, seq,
			       SPA_POD_TYPE_INT, direction,
			       SPA_POD_TYPE_INT, port_id,
			       SPA_POD_TYPE_INT, flags,
			       SPA_POD_TYPE_POD, format);

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_set_param(void *object,
			      uint32_t seq,
			      enum spa_direction direction,
			      uint32_t port_id,
			      const struct spa_param *param)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_EVENT_SET_PARAM);

	spa_pod_builder_struct(b, &f,
			       SPA_POD_TYPE_INT, seq,
			       SPA_POD_TYPE_INT, direction,
			       SPA_POD_TYPE_INT, port_id,
			       SPA_POD_TYPE_POD, param);

	pw_protocol_native_end_resource(resource, b);
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
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_EVENT_ADD_MEM);

	spa_pod_builder_struct(b, &f,
			       SPA_POD_TYPE_INT, direction,
			       SPA_POD_TYPE_INT, port_id,
			       SPA_POD_TYPE_INT, mem_id,
			       SPA_POD_TYPE_ID, type,
			       SPA_POD_TYPE_INT, pw_protocol_native_add_resource_fd(resource, memfd),
			       SPA_POD_TYPE_INT, flags,
			       SPA_POD_TYPE_INT, offset, SPA_POD_TYPE_INT, size);

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_use_buffers(void *object,
				uint32_t seq,
				enum spa_direction direction,
				uint32_t port_id,
				uint32_t n_buffers, struct pw_client_node_buffer *buffers)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;
	uint32_t i, j;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_EVENT_USE_BUFFERS);

	spa_pod_builder_add(b,
			    SPA_POD_TYPE_STRUCT, &f,
			    SPA_POD_TYPE_INT, seq,
			    SPA_POD_TYPE_INT, direction,
			    SPA_POD_TYPE_INT, port_id, SPA_POD_TYPE_INT, n_buffers, 0);

	for (i = 0; i < n_buffers; i++) {
		struct spa_buffer *buf = buffers[i].buffer;

		spa_pod_builder_add(b,
				    SPA_POD_TYPE_INT, buffers[i].mem_id,
				    SPA_POD_TYPE_INT, buffers[i].offset,
				    SPA_POD_TYPE_INT, buffers[i].size,
				    SPA_POD_TYPE_INT, buf->id, SPA_POD_TYPE_INT, buf->n_metas, 0);

		for (j = 0; j < buf->n_metas; j++) {
			struct spa_meta *m = &buf->metas[j];
			spa_pod_builder_add(b,
					    SPA_POD_TYPE_ID, m->type, SPA_POD_TYPE_INT, m->size, 0);
		}
		spa_pod_builder_add(b, SPA_POD_TYPE_INT, buf->n_datas, 0);
		for (j = 0; j < buf->n_datas; j++) {
			struct spa_data *d = &buf->datas[j];
			spa_pod_builder_add(b,
					    SPA_POD_TYPE_ID, d->type,
					    SPA_POD_TYPE_INT, SPA_PTR_TO_UINT32(d->data),
					    SPA_POD_TYPE_INT, d->flags,
					    SPA_POD_TYPE_INT, d->mapoffset,
					    SPA_POD_TYPE_INT, d->maxsize, 0);
		}
	}
	spa_pod_builder_add(b, -SPA_POD_TYPE_STRUCT, &f, 0);

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_node_command(void *object, uint32_t seq, const struct spa_command *command)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_EVENT_NODE_COMMAND);

	spa_pod_builder_struct(b, &f, SPA_POD_TYPE_INT, seq, SPA_POD_TYPE_POD, command);

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_port_command(void *object,
				 uint32_t direction,
				 uint32_t port_id,
				 const struct spa_command *command)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_EVENT_PORT_COMMAND);

	spa_pod_builder_struct(b, &f,
			       SPA_POD_TYPE_INT, direction,
			       SPA_POD_TYPE_INT, port_id,
			       SPA_POD_TYPE_POD, command);

	pw_protocol_native_end_resource(resource, b);
}

static void client_node_marshal_transport(void *object, uint32_t node_id, int readfd, int writefd,
					  int memfd, uint32_t offset, uint32_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct spa_pod_frame f;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_EVENT_TRANSPORT);

	spa_pod_builder_struct(b, &f,
			       SPA_POD_TYPE_INT, node_id,
			       SPA_POD_TYPE_INT, pw_protocol_native_add_resource_fd(resource, readfd),
			       SPA_POD_TYPE_INT, pw_protocol_native_add_resource_fd(resource, writefd),
			       SPA_POD_TYPE_INT, pw_protocol_native_add_resource_fd(resource, memfd),
			       SPA_POD_TYPE_INT, offset, SPA_POD_TYPE_INT, size);

	pw_protocol_native_end_resource(resource, b);
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

static bool client_node_demarshal_event_method(void *object, void *data, size_t size)
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

static const struct pw_client_node_methods pw_protocol_native_client_client_node_methods = {
	&client_node_marshal_done,
	&client_node_marshal_update,
	&client_node_marshal_port_update,
	&client_node_marshal_event_method,
	&client_node_marshal_destroy
};

static const demarshal_func_t pw_protocol_native_client_client_node_demarshal[] = {
	&client_node_demarshal_transport,
	&client_node_demarshal_set_props,
	&client_node_demarshal_event_event,
	&client_node_demarshal_add_port,
	&client_node_demarshal_remove_port,
	&client_node_demarshal_set_format,
	&client_node_demarshal_set_param,
	&client_node_demarshal_add_mem,
	&client_node_demarshal_use_buffers,
	&client_node_demarshal_node_command,
	&client_node_demarshal_port_command,
};

static const struct pw_interface pw_protocol_native_client_client_node_interface = {
	PW_TYPE__ClientNode,
	PW_VERSION_CLIENT_NODE,
	PW_CLIENT_NODE_METHOD_NUM, &pw_protocol_native_client_client_node_methods,
	PW_CLIENT_NODE_EVENT_NUM, pw_protocol_native_client_client_node_demarshal,
};

static const demarshal_func_t pw_protocol_native_server_client_node_demarshal[] = {
	&client_node_demarshal_done,
	&client_node_demarshal_update,
	&client_node_demarshal_port_update,
	&client_node_demarshal_event_method,
	&client_node_demarshal_destroy,
};

static const struct pw_client_node_events pw_protocol_native_server_client_node_events = {
	&client_node_marshal_transport,
	&client_node_marshal_set_props,
	&client_node_marshal_event_event,
	&client_node_marshal_add_port,
	&client_node_marshal_remove_port,
	&client_node_marshal_set_format,
	&client_node_marshal_set_param,
	&client_node_marshal_add_mem,
	&client_node_marshal_use_buffers,
	&client_node_marshal_node_command,
	&client_node_marshal_port_command,
};

const struct pw_interface pw_protocol_native_server_client_node_interface = {
	PW_TYPE__ClientNode,
	PW_VERSION_CLIENT_NODE,
	PW_CLIENT_NODE_METHOD_NUM, &pw_protocol_native_server_client_node_demarshal,
	PW_CLIENT_NODE_EVENT_NUM, &pw_protocol_native_server_client_node_events,
};

struct pw_protocol *pw_protocol_native_ext_client_node_init(struct pw_core *core)
{
	struct pw_protocol *protocol;

	protocol = pw_core_find_protocol(core, PW_TYPE_PROTOCOL__Native);

	if (protocol == NULL)
		return NULL;

	pw_protocol_add_interfaces(protocol,
				   &pw_protocol_native_client_client_node_interface,
				   &pw_protocol_native_server_client_node_interface);

	return protocol;
}
