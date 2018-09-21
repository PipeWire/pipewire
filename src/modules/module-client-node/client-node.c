/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/eventfd.h>

#include <spa/node/node.h>
#include <spa/pod/filter.h>
#include <spa/debug/types.h>

#include "pipewire/pipewire.h"
#include "pipewire/interfaces.h"
#include "pipewire/private.h"

#include "pipewire/core.h"
#include "modules/spa/spa-node.h"
#include "client-node.h"

/** \cond */

#define MAX_INPUTS	64
#define MAX_OUTPUTS	64

#define MAX_BUFFERS	64
#define MAX_AREAS	1024
#define MAX_IO		32
#define MAX_MIX		128

#define CHECK_IN_PORT_ID(this,d,p)       ((d) == SPA_DIRECTION_INPUT && (p) < MAX_INPUTS)
#define CHECK_OUT_PORT_ID(this,d,p)      ((d) == SPA_DIRECTION_OUTPUT && (p) < MAX_OUTPUTS)
#define CHECK_PORT_ID(this,d,p)          (CHECK_IN_PORT_ID(this,d,p) || CHECK_OUT_PORT_ID(this,d,p))
#define CHECK_FREE_IN_PORT(this,d,p)     (CHECK_IN_PORT_ID(this,d,p) && (this)->in_ports[p] == NULL)
#define CHECK_FREE_OUT_PORT(this,d,p)    (CHECK_OUT_PORT_ID(this,d,p) && (this)->out_ports[p] == NULL)
#define CHECK_FREE_PORT(this,d,p)        (CHECK_FREE_IN_PORT (this,d,p) || CHECK_FREE_OUT_PORT (this,d,p))
#define CHECK_IN_PORT(this,d,p)          (CHECK_IN_PORT_ID(this,d,p) && (this)->in_ports[p])
#define CHECK_OUT_PORT(this,d,p)         (CHECK_OUT_PORT_ID(this,d,p) && (this)->out_ports[p])
#define CHECK_PORT(this,d,p)             (CHECK_IN_PORT (this,d,p) || CHECK_OUT_PORT (this,d,p))

#define GET_IN_PORT(this,p)	(this->in_ports[p])
#define GET_OUT_PORT(this,p)	(this->out_ports[p])
#define GET_PORT(this,d,p)	(d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,p) : GET_OUT_PORT(this,p))

#define CHECK_PORT_BUFFER(this,b,p)      (b < p->n_buffers)

struct mem {
	uint32_t id;
	int ref;
	int fd;
	uint32_t type;
	uint32_t flags;
};

struct buffer {
	struct spa_buffer *outbuf;
	struct spa_buffer buffer;
	struct spa_meta metas[4];
	struct spa_data datas[4];
	uint32_t memid;
};

struct io {
	uint32_t id;
	uint32_t memid;
};

struct mix {
	bool valid;
	bool active;
	uint32_t id;
	struct port *port;
	uint32_t n_buffers;
	struct buffer buffers[MAX_BUFFERS];
	struct io ios[MAX_IO];
};

struct port {
	struct pw_port *port;
	struct node *node;
	struct impl *impl;

	enum spa_direction direction;
	uint32_t id;

	struct spa_node mix_node;

	struct spa_port_info info;
	struct pw_properties *properties;

	bool have_format;
	uint32_t n_params;
	struct spa_pod **params;

	struct mix mix[MAX_MIX+1];
};

struct node {
	struct spa_node node;

	struct impl *impl;

	struct spa_log *log;
	struct spa_loop *data_loop;

	const struct spa_node_callbacks *callbacks;
	void *callbacks_data;

	struct pw_resource *resource;

	struct spa_source data_source;
	int writefd;

	uint32_t max_inputs;
	uint32_t n_inputs;
	uint32_t max_outputs;
	uint32_t n_outputs;
	struct port *in_ports[MAX_INPUTS];
	struct port *out_ports[MAX_OUTPUTS];

	uint32_t n_params;
	struct spa_pod **params;

	uint32_t seq;
};

struct impl {
	struct pw_client_node this;

	struct pw_core *core;

	struct node node;

	struct pw_map io_map;
	struct pw_memblock *io_areas;

	struct spa_hook node_listener;
	struct spa_hook resource_listener;

	struct pw_array mems;

	int fds[2];
	int other_fds[2];

	struct pw_client_node_position *position;
	uint64_t start;
};

static int
do_port_use_buffers(struct impl *impl,
		    enum spa_direction direction,
		    uint32_t port_id,
		    uint32_t mix_id,
		    struct spa_buffer **buffers,
		    uint32_t n_buffers);

/** \endcond */

static struct mem *ensure_mem(struct impl *impl, int fd, uint32_t type, uint32_t flags)
{
	struct mem *m, *f = NULL;

	pw_array_for_each(m, &impl->mems) {
		if (m->ref <= 0)
			f = m;
		else if (m->fd == fd)
			goto found;
	}

	if (f == NULL) {
		m = pw_array_add(&impl->mems, sizeof(struct mem));
		m->id = pw_array_get_len(&impl->mems, struct mem) - 1;
	}
	else {
		m = f;
	}
	m->fd = fd;
	m->type = type;
	m->flags = flags;
	m->ref = 0;

