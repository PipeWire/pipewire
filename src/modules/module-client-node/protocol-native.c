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

#include <spa/pod/parser.h>

#include "pipewire/pipewire.h"
#include "pipewire/interfaces.h"
#include "pipewire/protocol.h"
#include "pipewire/client.h"

#include "extensions/protocol-native.h"
#include "extensions/client-node.h"

#include "transport.h"

static void
client_node_marshal_done(void *object, int seq, int res)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_NODE_PROXY_METHOD_DONE);

	spa_pod_builder_struct(b,
			       "i", seq,
			       "i", res);

	pw_protocol_native_end_proxy(proxy, b);
}


static void
client_node_marshal_update(void *object,
			   uint32_t change_mask,
			   uint32_t max_input_ports,
			   uint32_t max_output_ports,
			   uint32_t n_params,
			   const struct spa_pod **params)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	int i;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_NODE_PROXY_METHOD_UPDATE);

	spa_pod_builder_add(b,
			    "[",
			    "i", change_mask,
			    "i", max_input_ports,
			    "i", max_output_ports,
			    "i", n_params, NULL);

	for (i = 0; i < n_params; i++)
		spa_pod_builder_add(b, "P", params[i], NULL);

	spa_pod_builder_add(b, "]", NULL);

	pw_protocol_native_end_proxy(proxy, b);
}

static void
client_node_marshal_port_update(void *object,
				enum spa_direction direction,
				uint32_t port_id,
				uint32_t change_mask,
				uint32_t n_params,
				const struct spa_pod **params,
				const struct spa_port_info *info)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	int i, n_items;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_NODE_PROXY_METHOD_PORT_UPDATE);

	spa_pod_builder_add(b,
			    "[",
			    "i", direction,
			    "i", port_id,
			    "i", change_mask,
			    "i", n_params, NULL);

	for (i = 0; i < n_params; i++)
		spa_pod_builder_add(b, "P", params[i], NULL);

	if (info) {
		n_items = info->props ? info->props->n_items : 0;

		spa_pod_builder_add(b,
				    "[",
				    "i", info->flags,
				    "i", info->rate,
				    "i", n_items, NULL);
		for (i = 0; i < n_items; i++) {
			spa_pod_builder_add(b,
					    "s", info->props->items[i].key,
					    "s", info->props->items[i].value, NULL);
		}
		spa_pod_builder_add(b, "]", NULL);
	} else {
		spa_pod_builder_add(b, "P", NULL, NULL);
	}
	spa_pod_builder_add(b, "]", NULL);

	pw_protocol_native_end_proxy(proxy, b);
}

static void client_node_marshal_set_active(void *object, bool active)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_NODE_PROXY_METHOD_SET_ACTIVE);

	spa_pod_builder_struct(b, "b", active);

	pw_protocol_native_end_proxy(proxy, b);
}

static void client_node_marshal_event_method(void *object, struct spa_event *event)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_NODE_PROXY_METHOD_EVENT);

	spa_pod_builder_struct(b, "P", event);

	pw_protocol_native_end_proxy(proxy, b);
}

static void client_node_marshal_destroy(void *object)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_NODE_PROXY_METHOD_DESTROY);

	spa_pod_builder_struct(b);

	pw_protocol_native_end_proxy(proxy, b);
}

static int client_node_demarshal_add_mem(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t mem_id, type, memfd_idx, flags;
	int memfd;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &mem_id,
			"I", &type,
			"i", &memfd_idx,
			"i", &flags, NULL) < 0)
		return -EINVAL;

	memfd = pw_protocol_native_get_proxy_fd(proxy, memfd_idx);

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, add_mem,
								      mem_id,
								      type,
								      memfd, flags);
	return 0;
}

