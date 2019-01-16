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

#include <errno.h>

#include <spa/pod/parser.h>

#include <pipewire/pipewire.h>

#include <extensions/protocol-native.h>
#include <extensions/client-node.h>

static void
client_node_marshal_done(void *object, int seq, int res)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_NODE_PROXY_METHOD_DONE);

	spa_pod_builder_add_struct(b,
			       SPA_POD_Int(seq),
			       SPA_POD_Int(res));

	pw_protocol_native_end_proxy(proxy, b);
}


static void
client_node_marshal_update(void *object,
			   uint32_t change_mask,
			   uint32_t max_input_ports,
			   uint32_t max_output_ports,
			   uint32_t n_params,
			   const struct spa_pod **params,
			   const struct spa_dict *props)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;
	uint32_t i, n_items;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_NODE_PROXY_METHOD_UPDATE);

	spa_pod_builder_add(b,
			    "[",
			    SPA_POD_Int(change_mask),
			    SPA_POD_Int(max_input_ports),
			    SPA_POD_Int(max_output_ports),
			    SPA_POD_Int(n_params), NULL);

	for (i = 0; i < n_params; i++)
		spa_pod_builder_add(b, SPA_POD_Pod(params[i]), NULL);

	n_items = props ? props->n_items : 0;
	spa_pod_builder_add(b,
			SPA_POD_Int(n_items), NULL);
	for (i = 0; i < n_items; i++) {
		spa_pod_builder_add(b,
			    SPA_POD_String(props->items[i].key),
			    SPA_POD_String(props->items[i].value), NULL);
	}
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
	uint32_t i, n_items;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_NODE_PROXY_METHOD_PORT_UPDATE);

	spa_pod_builder_add(b,
			    "[",
			    SPA_POD_Int(direction),
			    SPA_POD_Int(port_id),
			    SPA_POD_Int(change_mask),
			    SPA_POD_Int(n_params), NULL);

	for (i = 0; i < n_params; i++)
		spa_pod_builder_add(b,
				SPA_POD_Pod(params[i]), NULL);

	if (info) {
		n_items = info->props ? info->props->n_items : 0;

		spa_pod_builder_add(b,
				    "[",
				    SPA_POD_Int(info->flags),
				    SPA_POD_Int(info->rate),
				    SPA_POD_Int(n_items), NULL);
		for (i = 0; i < n_items; i++) {
			spa_pod_builder_add(b,
					    SPA_POD_String(info->props->items[i].key),
					    SPA_POD_String(info->props->items[i].value), NULL);
		}
		spa_pod_builder_add(b,
				"]", NULL);
	} else {
		spa_pod_builder_add(b,
				SPA_POD_Pod(NULL), NULL);
	}
	spa_pod_builder_add(b, "]", NULL);

	pw_protocol_native_end_proxy(proxy, b);
}

static void client_node_marshal_set_active(void *object, bool active)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_NODE_PROXY_METHOD_SET_ACTIVE);

	spa_pod_builder_add_struct(b,
			SPA_POD_Bool(active));

	pw_protocol_native_end_proxy(proxy, b);
}

static void client_node_marshal_event_method(void *object, struct spa_event *event)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_proxy(proxy, PW_CLIENT_NODE_PROXY_METHOD_EVENT);

	spa_pod_builder_add_struct(b,
			SPA_POD_Pod(event));

	pw_protocol_native_end_proxy(proxy, b);
}

static int client_node_demarshal_add_mem(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t mem_id, type, memfd_idx, flags;
	int memfd;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Int(&mem_id),
			SPA_POD_Id(&type),
			SPA_POD_Int(&memfd_idx),
			SPA_POD_Int(&flags)) < 0)
		return -EINVAL;

	memfd = pw_protocol_native_get_proxy_fd(proxy, memfd_idx);

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, add_mem, 0,
								      mem_id,
								      type,
								      memfd, flags);
	return 0;
}