	pw_log_debug("client-node %p: add mem %d", impl, m->id);

	pw_client_node_resource_add_mem(impl->node.resource,
					m->id,
					type,
					m->fd,
					m->flags);
      found:
	m->ref++;
	pw_log_debug("client-node %p: mem %d, ref %d", impl, m->id, m->ref);
	return m;
}

static struct mix *find_mix(struct port *p, uint32_t mix_id)
{
	struct mix *mix;
	if (mix_id == SPA_ID_INVALID)
		return &p->mix[MAX_MIX];
	if (mix_id >= MAX_MIX)
		return NULL;
	mix = &p->mix[mix_id];
	return mix;
}

static void mix_init(struct mix *mix, struct port *p, uint32_t id)
{
	int i;
	mix->valid = true;
	mix->id = id;
	mix->port = p;
	mix->active = false;
	mix->n_buffers = 0;
	for (i = 0; i < MAX_IO; i++)
		mix->ios[i].id = SPA_ID_INVALID;
}


static struct mix *ensure_mix(struct impl *impl, struct port *p, uint32_t mix_id)
{
	struct mix *mix;

	if ((mix = find_mix(p, mix_id)) == NULL)
		return NULL;
	if (mix->valid)
		return mix;
	mix_init(mix, p, mix_id);
	return mix;
}

static void clear_io(struct node *node, struct io *io)
{
	struct mem *m;
	m = pw_array_get_unchecked(&node->impl->mems, io->memid, struct mem);
	m->ref--;
	io->id = SPA_ID_INVALID;
}

static struct io *update_io(struct impl *impl, struct port *port,
		struct mix *mix, uint32_t id, uint32_t memid)
{
	int i;
	struct io *io, *f = NULL;

	for (i = 0; i < MAX_IO; i++) {
		io = &mix->ios[i];
		if (io->id == SPA_ID_INVALID)
			f = io;
		else if (io->id == id) {
			if (io->memid != memid) {
				clear_io(&impl->node, io);
				if (memid == SPA_ID_INVALID)
					io->id = SPA_ID_INVALID;
			}
			goto found;
		}
	}
	if (f == NULL)
		return NULL;

	io = f;
	io->id = id;
	io->memid = memid;

     found:
	return io;
}

static void clear_ios(struct node *this, struct mix *mix)
{
	int i;

	for (i = 0; i < MAX_IO; i++) {
		struct io *io = &mix->ios[i];
		if (io->id != SPA_ID_INVALID)
			clear_io(this, io);
	}
}

static int clear_buffers(struct node *this, struct mix *mix)
{
	uint32_t i, j;
	struct impl *impl = this->impl;

	for (i = 0; i < mix->n_buffers; i++) {
		struct buffer *b = &mix->buffers[i];
		struct mem *m;

		spa_log_debug(this->log, "node %p: clear buffer %d", this, i);

		for (j = 0; j < b->buffer.n_datas; j++) {
			struct spa_data *d = &b->datas[j];

			if (d->type == SPA_DATA_DmaBuf ||
			    d->type == SPA_DATA_MemFd) {
				uint32_t id;

				id = SPA_PTR_TO_UINT32(b->buffer.datas[j].data);
				m = pw_array_get_unchecked(&impl->mems, id, struct mem);
				m->ref--;
				pw_log_debug("client-node %p: mem %d, ref %d", impl, m->id, m->ref);
			}
		}
		m = pw_array_get_unchecked(&impl->mems, b->memid, struct mem);
		m->ref--;
		pw_log_debug("client-node %p: mem %d, ref %d", impl, m->id, m->ref);
	}
	mix->n_buffers = 0;
	return 0;
}

static void mix_clear(struct node *this, struct mix *mix)
{
	struct port *port = mix->port;

	if (!mix->valid)
		return;
	do_port_use_buffers(this->impl, port->direction, port->id,
			mix->id, NULL, 0);
	clear_ios(this, mix);
	mix->valid = false;
}

static int impl_node_enum_params(struct spa_node *node,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod *filter,
				 struct spa_pod **result,
				 struct spa_pod_builder *builder)
{
	struct node *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);

	while (true) {
		struct spa_pod *param;

		if (*index >= this->n_params)
			return 0;

		param = this->params[(*index)++];

		if (!spa_pod_is_object_type(param, id))
			continue;

		if (spa_pod_filter(builder, result, param, filter) == 0)
			break;
	}
	return 1;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct node *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (this->resource == NULL)
		return 0;

	pw_client_node_resource_set_param(this->resource, this->seq, id, flags, param);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct node *this;
	int res = 0;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (this->resource == NULL)
		return 0;

	pw_client_node_resource_command(this->resource, this->seq, command);
	res = SPA_RESULT_RETURN_ASYNC(this->seq++);

	return res;
}

static int
impl_node_set_callbacks(struct spa_node *node,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct node *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);
	this->callbacks = callbacks;
	this->callbacks_data = data;

	return 0;
}