static int client_node_demarshal_transport(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t node_id, ridx, widx, memfd_idx;
	int readfd, writefd;
	struct pw_client_node_transport_info info;
	struct pw_client_node_transport *transport;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &node_id,
			"i", &ridx,
			"i", &widx,
			"i", &memfd_idx,
			"i", &info.offset,
			"i", &info.size, NULL) < 0)
		return -EINVAL;

	readfd = pw_protocol_native_get_proxy_fd(proxy, ridx);
	writefd = pw_protocol_native_get_proxy_fd(proxy, widx);
	info.memfd = pw_protocol_native_get_proxy_fd(proxy, memfd_idx);

	if (readfd == -1 || writefd == -1 || info.memfd == -1)
		return -EINVAL;

	transport = pw_client_node_transport_new_from_info(&info);

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, transport, node_id,
								   readfd, writefd, transport);
	return 0;
}

static int client_node_demarshal_set_param(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t seq, id, flags;
	const struct spa_pod *param = NULL;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &seq,
			"I", &id,
			"i", &flags,
			"O", &param, NULL) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, set_param, seq, id, flags, param);
	return 0;
}

static int client_node_demarshal_event_event(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	const struct spa_event *event;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs, "[ O", &event, NULL) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, event, event);
	return 0;
}

static int client_node_demarshal_command(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	const struct spa_command *command;
	uint32_t seq;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &seq,
			"O", &command, NULL) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, command, seq, command);
	return 0;
}

static int client_node_demarshal_add_port(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	int32_t seq, direction, port_id;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &seq,
			"i", &direction,
			"i", &port_id, NULL) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, add_port, seq, direction, port_id);
	return 0;
}

static int client_node_demarshal_remove_port(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	int32_t seq, direction, port_id;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &seq,
			"i", &direction,
			"i", &port_id, NULL) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, remove_port, seq, direction, port_id);
	return 0;
}

static int client_node_demarshal_port_set_param(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t seq, direction, port_id, id, flags;
	const struct spa_pod *param = NULL;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &seq,
			"i", &direction,
			"i", &port_id,
			"I", &id,
			"i", &flags,
			"O", &param, NULL) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, port_set_param,
			seq, direction, port_id, id, flags, param);
	return 0;
}

static int client_node_demarshal_port_use_buffers(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t seq, direction, port_id, mix_id, n_buffers, data_id;
	struct pw_client_node_buffer *buffers;
	int i, j;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &seq,
			"i", &direction,
			"i", &port_id,
			"i", &mix_id,
			"i", &n_buffers, NULL) < 0)
		return -EINVAL;

	buffers = alloca(sizeof(struct pw_client_node_buffer) * n_buffers);
	for (i = 0; i < n_buffers; i++) {
		struct spa_buffer *buf = buffers[i].buffer = alloca(sizeof(struct spa_buffer));

		if (spa_pod_parser_get(&prs,
				      "i", &buffers[i].mem_id,
				      "i", &buffers[i].offset,
				      "i", &buffers[i].size,
				      "i", &buf->id,
				      "i", &buf->n_metas, NULL) < 0)
			return -EINVAL;

		buf->metas = alloca(sizeof(struct spa_meta) * buf->n_metas);
		for (j = 0; j < buf->n_metas; j++) {
			struct spa_meta *m = &buf->metas[j];

			if (spa_pod_parser_get(&prs,
					      "I", &m->type,
					      "i", &m->size, NULL) < 0)
				return -EINVAL;
		}
		if (spa_pod_parser_get(&prs, "i", &buf->n_datas, NULL) < 0)
			return -EINVAL;

		buf->datas = alloca(sizeof(struct spa_data) * buf->n_datas);
		for (j = 0; j < buf->n_datas; j++) {
			struct spa_data *d = &buf->datas[j];

			if (spa_pod_parser_get(&prs,
					      "I", &d->type,
					      "i", &data_id,
					      "i", &d->flags,
					      "i", &d->mapoffset,
					      "i", &d->maxsize, NULL) < 0)
				return -EINVAL;

			d->data = SPA_UINT32_TO_PTR(data_id);
		}
	}
	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, port_use_buffers, seq,
									  direction,
									  port_id,
									  mix_id,
									  n_buffers, buffers);
	return 0;
}