static int client_node_demarshal_transport(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t node_id, ridx, widx;
	int readfd, writefd;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Int(&node_id),
			SPA_POD_Int(&ridx),
			SPA_POD_Int(&widx)) < 0)
		return -EINVAL;

	readfd = pw_protocol_native_get_proxy_fd(proxy, ridx);
	writefd = pw_protocol_native_get_proxy_fd(proxy, widx);

	if (readfd == -1 || writefd == -1)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, transport, 0, node_id,
								   readfd, writefd);
	return 0;
}

static int client_node_demarshal_set_param(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t seq, id, flags;
	const struct spa_pod *param = NULL;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Int(&seq),
			SPA_POD_Id(&id),
			SPA_POD_Int(&flags),
			SPA_POD_PodObject(&param)) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, set_param, 0, seq, id, flags, param);
	return 0;
}

static int client_node_demarshal_event_event(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	const struct spa_event *event;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_PodObject(&event)) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, event, 0, event);
	return 0;
}

static int client_node_demarshal_command(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	const struct spa_command *command;
	uint32_t seq;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Int(&seq),
			SPA_POD_PodObject(&command)) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, command, 0, seq, command);
	return 0;
}

static int client_node_demarshal_add_port(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	int32_t seq, direction, port_id;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Int(&seq),
			SPA_POD_Int(&direction),
			SPA_POD_Int(&port_id)) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, add_port, 0, seq, direction, port_id);
	return 0;
}

static int client_node_demarshal_remove_port(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	int32_t seq, direction, port_id;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Int(&seq),
			SPA_POD_Int(&direction),
			SPA_POD_Int(&port_id)) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, remove_port, 0, seq, direction, port_id);
	return 0;
}

static int client_node_demarshal_port_set_param(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t seq, direction, port_id, id, flags;
	const struct spa_pod *param = NULL;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Int(&seq),
			SPA_POD_Int(&direction),
			SPA_POD_Int(&port_id),
			SPA_POD_Id(&id),
			SPA_POD_Int(&flags),
			SPA_POD_PodObject(&param)) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, port_set_param, 0,
			seq, direction, port_id, id, flags, param);
	return 0;
}

static int client_node_demarshal_port_use_buffers(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t seq, direction, port_id, mix_id, n_buffers, data_id;
	struct pw_client_node_buffer *buffers;
	uint32_t i, j;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			SPA_POD_Int(&seq),
			SPA_POD_Int(&direction),
			SPA_POD_Int(&port_id),
			SPA_POD_Int(&mix_id),
			SPA_POD_Int(&n_buffers), NULL) < 0)
		return -EINVAL;

	buffers = alloca(sizeof(struct pw_client_node_buffer) * n_buffers);
	for (i = 0; i < n_buffers; i++) {
		struct spa_buffer *buf = buffers[i].buffer = alloca(sizeof(struct spa_buffer));

		if (spa_pod_parser_get(&prs,
				      SPA_POD_Int(&buffers[i].mem_id),
				      SPA_POD_Int(&buffers[i].offset),
				      SPA_POD_Int(&buffers[i].size),
				      SPA_POD_Int(&buf->n_metas), NULL) < 0)
			return -EINVAL;

		buf->metas = alloca(sizeof(struct spa_meta) * buf->n_metas);
		for (j = 0; j < buf->n_metas; j++) {
			struct spa_meta *m = &buf->metas[j];

			if (spa_pod_parser_get(&prs,
					      SPA_POD_Id(&m->type),
					      SPA_POD_Int(&m->size), NULL) < 0)
				return -EINVAL;
		}
		if (spa_pod_parser_get(&prs,
					SPA_POD_Int(&buf->n_datas), NULL) < 0)
			return -EINVAL;

		buf->datas = alloca(sizeof(struct spa_data) * buf->n_datas);
		for (j = 0; j < buf->n_datas; j++) {
			struct spa_data *d = &buf->datas[j];

			if (spa_pod_parser_get(&prs,
					      SPA_POD_Id(&d->type),
					      SPA_POD_Int(&data_id),
					      SPA_POD_Int(&d->flags),
					      SPA_POD_Int(&d->mapoffset),
					      SPA_POD_Int(&d->maxsize), NULL) < 0)
				return -EINVAL;

			d->data = SPA_UINT32_TO_PTR(data_id);
		}
	}
	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, port_use_buffers, 0, seq,
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
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Int(&direction),
			SPA_POD_Int(&port_id),
			SPA_POD_PodObject(&command)) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, port_command, 0, direction,
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
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Int(&seq),
			SPA_POD_Int(&direction),
			SPA_POD_Int(&port_id),
			SPA_POD_Int(&mix_id),
			SPA_POD_Id(&id),
			SPA_POD_Int(&memid),
			SPA_POD_Int(&off),
			SPA_POD_Int(&sz)) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, port_set_io, 0,
							seq,
							direction, port_id, mix_id,
							id, memid,
							off, sz);
	return 0;
}