static int
impl_node_get_n_ports(struct spa_node *node,
		      uint32_t *n_input_ports,
		      uint32_t *max_input_ports,
		      uint32_t *n_output_ports,
		      uint32_t *max_output_ports)
{
	struct node *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (n_input_ports)
		*n_input_ports = this->n_inputs;
	if (max_input_ports)
		*max_input_ports = this->max_inputs == 0 ? this->n_inputs : this->max_inputs;
	if (n_output_ports)
		*n_output_ports = this->n_outputs;
	if (max_output_ports)
		*max_output_ports = this->max_outputs == 0 ? this->n_outputs : this->max_outputs;

	return 0;
}

static int
impl_node_get_port_ids(struct spa_node *node,
		       uint32_t *input_ids,
		       uint32_t n_input_ids,
		       uint32_t *output_ids,
		       uint32_t n_output_ids)
{
	struct node *this;
	int c, i;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (input_ids) {
		for (c = 0, i = 0; i < MAX_INPUTS && c < n_input_ids; i++) {
			if (this->in_ports[i])
				input_ids[c++] = i;
		}
	}
	if (output_ids) {
		for (c = 0, i = 0; i < MAX_OUTPUTS && c < n_output_ids; i++) {
			if (this->out_ports[i])
				output_ids[c++] = i;
		}
	}
	return 0;
}

static void
do_update_port(struct node *this,
	       struct port *port,
	       uint32_t change_mask,
	       uint32_t n_params,
	       const struct spa_pod **params,
	       const struct spa_port_info *info)
{
	int i;

	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_PARAMS) {
		port->have_format = false;

		spa_log_debug(this->log, "node %p: port %u update %d params", this, port->id, n_params);
		for (i = 0; i < port->n_params; i++)
			free(port->params[i]);
		port->n_params = n_params;
		port->params = realloc(port->params, port->n_params * sizeof(struct spa_pod *));

		for (i = 0; i < port->n_params; i++) {
			port->params[i] = pw_spa_pod_copy(params[i]);

			if (spa_pod_is_object_id(port->params[i], SPA_PARAM_Format))
				port->have_format = true;
		}
	}

	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_INFO) {
		if (port->properties)
			pw_properties_free(port->properties);
		port->properties = NULL;
		port->info.props = NULL;

		if (info) {
			port->info = *info;
			if (info->props) {
				port->properties = pw_properties_new_dict(info->props);
				port->info.props = &port->properties->dict;
			}
		}
	}
}

static void
clear_port(struct node *this, struct port *port)
{
	int i;

	spa_log_debug(this->log, "node %p: clear port %p", this, port);

	if (port == NULL)
		return ;

	do_update_port(this, port,
		       PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
		       PW_CLIENT_NODE_PORT_UPDATE_INFO, 0, NULL, NULL);

	for (i = 0; i < MAX_MIX+1; i++) {
		struct mix *mix = &port->mix[i];
		mix_clear(this, mix);
	}

	if (port->direction == SPA_DIRECTION_INPUT) {
		if (this->in_ports[port->id] == port) {
			this->in_ports[port->id] = NULL;
			this->n_inputs--;
		}
	}
	else {
		if (this->out_ports[port->id] == port) {
			this->out_ports[port->id] = NULL;
			this->n_outputs--;
		}
	}
}

static int
impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct node *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	this = SPA_CONTAINER_OF(node, struct node, node);

	spa_return_val_if_fail(CHECK_FREE_PORT(this, direction, port_id), -EINVAL);

	pw_client_node_resource_add_port(this->resource,
					 this->seq,
					 direction, port_id);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int
impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct node *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	this = SPA_CONTAINER_OF(node, struct node, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	pw_client_node_resource_remove_port(this->resource,
					    this->seq,
					    direction, port_id);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int
impl_node_port_get_info(struct spa_node *node,
			enum spa_direction direction, uint32_t port_id,
			const struct spa_port_info **info)
{
	struct node *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);
	*info = &port->info;

	return 0;
}

static int
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t *index,
			   const struct spa_pod *filter,
			   struct spa_pod **result,
			   struct spa_pod_builder *builder)
{
	struct node *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	pw_log_debug("client-node %p: port %d.%d", this,
			direction, port_id);

	while (true) {
		struct spa_pod *param;

		if (*index >= port->n_params)
			return 0;

		param = port->params[(*index)++];

		if (!spa_pod_is_object_id(param, id))
			continue;

		if (spa_pod_filter(builder, result, param, filter) == 0)
			break;
	}
	return 1;
}