static int client_node_demarshal_port_command(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	const struct spa_command *command;
	uint32_t direction, port_id;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &direction,
			"i", &port_id,
			"O", &command, NULL) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, port_command, direction,
									   port_id,
									   command);
	return 0;
}

static int client_node_demarshal_port_set_io(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t seq, direction, port_id, mix_id, id, memid, off, sz;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &seq,
			"i", &direction,
			"i", &port_id,
			"i", &mix_id,
			"I", &id,
			"i", &memid,
			"i", &off,
			"i", &sz, NULL) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, port_set_io,
							seq,
							direction, port_id, mix_id,
							id, memid,
							off, sz);
	return 0;
}

static void
client_node_marshal_add_mem(void *object,
			    uint32_t mem_id,
			    uint32_t type,
			    int memfd, uint32_t flags)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_ADD_MEM);

	spa_pod_builder_struct(b,
			       "i", mem_id,
			       "I", type,
			       "i", pw_protocol_native_add_resource_fd(resource, memfd),
			       "i", flags);

	pw_protocol_native_end_resource(resource, b);
}

static void client_node_marshal_transport(void *object, uint32_t node_id, int readfd, int writefd,
					  struct pw_client_node_transport *transport)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	struct pw_client_node_transport_info info;

	pw_client_node_transport_get_info(transport, &info);

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_TRANSPORT);

	spa_pod_builder_struct(b,
			       "i", node_id,
			       "i", pw_protocol_native_add_resource_fd(resource, readfd),
			       "i", pw_protocol_native_add_resource_fd(resource, writefd),
			       "i", pw_protocol_native_add_resource_fd(resource, info.memfd),
			       "i", info.offset,
			       "i", info.size);

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_set_param(void *object, uint32_t seq, uint32_t id, uint32_t flags,
			      const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_SET_PARAM);

	spa_pod_builder_struct(b,
			       "i", seq,
			       "I", id,
			       "i", flags,
			       "P", param);

	pw_protocol_native_end_resource(resource, b);
}

static void client_node_marshal_event_event(void *object, const struct spa_event *event)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_EVENT);

	spa_pod_builder_struct(b, "P", event);

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_command(void *object, uint32_t seq, const struct spa_command *command)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_COMMAND);

	spa_pod_builder_struct(b, "i", seq, "P", command);

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_add_port(void *object,
			     uint32_t seq, enum spa_direction direction, uint32_t port_id)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_ADD_PORT);

	spa_pod_builder_struct(b,
			       "i", seq,
			       "i", direction,
			       "i", port_id);

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_remove_port(void *object,
				uint32_t seq, enum spa_direction direction, uint32_t port_id)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_REMOVE_PORT);

	spa_pod_builder_struct(b,
			       "i", seq,
			       "i", direction,
			       "i", port_id);

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_port_set_param(void *object,
				   uint32_t seq,
				   enum spa_direction direction,
				   uint32_t port_id,
				   uint32_t id,
				   uint32_t flags,
				   const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_PORT_SET_PARAM);

	spa_pod_builder_struct(b,
			       "i", seq,
			       "i", direction,
			       "i", port_id,
			       "I", id,
			       "i", flags,
			       "P", param);

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_port_use_buffers(void *object,
				     uint32_t seq,
				     enum spa_direction direction,
				     uint32_t port_id,
				     uint32_t mix_id,
				     uint32_t n_buffers, struct pw_client_node_buffer *buffers)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;
	uint32_t i, j;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_PORT_USE_BUFFERS);

	spa_pod_builder_add(b,
			    "[",
			    "i", seq,
			    "i", direction,
			    "i", port_id,
			    "i", mix_id,
			    "i", n_buffers, NULL);

	for (i = 0; i < n_buffers; i++) {
		struct spa_buffer *buf = buffers[i].buffer;

		spa_pod_builder_add(b,
				    "i", buffers[i].mem_id,
				    "i", buffers[i].offset,
				    "i", buffers[i].size,
				    "i", buf->id,
				    "i", buf->n_metas, NULL);

		for (j = 0; j < buf->n_metas; j++) {
			struct spa_meta *m = &buf->metas[j];
			spa_pod_builder_add(b,
					    "I", m->type,
					    "i", m->size, NULL);
		}
		spa_pod_builder_add(b, "i", buf->n_datas, NULL);
		for (j = 0; j < buf->n_datas; j++) {
			struct spa_data *d = &buf->datas[j];
			spa_pod_builder_add(b,
					    "I", d->type,
					    "i", SPA_PTR_TO_UINT32(d->data),
					    "i", d->flags,
					    "i", d->mapoffset,
					    "i", d->maxsize, NULL);
		}
	}
	spa_pod_builder_add(b, "]", NULL);

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

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_PORT_COMMAND);

	spa_pod_builder_struct(b,
			       "i", direction,
			       "i", port_id,
			       "P", command);

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_port_set_io(void *object,
				uint32_t seq,
				uint32_t direction,
				uint32_t port_id,
				uint32_t mix_id,
				uint32_t id,
				uint32_t memid,
				uint32_t offset,
				uint32_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_PORT_SET_IO);

	spa_pod_builder_struct(b,
			       "i", seq,
			       "i", direction,
			       "i", port_id,
			       "i", mix_id,
			       "I", id,
			       "i", memid,
			       "i", offset,
			       "i", size);

	pw_protocol_native_end_resource(resource, b);
}