static int client_node_demarshal_set_io(void *object, void *data, size_t size)
{
	struct pw_proxy *proxy = object;
	struct spa_pod_parser prs;
	uint32_t id, memid, off, sz;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Id(&id),
			SPA_POD_Int(&memid),
			SPA_POD_Int(&off),
			SPA_POD_Int(&sz)) < 0)
		return -EINVAL;

	pw_proxy_notify(proxy, struct pw_client_node_proxy_events, set_io, 0,
			id, memid, off, sz);
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

	spa_pod_builder_add_struct(b,
		       SPA_POD_Int(mem_id),
		       SPA_POD_Id(type),
		       SPA_POD_Int(pw_protocol_native_add_resource_fd(resource, memfd)),
		       SPA_POD_Int(flags));

	pw_protocol_native_end_resource(resource, b);
}

static void client_node_marshal_transport(void *object, uint32_t node_id, int readfd, int writefd)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_TRANSPORT);

	spa_pod_builder_add_struct(b,
			       SPA_POD_Int(node_id),
			       SPA_POD_Int(pw_protocol_native_add_resource_fd(resource, readfd)),
			       SPA_POD_Int(pw_protocol_native_add_resource_fd(resource, writefd)));

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_set_param(void *object, uint32_t seq, uint32_t id, uint32_t flags,
			      const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_SET_PARAM);

	spa_pod_builder_add_struct(b,
			       SPA_POD_Int(seq),
			       SPA_POD_Id(id),
			       SPA_POD_Int(flags),
			       SPA_POD_Pod(param));

	pw_protocol_native_end_resource(resource, b);
}