static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct node *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (this->resource == NULL)
		return 0;

	pw_client_node_resource_port_set_param(this->resource,
					       this->seq,
					       direction, port_id,
					       id, flags,
					       param);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int do_port_set_io(struct impl *impl,
			  enum spa_direction direction, uint32_t port_id,
			  uint32_t mix_id,
			  uint32_t id, void *data, size_t size)
{
	struct node *this = &impl->node;
	struct pw_memblock *mem;
	struct mem *m;
	uint32_t memid, mem_offset, mem_size;
	struct port *port;
	struct mix *mix;

	pw_log_debug("client-node %p: %s port %d.%d set io %p %zd", impl,
			direction == SPA_DIRECTION_INPUT ? "input" : "output",
			port_id, mix_id, data, size);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (this->resource == NULL)
		return 0;

	port = GET_PORT(this, direction, port_id);

	if ((mix = find_mix(port, mix_id)) == NULL || !mix->valid)
		return -EINVAL;

	if (data) {
		if ((mem = pw_memblock_find(data)) == NULL)
			return -EINVAL;

		mem_offset = SPA_PTRDIFF(data, mem->ptr);
		mem_size = mem->size;
		if (mem_size - mem_offset < size)
			return -EINVAL;

		mem_offset += mem->offset;
		m = ensure_mem(impl, mem->fd, SPA_DATA_MemFd, mem->flags);
		memid = m->id;
	}
	else {
		memid = SPA_ID_INVALID;
		mem_offset = mem_size = 0;
	}

	update_io(impl, port, mix, id, memid);

	pw_client_node_resource_port_set_io(this->resource,
					    this->seq,
					    direction, port_id,
					    mix_id,
					    id,
					    memid,
					    mem_offset, mem_size);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int
impl_node_port_set_io(struct spa_node *node,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	struct node *this;
	struct impl *impl;

	this = SPA_CONTAINER_OF(node, struct node, node);
	impl = this->impl;

	return do_port_set_io(impl, direction, port_id, SPA_ID_INVALID, id, data, size);
}

static int
do_port_use_buffers(struct impl *impl,
		    enum spa_direction direction,
		    uint32_t port_id,
		    uint32_t mix_id,
		    struct spa_buffer **buffers,
		    uint32_t n_buffers)
{
	struct node *this = &impl->node;
	struct port *p;
	struct mix *mix;
	uint32_t i, j;
	struct pw_client_node_buffer *mb;

	spa_log_debug(this->log, "client-node %p: %s port %d.%d use buffers %p %u", impl,
			direction == SPA_DIRECTION_INPUT ? "input" : "output",
			port_id, mix_id, buffers, n_buffers);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	p = GET_PORT(this, direction, port_id);
	if (!p->have_format)
		return -EIO;

	if ((mix = find_mix(p, mix_id)) == NULL || !mix->valid)
		return -EINVAL;

	clear_buffers(this, mix);

	if (n_buffers > 0) {
		mb = alloca(n_buffers * sizeof(struct pw_client_node_buffer));
	} else {
		mb = NULL;
	}

	mix->n_buffers = n_buffers;

	if (this->resource == NULL)
		return 0;

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &mix->buffers[i];
		struct pw_memblock *mem;
		struct mem *m;
		size_t data_size, size;
		void *baseptr;

		b->outbuf = buffers[i];
		memcpy(&b->buffer, buffers[i], sizeof(struct spa_buffer));
		b->buffer.datas = b->datas;
		b->buffer.metas = b->metas;

		if (buffers[i]->n_metas > 0)
			baseptr = buffers[i]->metas[0].data;
		else if (buffers[i]->n_datas > 0)
			baseptr = buffers[i]->datas[0].chunk;
		else
			return -EINVAL;

		if ((mem = pw_memblock_find(baseptr)) == NULL)
			return -EINVAL;

		data_size = 0;
		for (j = 0; j < buffers[i]->n_metas; j++) {
			data_size += buffers[i]->metas[j].size;
		}
		for (j = 0; j < buffers[i]->n_datas; j++) {
			struct spa_data *d = buffers[i]->datas;
			data_size += sizeof(struct spa_chunk);
			if (d->type == SPA_DATA_MemPtr)
				data_size += d->maxsize;
		}

		m = ensure_mem(impl, mem->fd, SPA_DATA_MemFd, mem->flags);
		b->memid = m->id;

		mb[i].buffer = &b->buffer;
		mb[i].mem_id = b->memid;
		mb[i].offset = SPA_PTRDIFF(baseptr, mem->ptr + mem->offset);
		mb[i].size = data_size;

		for (j = 0; j < buffers[i]->n_metas; j++)
			memcpy(&b->buffer.metas[j], &buffers[i]->metas[j], sizeof(struct spa_meta));
		b->buffer.n_metas = j;

		size = buffers[i]->n_datas * sizeof(struct spa_chunk);
		for (j = 0; j < buffers[i]->n_datas; j++) {
			struct spa_data *d = &buffers[i]->datas[j];

			memcpy(&b->buffer.datas[j], d, sizeof(struct spa_data));

			if (d->type == SPA_DATA_DmaBuf ||
			    d->type == SPA_DATA_MemFd) {
				m = ensure_mem(impl, d->fd, d->type, d->flags);
				b->buffer.datas[j].data = SPA_UINT32_TO_PTR(m->id);
			} else if (d->type == SPA_DATA_MemPtr) {
				b->buffer.datas[j].data = SPA_INT_TO_PTR(size);
				size += d->maxsize;
			} else {
				b->buffer.datas[j].type = SPA_ID_INVALID;
				b->buffer.datas[j].data = NULL;
				spa_log_error(this->log, "invalid memory type %d", d->type);
			}
		}
	}
	impl->start = -1;

	pw_client_node_resource_port_use_buffers(this->resource,
						 this->seq,
						 direction, port_id, mix_id,
						 n_buffers, mb);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int
impl_node_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct node *this;
	struct impl *impl;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);
	impl = this->impl;

	return do_port_use_buffers(impl, direction, port_id,
			SPA_ID_INVALID, buffers, n_buffers);
}