static int client_node_demarshal_done(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t seq, res;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &seq,
			"i", &res, NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_client_node_proxy_methods, done, seq, res);
	return 0;
}

static int client_node_demarshal_update(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t change_mask, max_input_ports, max_output_ports, n_params;
	const struct spa_pod **params;
	int i;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &change_mask,
			"i", &max_input_ports,
			"i", &max_output_ports,
			"i", &n_params, NULL) < 0)
		return -EINVAL;

	params = alloca(n_params * sizeof(struct spa_pod *));
	for (i = 0; i < n_params; i++)
		if (spa_pod_parser_get(&prs, "O", &params[i], NULL) < 0)
			return -EINVAL;

	pw_resource_do(resource, struct pw_client_node_proxy_methods, update, change_mask,
									max_input_ports,
									max_output_ports,
									n_params,
									params);
	return 0;
}

static int client_node_demarshal_port_update(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t i, direction, port_id, change_mask, n_params;
	const struct spa_pod **params = NULL;
	struct spa_port_info info = { 0 }, *infop = NULL;
	struct spa_pod *ipod;
	struct spa_dict props;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"i", &direction,
			"i", &port_id,
			"i", &change_mask,
			"i", &n_params, NULL) < 0)
		return -EINVAL;

	params = alloca(n_params * sizeof(struct spa_pod *));
	for (i = 0; i < n_params; i++)
		if (spa_pod_parser_get(&prs, "O", &params[i], NULL) < 0)
			return -EINVAL;

	if (spa_pod_parser_get(&prs, "T", &ipod, NULL) < 0)
		return -EINVAL;

	if (ipod) {
		struct spa_pod_parser p2;
		infop = &info;

		spa_pod_parser_pod(&p2, ipod);
		if (spa_pod_parser_get(&p2,
				"["
				"i", &info.flags,
				"i", &info.rate,
				"i", &props.n_items, NULL) < 0)
			return -EINVAL;

		if (props.n_items > 0) {
			info.props = &props;

			props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
			for (i = 0; i < props.n_items; i++) {
				if (spa_pod_parser_get(&p2,
						"s", &props.items[i].key,
						"s", &props.items[i].value,
						NULL) < 0)
					return -EINVAL;
			}
		}
	}

	pw_resource_do(resource, struct pw_client_node_proxy_methods, port_update, direction,
									     port_id,
									     change_mask,
									     n_params,
									     params, infop);
	return 0;
}

static int client_node_demarshal_set_active(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	int active;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"b", &active, NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_client_node_proxy_methods, set_active, active);
	return 0;
}