static void client_node_marshal_event_event(void *object, const struct spa_event *event)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_EVENT);

	spa_pod_builder_add_struct(b,
			SPA_POD_Pod(event));

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_command(void *object, uint32_t seq, const struct spa_command *command)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_COMMAND);

	spa_pod_builder_add_struct(b,
			SPA_POD_Int(seq),
			SPA_POD_Pod(command));

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_add_port(void *object,
			     uint32_t seq, enum spa_direction direction, uint32_t port_id)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_ADD_PORT);

	spa_pod_builder_add_struct(b,
			       SPA_POD_Int(seq),
			       SPA_POD_Int(direction),
			       SPA_POD_Int(port_id));

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_remove_port(void *object,
				uint32_t seq, enum spa_direction direction, uint32_t port_id)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_REMOVE_PORT);

	spa_pod_builder_add_struct(b,
			       SPA_POD_Int(seq),
			       SPA_POD_Int(direction),
			       SPA_POD_Int(port_id));

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

	spa_pod_builder_add_struct(b,
			       SPA_POD_Int(seq),
			       SPA_POD_Int(direction),
			       SPA_POD_Int(port_id),
			       SPA_POD_Id(id),
			       SPA_POD_Int(flags),
			       SPA_POD_Pod(param));

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
			    SPA_POD_Int(seq),
			    SPA_POD_Int(direction),
			    SPA_POD_Int(port_id),
			    SPA_POD_Int(mix_id),
			    SPA_POD_Int(n_buffers), NULL);

	for (i = 0; i < n_buffers; i++) {
		struct spa_buffer *buf = buffers[i].buffer;

		spa_pod_builder_add(b,
				    SPA_POD_Int(buffers[i].mem_id),
				    SPA_POD_Int(buffers[i].offset),
				    SPA_POD_Int(buffers[i].size),
				    SPA_POD_Int(buf->n_metas), NULL);

		for (j = 0; j < buf->n_metas; j++) {
			struct spa_meta *m = &buf->metas[j];
			spa_pod_builder_add(b,
					    SPA_POD_Id(m->type),
					    SPA_POD_Int(m->size), NULL);
		}
		spa_pod_builder_add(b,
				SPA_POD_Int(buf->n_datas), NULL);
		for (j = 0; j < buf->n_datas; j++) {
			struct spa_data *d = &buf->datas[j];
			spa_pod_builder_add(b,
					    SPA_POD_Id(d->type),
					    SPA_POD_Int(SPA_PTR_TO_UINT32(d->data)),
					    SPA_POD_Int(d->flags),
					    SPA_POD_Int(d->mapoffset),
					    SPA_POD_Int(d->maxsize), NULL);
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

	spa_pod_builder_add_struct(b,
			       SPA_POD_Int(direction),
			       SPA_POD_Int(port_id),
			       SPA_POD_Pod(command));

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

	spa_pod_builder_add_struct(b,
			       SPA_POD_Int(seq),
			       SPA_POD_Int(direction),
			       SPA_POD_Int(port_id),
			       SPA_POD_Int(mix_id),
			       SPA_POD_Id(id),
			       SPA_POD_Int(memid),
			       SPA_POD_Int(offset),
			       SPA_POD_Int(size));

	pw_protocol_native_end_resource(resource, b);
}

static void
client_node_marshal_set_io(void *object,
			   uint32_t id,
			   uint32_t memid,
			   uint32_t offset,
			   uint32_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_builder *b;

	b = pw_protocol_native_begin_resource(resource, PW_CLIENT_NODE_PROXY_EVENT_SET_IO);
	spa_pod_builder_add_struct(b,
			       SPA_POD_Id(id),
			       SPA_POD_Int(memid),
			       SPA_POD_Int(offset),
			       SPA_POD_Int(size));
	pw_protocol_native_end_resource(resource, b);
}

static int client_node_demarshal_done(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t seq, res;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_Int(&seq),
			SPA_POD_Int(&res)) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_client_node_proxy_methods, done, 0, seq, res);
	return 0;
}

static int client_node_demarshal_update(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	uint32_t change_mask, max_input_ports, max_output_ports, n_params;
	const struct spa_pod **params;
	struct spa_dict props;
	uint32_t i;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get(&prs,
			"["
			SPA_POD_Int(&change_mask),
			SPA_POD_Int(&max_input_ports),
			SPA_POD_Int(&max_output_ports),
			SPA_POD_Int(&n_params), NULL) < 0)
		return -EINVAL;

	params = alloca(n_params * sizeof(struct spa_pod *));
	for (i = 0; i < n_params; i++)
		if (spa_pod_parser_get(&prs,
					SPA_POD_PodObject(&params[i]), NULL) < 0)
			return -EINVAL;

	if (spa_pod_parser_get(&prs,
			SPA_POD_Int(&props.n_items), NULL) < 0)
		return -EINVAL;

	props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
	for (i = 0; i < props.n_items; i++) {
		if (spa_pod_parser_get(&prs,
				SPA_POD_String(&props.items[i].key),
				SPA_POD_String(&props.items[i].value), NULL) < 0)
			return -EINVAL;
	}

	pw_resource_do(resource, struct pw_client_node_proxy_methods, update, 0, change_mask,
									max_input_ports,
									max_output_ports,
									n_params,
									params,
									props.n_items > 0 ?
										&props : NULL);
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
			SPA_POD_Int(&direction),
			SPA_POD_Int(&port_id),
			SPA_POD_Int(&change_mask),
			SPA_POD_Int(&n_params), NULL) < 0)
		return -EINVAL;

	params = alloca(n_params * sizeof(struct spa_pod *));
	for (i = 0; i < n_params; i++)
		if (spa_pod_parser_get(&prs,
					SPA_POD_PodObject(&params[i]), NULL) < 0)
			return -EINVAL;

	if (spa_pod_parser_get(&prs,
				SPA_POD_PodStruct(&ipod), NULL) < 0)
		return -EINVAL;

	if (ipod) {
		struct spa_pod_parser p2;
		infop = &info;

		spa_pod_parser_pod(&p2, ipod);
		if (spa_pod_parser_get(&p2,
				"["
				SPA_POD_Int(&info.flags),
				SPA_POD_Int(&info.rate),
				SPA_POD_Int(&props.n_items), NULL) < 0)
			return -EINVAL;

		if (props.n_items > 0) {
			info.props = &props;

			props.items = alloca(props.n_items * sizeof(struct spa_dict_item));
			for (i = 0; i < props.n_items; i++) {
				if (spa_pod_parser_get(&p2,
						SPA_POD_String(&props.items[i].key),
						SPA_POD_String(&props.items[i].value), NULL) < 0)
					return -EINVAL;
			}
		}
	}

	pw_resource_do(resource, struct pw_client_node_proxy_methods, port_update, 0, direction,
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
	if (spa_pod_parser_get_struct(&prs,
				SPA_POD_Bool(&active)) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_client_node_proxy_methods, set_active, 0, active);
	return 0;
}