static int
impl_node_port_alloc_buffers(struct spa_node *node,
			     enum spa_direction direction,
			     uint32_t port_id,
			     struct spa_pod **params,
			     uint32_t n_params,
			     struct spa_buffer **buffers,
			     uint32_t *n_buffers)
{
	struct node *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(buffers != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	if (!port->have_format)
		return -EIO;

	spa_log_warn(this->log, "not supported");
	return -ENOTSUP;
}

static int
impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct node *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	this = SPA_CONTAINER_OF(node, struct node, node);

	spa_return_val_if_fail(CHECK_OUT_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	spa_log_trace(this->log, "reuse buffer %d", buffer_id);

	return -ENOTSUP;
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id, const struct spa_command *command)
{
	struct node *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct node, node);

	if (this->resource == NULL)
		return 0;

	spa_log_trace(this->log, "send command %s",
			spa_debug_type_find_name(spa_type_node_command_id, SPA_NODE_COMMAND_ID(command)));

	pw_client_node_resource_port_command(this->resource,
					     direction, port_id,
					     command);
	return 0;
}

static int impl_node_process(struct spa_node *node)
{
	struct node *this = SPA_CONTAINER_OF(node, struct node, node);
	struct impl *impl = this->impl;
	struct pw_driver_quantum *q, *rq;
	uint64_t cmd = 1;

	spa_log_trace(this->log, "%p: send process %p", this, impl->this.node->driver_node);

	q = impl->this.node->driver_node->rt.quantum;
	rq = SPA_MEMBER(impl->position, sizeof(struct pw_client_node_position),
			struct pw_driver_quantum);

	if (impl->start == -1)
		impl->start = q->position;
	*rq = *q;
	rq->position -= impl->start;

	if (write(this->writefd, &cmd, 8) != 8)
		spa_log_warn(this->log, "node %p: error %s", this, strerror(errno));

	return SPA_STATUS_OK;
}

static void
client_node_done(void *data, int seq, int res)
{
	struct impl *impl = data;
	struct node *this = &impl->node;

	this->callbacks->done(this->callbacks_data, seq, res);
}

static void
client_node_update(void *data,
		   uint32_t change_mask,
		   uint32_t max_input_ports,
		   uint32_t max_output_ports,
		   uint32_t n_params,
		   const struct spa_pod **params,
		   const struct spa_dict *props)
{
	struct impl *impl = data;
	struct node *this = &impl->node;

	if (change_mask & PW_CLIENT_NODE_UPDATE_MAX_INPUTS)
		this->max_inputs = max_input_ports;
	if (change_mask & PW_CLIENT_NODE_UPDATE_MAX_OUTPUTS)
		this->max_outputs = max_output_ports;
	if (change_mask & PW_CLIENT_NODE_UPDATE_PARAMS) {
		int i;
		spa_log_debug(this->log, "node %p: update %d params", this, n_params);

		for (i = 0; i < this->n_params; i++)
			free(this->params[i]);
		this->n_params = n_params;
		this->params = realloc(this->params, this->n_params * sizeof(struct spa_pod *));

		for (i = 0; i < this->n_params; i++)
			this->params[i] = pw_spa_pod_copy(params[i]);
	}
	if (change_mask & PW_CLIENT_NODE_UPDATE_PROPS) {
		pw_node_update_properties(impl->this.node, props);
	}
	spa_log_debug(this->log, "node %p: got node update max_in %u, max_out %u", this,
		     this->max_inputs, this->max_outputs);
}

static void
client_node_port_update(void *data,
			enum spa_direction direction,
			uint32_t port_id,
			uint32_t change_mask,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct spa_port_info *info)
{
	struct impl *impl = data;
	struct node *this = &impl->node;
	struct port *port;
	bool remove;

	spa_log_debug(this->log, "node %p: got port update", this);
	if (!CHECK_PORT_ID(this, direction, port_id))
		return;

	remove = (change_mask == 0);

	port = GET_PORT(this, direction, port_id);

	if (remove) {
		clear_port(this, port);
		pw_node_update_ports(impl->this.node);
	} else {
		struct port dummy = { 0 }, *target;

		if (port == NULL)
			target = &dummy;
		else
			target = port;

		do_update_port(this,
			       target,
			       change_mask,
			       n_params, params, info);

		if (port == NULL) {
			if (direction == SPA_DIRECTION_INPUT) {
				this->n_inputs++;
				this->in_ports[port_id] = &dummy;
			}
			else {
				this->n_outputs++;
				this->out_ports[port_id] = &dummy;
			}
			pw_node_update_ports(impl->this.node);
		}

	}
}

static void client_node_set_active(void *data, bool active)
{
	struct impl *impl = data;
	pw_node_set_active(impl->this.node, active);
}