static int client_node_demarshal_event_method(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	struct spa_event *event;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			"O", &event, NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_client_node_proxy_methods, event, event);
	return 0;
}

static int client_node_demarshal_destroy(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs, "[", NULL) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_client_node_proxy_methods, destroy);
	return 0;
}

static const struct pw_client_node_proxy_methods pw_protocol_native_client_node_method_marshal = {
	PW_VERSION_CLIENT_NODE_PROXY_METHODS,
	&client_node_marshal_done,
	&client_node_marshal_update,
	&client_node_marshal_port_update,
	&client_node_marshal_set_active,
	&client_node_marshal_event_method,
	&client_node_marshal_destroy
};

static const struct pw_protocol_native_demarshal pw_protocol_native_client_node_method_demarshal[] = {
	{ &client_node_demarshal_done, 0 },
	{ &client_node_demarshal_update, PW_PROTOCOL_NATIVE_REMAP },
	{ &client_node_demarshal_port_update, PW_PROTOCOL_NATIVE_REMAP },
	{ &client_node_demarshal_set_active, 0 },
	{ &client_node_demarshal_event_method, PW_PROTOCOL_NATIVE_REMAP },
	{ &client_node_demarshal_destroy, 0 },
};

static const struct pw_client_node_proxy_events pw_protocol_native_client_node_event_marshal = {
	PW_VERSION_CLIENT_NODE_PROXY_EVENTS,
	&client_node_marshal_add_mem,
	&client_node_marshal_transport,
	&client_node_marshal_set_param,
	&client_node_marshal_event_event,
	&client_node_marshal_command,
	&client_node_marshal_add_port,
	&client_node_marshal_remove_port,
	&client_node_marshal_port_set_param,
	&client_node_marshal_port_use_buffers,
	&client_node_marshal_port_command,
	&client_node_marshal_port_set_io,
};

static const struct pw_protocol_native_demarshal pw_protocol_native_client_node_event_demarshal[] = {
	{ &client_node_demarshal_add_mem, PW_PROTOCOL_NATIVE_REMAP },
	{ &client_node_demarshal_transport, 0 },
	{ &client_node_demarshal_set_param, PW_PROTOCOL_NATIVE_REMAP },
	{ &client_node_demarshal_event_event, PW_PROTOCOL_NATIVE_REMAP },
	{ &client_node_demarshal_command, PW_PROTOCOL_NATIVE_REMAP },
	{ &client_node_demarshal_add_port, 0 },
	{ &client_node_demarshal_remove_port, 0 },
	{ &client_node_demarshal_port_set_param, PW_PROTOCOL_NATIVE_REMAP },
	{ &client_node_demarshal_port_use_buffers, PW_PROTOCOL_NATIVE_REMAP },
	{ &client_node_demarshal_port_command, PW_PROTOCOL_NATIVE_REMAP },
	{ &client_node_demarshal_port_set_io, PW_PROTOCOL_NATIVE_REMAP },
};

static const struct pw_protocol_marshal pw_protocol_native_client_node_marshal = {
	PW_TYPE_INTERFACE__ClientNode,
	PW_VERSION_CLIENT_NODE,
	&pw_protocol_native_client_node_method_marshal,
	&pw_protocol_native_client_node_method_demarshal,
	PW_CLIENT_NODE_PROXY_METHOD_NUM,
	&pw_protocol_native_client_node_event_marshal,
	pw_protocol_native_client_node_event_demarshal,
	PW_CLIENT_NODE_PROXY_EVENT_NUM,
};

struct pw_protocol *pw_protocol_native_ext_client_node_init(struct pw_core *core)
{
	struct pw_protocol *protocol;

	protocol = pw_core_find_protocol(core, PW_TYPE_PROTOCOL__Native);

	if (protocol == NULL)
		return NULL;

	pw_protocol_add_marshal(protocol, &pw_protocol_native_client_node_marshal);

	return protocol;
}