static int client_node_demarshal_event_method(void *object, void *data, size_t size)
{
	struct pw_resource *resource = object;
	struct spa_pod_parser prs;
	struct spa_event *event;

	spa_pod_parser_init(&prs, data, size, 0);
	if (spa_pod_parser_get_struct(&prs,
			SPA_POD_PodObject(&event)) < 0)
		return -EINVAL;

	pw_resource_do(resource, struct pw_client_node_proxy_methods, event, 0, event);
	return 0;
}

static const struct pw_client_node_proxy_methods pw_protocol_native_client_node_method_marshal = {
	PW_VERSION_CLIENT_NODE_PROXY_METHODS,
	&client_node_marshal_done,
	&client_node_marshal_update,
	&client_node_marshal_port_update,
	&client_node_marshal_set_active,
	&client_node_marshal_event_method
};

static const struct pw_protocol_native_demarshal pw_protocol_native_client_node_method_demarshal[] = {
	{ &client_node_demarshal_done, 0 },
	{ &client_node_demarshal_update, 0 },
	{ &client_node_demarshal_port_update, 0 },
	{ &client_node_demarshal_set_active, 0 },
	{ &client_node_demarshal_event_method, 0 }
};

static const struct pw_client_node_proxy_events pw_protocol_native_client_node_event_marshal = {
	PW_VERSION_CLIENT_NODE_PROXY_EVENTS,
	&client_node_marshal_add_mem,
	&client_node_marshal_transport,
	&client_node_marshal_set_param,
	&client_node_marshal_set_io,
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
	{ &client_node_demarshal_add_mem, 0 },
	{ &client_node_demarshal_transport, 0 },
	{ &client_node_demarshal_set_param, 0 },
	{ &client_node_demarshal_set_io, 0 },
	{ &client_node_demarshal_event_event, 0 },
	{ &client_node_demarshal_command, 0 },
	{ &client_node_demarshal_add_port, 0 },
	{ &client_node_demarshal_remove_port, 0 },
	{ &client_node_demarshal_port_set_param, 0 },
	{ &client_node_demarshal_port_use_buffers, 0 },
	{ &client_node_demarshal_port_command, 0 },
	{ &client_node_demarshal_port_set_io, 0 },
};

static const struct pw_protocol_marshal pw_protocol_native_client_node_marshal = {
	PW_TYPE_INTERFACE_ClientNode,
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

	protocol = pw_core_find_protocol(core, PW_TYPE_INFO_PROTOCOL_Native);

	if (protocol == NULL)
		return NULL;

	pw_protocol_add_marshal(protocol, &pw_protocol_native_client_node_marshal);

	return protocol;
}