static void client_node_event(void *data, struct spa_event *event)
{
	struct impl *impl = data;
	struct node *this = &impl->node;
	this->callbacks->event(this->callbacks_data, event);
}

static struct pw_client_node_proxy_methods client_node_methods = {
	PW_VERSION_CLIENT_NODE_PROXY_METHODS,
	.done = client_node_done,
	.update = client_node_update,
	.port_update = client_node_port_update,
	.set_active = client_node_set_active,
	.event = client_node_event,
};

static void node_on_data_fd_events(struct spa_source *source)
{
	struct node *this = source->data;

	if (source->rmask & (SPA_IO_ERR | SPA_IO_HUP)) {
		spa_log_warn(this->log, "node %p: got error", this);
		return;
	}

	if (source->rmask & SPA_IO_IN) {
		uint64_t cmd;

		if (read(this->data_source.fd, &cmd, sizeof(uint64_t)) != sizeof(uint64_t))
			spa_log_warn(this->log, "node %p: error reading message: %s",
					this, strerror(errno));

		spa_log_trace(this->log, "node %p: got process", this);
		this->callbacks->process(this->callbacks_data, SPA_STATUS_HAVE_BUFFER);
	}
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	NULL,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
	.send_command = impl_node_send_command,
	.set_callbacks = impl_node_set_callbacks,
	.get_n_ports = impl_node_get_n_ports,
	.get_port_ids = impl_node_get_port_ids,
	.add_port = impl_node_add_port,
	.remove_port = impl_node_remove_port,
	.port_get_info = impl_node_port_get_info,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_alloc_buffers = impl_node_port_alloc_buffers,
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.port_send_command = impl_node_port_send_command,
	.process = impl_node_process,
};

static int
node_init(struct node *this,
	  struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	uint32_t i;

	for (i = 0; i < n_support; i++) {
		switch (support[i].type) {
		case SPA_TYPE_INTERFACE_Log:
			this->log = support[i].data;
			break;
		case SPA_TYPE_INTERFACE_DataLoop:
			this->data_loop = support[i].data;
			break;
		default:
			break;
		}
	}
	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data-loop is needed");
		return -EINVAL;
	}

	this->node = impl_node;

	this->data_source.func = node_on_data_fd_events;
	this->data_source.data = this;
	this->data_source.fd = -1;
	this->data_source.mask = SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP;
	this->data_source.rmask = 0;

	this->seq = 1;

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int node_clear(struct node *this)
{
	uint32_t i;

	for (i = 0; i < this->n_params; i++)
		free(this->params[i]);
	free(this->params);

	return 0;
}

static int do_remove_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct spa_source *source = user_data;
	spa_loop_remove_source(loop, source);
	return 0;
}

static void client_node_resource_destroy(void *data)
{
	struct impl *impl = data;
	struct pw_client_node *this = &impl->this;
	struct node *node = &impl->node;

	pw_log_debug("client-node %p: destroy", impl);

	impl->node.resource = this->resource = NULL;
	spa_hook_remove(&impl->resource_listener);

	if (node->data_source.fd != -1) {
		spa_loop_invoke(node->data_loop,
				do_remove_source,
				SPA_ID_INVALID,
				NULL,
				0,
				true,
				&node->data_source);
	}
	pw_node_destroy(this->node);
}

void pw_client_node_registered(struct pw_client_node *this, uint32_t node_id)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);

	pw_log_debug("client-node %p: %d", this, node_id);
	pw_client_node_resource_transport(this->resource,
					  node_id,
					  impl->other_fds[0],
					  impl->other_fds[1]);
}

static void node_initialized(void *data)
{
	struct impl *impl = data;
	struct pw_client_node *this = &impl->this;
	struct pw_node *node = this->node;
	struct pw_global *global;
	uint32_t area_size, size;
	struct mem *m;

	if (this->resource == NULL)
		return;

	impl->fds[0] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	impl->fds[1] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	impl->node.data_source.fd = impl->fds[0];
	impl->node.writefd = impl->fds[1];
	impl->other_fds[0] = impl->fds[1];
	impl->other_fds[1] = impl->fds[0];

	spa_loop_add_source(impl->node.data_loop, &impl->node.data_source);
	pw_log_debug("client-node %p: transport fd %d %d", node, impl->fds[0], impl->fds[1]);

	area_size = sizeof(struct spa_io_buffers) * MAX_AREAS;
	size = area_size + sizeof(struct pw_client_node_position) + sizeof(struct pw_driver_quantum);

	if (pw_memblock_alloc(PW_MEMBLOCK_FLAG_WITH_FD |
			      PW_MEMBLOCK_FLAG_MAP_READWRITE |
			      PW_MEMBLOCK_FLAG_SEAL,
			      size,
			      &impl->io_areas) < 0)
                return;

	impl->position = SPA_MEMBER(impl->io_areas->ptr,
			area_size, struct pw_client_node_position);

	m = ensure_mem(impl, impl->io_areas->fd, SPA_DATA_MemFd, impl->io_areas->flags);
	pw_log_debug("client-node %p: io areas %p", node, impl->io_areas->ptr);

	pw_client_node_resource_set_io(this->resource,
				       PW_IO_ClientNodePosition,
				       m->id,
				       area_size,
				       sizeof(struct pw_client_node_position));

	if ((global = pw_node_get_global(node)) != NULL)
		pw_client_node_registered(this, pw_global_get_id(global));
}

static void node_free(void *data)
{
	struct impl *impl = data;

	pw_log_debug("client-node %p: free", &impl->this);
	node_clear(&impl->node);

	spa_hook_remove(&impl->node_listener);

	pw_array_clear(&impl->mems);
	if (impl->io_areas)
		pw_memblock_free(impl->io_areas);

	pw_map_clear(&impl->io_map);

	if (impl->fds[0] != -1)
		close(impl->fds[0]);
	if (impl->fds[1] != -1)
		close(impl->fds[1]);
	free(impl);
}

static int port_init_mix(void *data, struct pw_port_mix *mix)
{
	struct port *port = data;
	struct impl *impl = port->impl;
	struct mix *m;

	if ((m = ensure_mix(impl, port, mix->port.port_id)) == NULL)
		return -ENOMEM;

	mix->id = pw_map_insert_new(&impl->io_map, NULL);

	mix->io = SPA_MEMBER(impl->io_areas->ptr,
			mix->id * sizeof(struct spa_io_buffers), void);
	mix->io->buffer_id = SPA_ID_INVALID;
	mix->io->status = SPA_STATUS_NEED_BUFFER;

	pw_log_debug("client-node %p: init mix io %d %p %p", impl, mix->id, mix->io,
			impl->io_areas->ptr);

	return 0;
}

static int port_release_mix(void *data, struct pw_port_mix *mix)
{
	struct port *port = data;
	struct impl *impl = port->impl;
	struct node *this = &impl->node;
	struct mix *m;

	pw_log_debug("client-node %p: remove mix io %d %p %p", impl, mix->id, mix->io,
			impl->io_areas->ptr);

	if ((m = find_mix(port, mix->port.port_id)) == NULL || !m->valid)
		return -EINVAL;

	pw_map_remove(&impl->io_map, mix->id);
	mix_clear(this, m);
	return 0;
}

static const struct pw_port_implementation port_impl = {
	PW_VERSION_PORT_IMPLEMENTATION,
	.init_mix = port_init_mix,
	.release_mix = port_release_mix,
};

static int
impl_mix_port_enum_params(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t *index,
			   const struct spa_pod *filter,
			   struct spa_pod **result,
			   struct spa_pod_builder *builder)
{
	struct port *port = SPA_CONTAINER_OF(node, struct port, mix_node);
	return impl_node_port_enum_params(&port->node->node, direction, port->id,
			id, index, filter, result, builder);
}

static int
impl_mix_port_set_param(struct spa_node *node,
			enum spa_direction direction, uint32_t port_id,
			uint32_t id, uint32_t flags,
			const struct spa_pod *param)
{
	struct port *port = SPA_CONTAINER_OF(node, struct port, mix_node);
	return impl_node_port_set_param(&port->node->node, direction, port->id,
			id, flags, param);
}

static int
impl_mix_add_port(struct spa_node *node, enum spa_direction direction, uint32_t mix_id)
{
	struct port *port = SPA_CONTAINER_OF(node, struct port, mix_node);
	pw_log_debug("client-node %p: add port %d:%d.%d", node, direction, port->id, mix_id);
	return 0;
}

static int
impl_mix_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t mix_id)
{
	struct port *port = SPA_CONTAINER_OF(node, struct port, mix_node);
	pw_log_debug("client-node %p: remove port %d:%d.%d", node, direction, port->id, mix_id);
	return 0;
}

static int
impl_mix_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t mix_id,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct port *port = SPA_CONTAINER_OF(node, struct port, mix_node);
	struct impl *impl = port->impl;

	return do_port_use_buffers(impl, direction, port->id, mix_id, buffers, n_buffers);
}

static int
impl_mix_port_alloc_buffers(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    struct spa_pod **params,
			    uint32_t n_params,
			    struct spa_buffer **buffers,
			    uint32_t *n_buffers)
{
	return -ENOTSUP;
}

static int impl_mix_port_set_io(struct spa_node *node,
			   enum spa_direction direction, uint32_t mix_id,
			   uint32_t id, void *data, size_t size)
{
	struct port *p = SPA_CONTAINER_OF(node, struct port, mix_node);
	struct pw_port *port = p->port;
	struct impl *impl = port->owner_data;
	struct pw_port_mix *mix;

	mix = pw_map_lookup(&port->mix_port_map, mix_id);
	if (mix == NULL)
		return -EIO;

	if (id == SPA_IO_Buffers) {
		if (data && size >= sizeof(struct spa_io_buffers))
			mix->io = data;
		else
			mix->io = NULL;
	}

	return do_port_set_io(impl,
			      direction, port->port_id, mix->port.port_id,
			      id, data, size);
}

static int
impl_mix_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct port *p = SPA_CONTAINER_OF(node, struct port, mix_node);
	return impl_node_port_reuse_buffer(&p->node->node, p->id, buffer_id);
}

static int
impl_mix_port_send_command(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id, const struct spa_command *command)
{
	struct port *p = SPA_CONTAINER_OF(node, struct port, mix_node);
	return impl_node_port_send_command(&p->node->node, direction, p->id, command);
}

static int impl_mix_process(struct spa_node *data)
{
	return SPA_STATUS_HAVE_BUFFER;
}

static const struct spa_node impl_port_mix = {
	SPA_VERSION_NODE,
	NULL,
	.port_enum_params = impl_mix_port_enum_params,
	.port_set_param = impl_mix_port_set_param,
	.add_port = impl_mix_add_port,
	.remove_port = impl_mix_remove_port,
	.port_use_buffers = impl_mix_port_use_buffers,
	.port_alloc_buffers = impl_mix_port_alloc_buffers,
	.port_set_io = impl_mix_port_set_io,
	.port_reuse_buffer = impl_mix_port_reuse_buffer,
	.port_send_command = impl_mix_port_send_command,
	.process = impl_mix_process,
};

static void node_port_init(void *data, struct pw_port *port)
{
	struct impl *impl = data;
	struct port *p = pw_port_get_user_data(port), *dummy;
	struct node *this = &impl->node;

	pw_log_debug("client-node %p: port %p init", &impl->this, port);

	if (port->direction == PW_DIRECTION_INPUT)
		dummy = this->in_ports[port->port_id];
	else
		dummy = this->out_ports[port->port_id];

	*p = *dummy;
	p->port = port;
	p->node = this;
	p->direction = port->direction;
	p->id = port->port_id;
	p->impl = impl;
	p->mix_node = impl_port_mix;
	mix_init(&p->mix[MAX_MIX], p, SPA_ID_INVALID);

	if (p->direction == SPA_DIRECTION_INPUT)
		this->in_ports[p->id] = p;
	else
		this->out_ports[p->id] = p;

	return;
}

static void node_port_added(void *data, struct pw_port *port)
{
	struct impl *impl = data;
	struct port *p = pw_port_get_user_data(port);

	pw_port_set_mix(port, &p->mix_node,
			PW_PORT_MIX_FLAG_MULTI |
			PW_PORT_MIX_FLAG_MIX_ONLY);

	port->implementation = &port_impl;
	port->implementation_data = p;

	port->owner_data = impl;
}

static void node_port_removed(void *data, struct pw_port *port)
{
	struct impl *impl = data;
	struct node *this = &impl->node;
	struct port *p = pw_port_get_user_data(port);

	pw_log_debug("client-node %p: port %p remove", &impl->this, port);

	clear_port(this, p);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.free = node_free,
	.initialized = node_initialized,
	.port_init = node_port_init,
	.port_added = node_port_added,
	.port_removed = node_port_removed,
};

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = client_node_resource_destroy,
};

/** Create a new client node
 * \param client an owner \ref pw_client
 * \param id an id
 * \param name a name
 * \param properties extra properties
 * \return a newly allocated client node
 *
 * Create a new \ref pw_node.
 *
 * \memberof pw_client_node
 */
struct pw_client_node *pw_client_node_new(struct pw_resource *resource,
					  struct pw_global *parent,
					  struct pw_properties *properties,
					  bool do_register)
{
	struct impl *impl;
	struct pw_client_node *this;
	struct pw_client *client = pw_resource_get_client(resource);
	struct pw_core *core = pw_client_get_core(client);
	const struct spa_support *support;
	uint32_t n_support;
	const char *name;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	this = &impl->this;

	impl->core = core;
	impl->fds[0] = impl->fds[1] = -1;
	pw_log_debug("client-node %p: new", impl);

	support = pw_core_get_support(impl->core, &n_support);
	node_init(&impl->node, NULL, support, n_support);
	impl->node.impl = impl;

	pw_map_init(&impl->io_map, 64, 64);
	pw_array_init(&impl->mems, 64);

	if ((name = pw_properties_get(properties, "node.name")) == NULL)
		name = "client-node";

	this->resource = resource;
	this->parent = parent;
	this->node = pw_spa_node_new(core,
				     pw_resource_get_client(this->resource),
				     parent,
				     name,
				     PW_SPA_NODE_FLAG_ASYNC |
				     (do_register ? 0 : PW_SPA_NODE_FLAG_NO_REGISTER),
				     &impl->node.node,
				     NULL,
				     properties, 0);
	if (this->node == NULL)
		goto error_no_node;

	this->node->remote = true;

	pw_resource_add_listener(this->resource,
				 &impl->resource_listener,
				 &resource_events,
				 impl);
	pw_resource_set_implementation(this->resource,
				       &client_node_methods,
				       impl);

	impl->node.resource = this->resource;
	this->node->port_user_data_size = sizeof(struct port);

	pw_node_add_listener(this->node, &impl->node_listener, &node_events, impl);

	return this;

      error_no_node:
	pw_resource_destroy(this->resource);
	node_clear(&impl->node);
	free(impl);
	return NULL;
}

/** Destroy a client node
 * \param node the client node to destroy
 * \memberof pw_client_node
 */
void pw_client_node_destroy(struct pw_client_node *node)
{
	pw_resource_destroy(node->resource);
}
