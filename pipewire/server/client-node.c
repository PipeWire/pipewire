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

#include "spa/node.h"
#include "spa/format-builder.h"

#include "pipewire/client/pipewire.h"
#include "pipewire/client/interfaces.h"
#include "pipewire/client/transport.h"

#include "pipewire/server/core.h"
#include "pipewire/server/client-node.h"

/** \cond */

#define MAX_INPUTS       64
#define MAX_OUTPUTS      64

#define MAX_BUFFERS      64

#define CHECK_IN_PORT_ID(this,d,p)       ((d) == SPA_DIRECTION_INPUT && (p) < MAX_INPUTS)
#define CHECK_OUT_PORT_ID(this,d,p)      ((d) == SPA_DIRECTION_OUTPUT && (p) < MAX_OUTPUTS)
#define CHECK_PORT_ID(this,d,p)          (CHECK_IN_PORT_ID(this,d,p) || CHECK_OUT_PORT_ID(this,d,p))
#define CHECK_FREE_IN_PORT(this,d,p)     (CHECK_IN_PORT_ID(this,d,p) && !(this)->in_ports[p].valid)
#define CHECK_FREE_OUT_PORT(this,d,p)    (CHECK_OUT_PORT_ID(this,d,p) && !(this)->out_ports[p].valid)
#define CHECK_FREE_PORT(this,d,p)        (CHECK_FREE_IN_PORT (this,d,p) || CHECK_FREE_OUT_PORT (this,d,p))
#define CHECK_IN_PORT(this,d,p)          (CHECK_IN_PORT_ID(this,d,p) && (this)->in_ports[p].valid)
#define CHECK_OUT_PORT(this,d,p)         (CHECK_OUT_PORT_ID(this,d,p) && (this)->out_ports[p].valid)
#define CHECK_PORT(this,d,p)             (CHECK_IN_PORT (this,d,p) || CHECK_OUT_PORT (this,d,p))

#define CHECK_PORT_BUFFER(this,b,p)      (b < p->n_buffers)

struct proxy_buffer {
	struct spa_buffer *outbuf;
	struct spa_buffer buffer;
	struct spa_meta metas[4];
	struct spa_data datas[4];
	off_t offset;
	size_t size;
	bool outstanding;
};

struct proxy_port {
	bool valid;
	struct spa_port_info info;
	struct spa_format *format;
	uint32_t n_formats;
	struct spa_format **formats;
	uint32_t n_params;
	struct spa_param **params;
	struct spa_port_io *io;

	uint32_t n_buffers;
	struct proxy_buffer buffers[MAX_BUFFERS];

	uint32_t buffer_mem_id;
	struct pw_memblock buffer_mem;
};

struct proxy {
	struct spa_node node;

	struct impl *impl;

	struct spa_type_map *map;
	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_loop *data_loop;

	struct spa_node_callbacks callbacks;
	void *user_data;

	struct pw_resource *resource;

	struct spa_source data_source;
	int writefd;

	uint32_t max_inputs;
	uint32_t n_inputs;
	uint32_t max_outputs;
	uint32_t n_outputs;
	struct proxy_port in_ports[MAX_INPUTS];
	struct proxy_port out_ports[MAX_OUTPUTS];

	uint8_t format_buffer[1024];
	uint32_t seq;
};

struct impl {
	struct pw_client_node this;

	struct pw_core *core;

	struct proxy proxy;

	struct pw_transport *transport;

	struct pw_listener node_free;
	struct pw_listener initialized;
	struct pw_listener loop_changed;
	struct pw_listener global_added;

	int fds[2];
	int other_fds[2];
};

/** \endcond */

static int clear_buffers(struct proxy *this, struct proxy_port *port)
{
	if (port->n_buffers) {
		spa_log_info(this->log, "proxy %p: clear buffers", this);

		pw_memblock_free(&port->buffer_mem);

		port->n_buffers = 0;
	}
	return SPA_RESULT_OK;
}

static int spa_proxy_node_get_props(struct spa_node *node, struct spa_props **props)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int spa_proxy_node_set_props(struct spa_node *node, const struct spa_props *props)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static inline void do_flush(struct proxy *this)
{
	uint64_t cmd = 1;
	write(this->writefd, &cmd, 8);
}

static inline void send_need_input(struct proxy *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, proxy);

	pw_transport_add_event(impl->transport,
			       &SPA_EVENT_INIT(impl->core->type.event_transport.NeedInput));
	do_flush(this);
}

static inline void send_have_output(struct proxy *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, proxy);

	pw_transport_add_event(impl->transport,
			       &SPA_EVENT_INIT(impl->core->type.event_transport.HaveOutput));
	do_flush(this);
}

static int spa_proxy_node_send_command(struct spa_node *node, struct spa_command *command)
{
	struct proxy *this;
	int res = SPA_RESULT_OK;
	struct pw_core *core;

	if (node == NULL || command == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (this->resource == NULL)
		return SPA_RESULT_OK;

	core = this->impl->core;

	if (SPA_COMMAND_TYPE(command) == core->type.command_node.ClockUpdate) {
		pw_client_node_notify_node_command(this->resource, this->seq++, command);
	} else {
		/* send start */
		pw_client_node_notify_node_command(this->resource, this->seq, command);
		if (SPA_COMMAND_TYPE(command) == core->type.command_node.Start)
			send_need_input(this);

		res = SPA_RESULT_RETURN_ASYNC(this->seq++);
	}
	return res;
}

static int
spa_proxy_node_set_callbacks(struct spa_node *node,
			     const struct spa_node_callbacks *callbacks,
			     size_t callbacks_size, void *user_data)
{
	struct proxy *this;

	if (node == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	this = SPA_CONTAINER_OF(node, struct proxy, node);
	this->callbacks = *callbacks;
	this->user_data = user_data;

	return SPA_RESULT_OK;
}

static int
spa_proxy_node_get_n_ports(struct spa_node *node,
			   uint32_t *n_input_ports,
			   uint32_t *max_input_ports,
			   uint32_t *n_output_ports,
			   uint32_t *max_output_ports)
{
	struct proxy *this;

	if (node == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (n_input_ports)
		*n_input_ports = this->n_inputs;
	if (max_input_ports)
		*max_input_ports = this->max_inputs;
	if (n_output_ports)
		*n_output_ports = this->n_outputs;
	if (max_output_ports)
		*max_output_ports = this->max_outputs;

	return SPA_RESULT_OK;
}

static int
spa_proxy_node_get_port_ids(struct spa_node *node,
			    uint32_t n_input_ports,
			    uint32_t *input_ids,
			    uint32_t n_output_ports,
			    uint32_t *output_ids)
{
	struct proxy *this;
	int c, i;

	if (node == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (input_ids) {
		for (c = 0, i = 0; i < MAX_INPUTS && c < n_input_ports; i++) {
			if (this->in_ports[i].valid)
				input_ids[c++] = i;
		}
	}
	if (output_ids) {
		for (c = 0, i = 0; i < MAX_OUTPUTS && c < n_output_ports; i++) {
			if (this->out_ports[i].valid)
				output_ids[c++] = i;
		}
	}
	return SPA_RESULT_OK;
}

static void
do_update_port(struct proxy *this,
	       enum spa_direction direction,
	       uint32_t port_id,
	       uint32_t change_mask,
	       uint32_t n_possible_formats,
	       const struct spa_format **possible_formats,
	       const struct spa_format *format,
	       uint32_t n_params,
	       const struct spa_param **params,
	       const struct spa_port_info *info)
{
	struct proxy_port *port;
	uint32_t i;

	if (direction == SPA_DIRECTION_INPUT) {
		port = &this->in_ports[port_id];
	} else {
		port = &this->out_ports[port_id];
	}

	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_POSSIBLE_FORMATS) {
		for (i = 0; i < port->n_formats; i++)
			free(port->formats[i]);
		port->n_formats = n_possible_formats;
		port->formats =
		    realloc(port->formats, port->n_formats * sizeof(struct spa_format *));
		for (i = 0; i < port->n_formats; i++)
			port->formats[i] = spa_format_copy(possible_formats[i]);
	}
	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_FORMAT) {
		if (port->format)
			free(port->format);
		port->format = spa_format_copy(format);
	}

	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_PARAMS) {
		for (i = 0; i < port->n_params; i++)
			free(port->params[i]);
		port->n_params = n_params;
		port->params = realloc(port->params, port->n_params * sizeof(struct spa_param *));
		for (i = 0; i < port->n_params; i++)
			port->params[i] = spa_param_copy(params[i]);
	}

	if (change_mask & PW_CLIENT_NODE_PORT_UPDATE_INFO && info)
		port->info = *info;

	if (!port->valid) {
		spa_log_info(this->log, "proxy %p: adding port %d", this, port_id);
		port->format = NULL;
		port->valid = true;

		if (direction == SPA_DIRECTION_INPUT)
			this->n_inputs++;
		else
			this->n_outputs++;
	}
}

static void
clear_port(struct proxy *this,
	   struct proxy_port *port, enum spa_direction direction, uint32_t port_id)
{
	do_update_port(this,
		       direction,
		       port_id,
		       PW_CLIENT_NODE_PORT_UPDATE_POSSIBLE_FORMATS |
		       PW_CLIENT_NODE_PORT_UPDATE_FORMAT |
		       PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
		       PW_CLIENT_NODE_PORT_UPDATE_INFO, 0, NULL, NULL, 0, NULL, NULL);
	clear_buffers(this, port);
}

static void do_uninit_port(struct proxy *this, enum spa_direction direction, uint32_t port_id)
{
	struct proxy_port *port;

	spa_log_info(this->log, "proxy %p: removing port %d", this, port_id);
	if (direction == SPA_DIRECTION_INPUT) {
		port = &this->in_ports[port_id];
		this->n_inputs--;
	} else {
		port = &this->out_ports[port_id];
		this->n_outputs--;
	}
	clear_port(this, port, direction, port_id);
	port->valid = false;
}

static int
spa_proxy_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct proxy *this;
	struct proxy_port *port;

	if (node == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (!CHECK_FREE_PORT(this, direction, port_id))
		return SPA_RESULT_INVALID_PORT;

	port =
	    direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];
	clear_port(this, port, direction, port_id);

	return SPA_RESULT_OK;
}

static int
spa_proxy_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct proxy *this;

	if (node == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (!CHECK_PORT(this, direction, port_id))
		return SPA_RESULT_INVALID_PORT;

	do_uninit_port(this, direction, port_id);

	return SPA_RESULT_OK;
}

static int
spa_proxy_node_port_enum_formats(struct spa_node *node,
				 enum spa_direction direction,
				 uint32_t port_id,
				 struct spa_format **format,
				 const struct spa_format *filter,
				 uint32_t index)
{
	struct proxy *this;
	struct proxy_port *port;
	struct spa_format *fmt;
	struct spa_pod_builder b = { NULL, };
	int res;
	uint32_t count, match = 0;

	if (node == NULL || format == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (!CHECK_PORT(this, direction, port_id))
		return SPA_RESULT_INVALID_PORT;

	port =
	    direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

	count = match = filter ? 0 : index;

      next:
	if (count >= port->n_formats)
		return SPA_RESULT_ENUM_END;

	fmt = port->formats[count++];

	spa_pod_builder_init(&b, this->format_buffer, sizeof(this->format_buffer));

	if ((res = spa_format_filter(fmt, filter, &b)) != SPA_RESULT_OK || match++ != index)
		goto next;

	*format = SPA_POD_BUILDER_DEREF(&b, 0, struct spa_format);

	return SPA_RESULT_OK;
}

static int
spa_proxy_node_port_set_format(struct spa_node *node,
			       enum spa_direction direction,
			       uint32_t port_id, uint32_t flags, const struct spa_format *format)
{
	struct proxy *this;

	if (node == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (!CHECK_PORT(this, direction, port_id))
		return SPA_RESULT_INVALID_PORT;

	if (this->resource == NULL)
		return SPA_RESULT_OK;

	pw_client_node_notify_set_format(this->resource,
					 this->seq, direction, port_id, flags, format);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int
spa_proxy_node_port_get_format(struct spa_node *node,
			       enum spa_direction direction,
			       uint32_t port_id, const struct spa_format **format)
{
	struct proxy *this;
	struct proxy_port *port;

	if (node == NULL || format == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (!CHECK_PORT(this, direction, port_id))
		return SPA_RESULT_INVALID_PORT;

	port =
	    direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

	if (!port->format)
		return SPA_RESULT_NO_FORMAT;

	*format = port->format;

	return SPA_RESULT_OK;
}

static int
spa_proxy_node_port_get_info(struct spa_node *node,
			     enum spa_direction direction,
			     uint32_t port_id, const struct spa_port_info **info)
{
	struct proxy *this;
	struct proxy_port *port;

	if (node == NULL || info == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (!CHECK_PORT(this, direction, port_id))
		return SPA_RESULT_INVALID_PORT;

	port =
	    direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

	*info = &port->info;

	return SPA_RESULT_OK;
}

static int
spa_proxy_node_port_enum_params(struct spa_node *node,
				enum spa_direction direction,
				uint32_t port_id, uint32_t index, struct spa_param **param)
{
	struct proxy *this;
	struct proxy_port *port;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(param != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	port =
	    direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

	if (index >= port->n_params)
		return SPA_RESULT_ENUM_END;

	*param = port->params[index];

	return SPA_RESULT_OK;
}

static int
spa_proxy_node_port_set_param(struct spa_node *node,
			      enum spa_direction direction,
			      uint32_t port_id, const struct spa_param *param)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_proxy_node_port_set_io(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id, struct spa_port_io *io)
{
	struct proxy *this;
	struct proxy_port *port;

	if (node == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (!CHECK_PORT(this, direction, port_id))
		return SPA_RESULT_INVALID_PORT;

	port =
	    direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];
	port->io = io;

	return SPA_RESULT_OK;
}

static int
spa_proxy_node_port_use_buffers(struct spa_node *node,
				enum spa_direction direction,
				uint32_t port_id,
				struct spa_buffer **buffers,
				uint32_t n_buffers)
{
	struct proxy *this;
	struct impl *impl;
	struct proxy_port *port;
	uint32_t i, j;
	size_t n_mem;
	struct pw_client_node_buffer *mb;
	struct spa_meta_shared *msh;

	this = SPA_CONTAINER_OF(node, struct proxy, node);
	impl = this->impl;
	spa_log_info(this->log, "proxy %p: use buffers %p %u", this, buffers, n_buffers);

	if (!CHECK_PORT(this, direction, port_id))
		return SPA_RESULT_INVALID_PORT;

	port =
	    direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

	if (!port->format)
		return SPA_RESULT_NO_FORMAT;

	clear_buffers(this, port);

	if (n_buffers > 0) {
		mb = alloca(n_buffers * sizeof(struct pw_client_node_buffer));
	} else {
		mb = NULL;
	}

	port->n_buffers = n_buffers;

	if (this->resource == NULL)
		return SPA_RESULT_OK;

	n_mem = 0;
	for (i = 0; i < n_buffers; i++) {
		struct proxy_buffer *b = &port->buffers[i];

		msh = spa_buffer_find_meta(buffers[i], impl->core->type.meta.Shared);
		if (msh == NULL) {
			spa_log_error(this->log, "missing shared metadata on buffer %d", i);
			return SPA_RESULT_ERROR;
		}

		b->outbuf = buffers[i];
		memcpy(&b->buffer, buffers[i], sizeof(struct spa_buffer));
		b->buffer.datas = b->datas;
		b->buffer.metas = b->metas;

		mb[i].buffer = &b->buffer;
		mb[i].mem_id = n_mem++;
		mb[i].offset = 0;
		mb[i].size = msh->size;

		pw_client_node_notify_add_mem(this->resource,
					      direction,
					      port_id,
					      mb[i].mem_id,
					      impl->core->type.data.MemFd,
					      msh->fd, msh->flags, msh->offset, msh->size);

		for (j = 0; j < buffers[i]->n_metas; j++) {
			memcpy(&b->buffer.metas[j], &buffers[i]->metas[j], sizeof(struct spa_meta));
		}

		for (j = 0; j < buffers[i]->n_datas; j++) {
			struct spa_data *d = &buffers[i]->datas[j];

			memcpy(&b->buffer.datas[j], d, sizeof(struct spa_data));

			if (d->type == impl->core->type.data.DmaBuf ||
			    d->type == impl->core->type.data.MemFd) {
				pw_client_node_notify_add_mem(this->resource,
							      direction,
							      port_id,
							      n_mem,
							      d->type,
							      d->fd,
							      d->flags, d->mapoffset, d->maxsize);
				b->buffer.datas[j].type = impl->core->type.data.Id;
				b->buffer.datas[j].data = SPA_UINT32_TO_PTR(n_mem);
				n_mem++;
			} else if (d->type == impl->core->type.data.MemPtr) {
				b->buffer.datas[j].data = SPA_INT_TO_PTR(b->size);
				b->size += d->maxsize;
			} else {
				b->buffer.datas[j].type = SPA_ID_INVALID;
				b->buffer.datas[j].data = 0;
				spa_log_error(this->log, "invalid memory type %d", d->type);
			}
		}
	}

	pw_client_node_notify_use_buffers(this->resource,
					  this->seq, direction, port_id, n_buffers, mb);

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static int
spa_proxy_node_port_alloc_buffers(struct spa_node *node,
				  enum spa_direction direction,
				  uint32_t port_id,
				  struct spa_param **params,
				  uint32_t n_params,
				  struct spa_buffer **buffers,
				  uint32_t *n_buffers)
{
	struct proxy *this;
	struct proxy_port *port;

	if (node == NULL || buffers == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	if (!CHECK_PORT(this, direction, port_id))
		return SPA_RESULT_INVALID_PORT;

	port =
	    direction == SPA_DIRECTION_INPUT ? &this->in_ports[port_id] : &this->out_ports[port_id];

	if (!port->format)
		return SPA_RESULT_NO_FORMAT;

	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_proxy_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct proxy *this;
	struct impl *impl;

	if (node == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	this = SPA_CONTAINER_OF(node, struct proxy, node);
	impl = this->impl;

	if (!CHECK_OUT_PORT(this, SPA_DIRECTION_OUTPUT, port_id))
		return SPA_RESULT_INVALID_PORT;

	spa_log_trace(this->log, "reuse buffer %d", buffer_id);
	{
		struct pw_event_transport_reuse_buffer rb = PW_EVENT_TRANSPORT_REUSE_BUFFER_INIT
		    (impl->core->type.event_transport.ReuseBuffer, port_id, buffer_id);
		pw_transport_add_event(impl->transport, (struct spa_event *) &rb);
	}

	return SPA_RESULT_OK;
}

static int
spa_proxy_node_port_send_command(struct spa_node *node,
				 enum spa_direction direction,
				 uint32_t port_id, struct spa_command *command)
{
	struct proxy *this;

	if (node == NULL || command == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	this = SPA_CONTAINER_OF(node, struct proxy, node);

	spa_log_warn(this->log, "unhandled command %d", SPA_COMMAND_TYPE(command));
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int spa_proxy_node_process_input(struct spa_node *node)
{
	struct impl *impl;
	struct proxy *this;
	int i;

	if (node == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	this = SPA_CONTAINER_OF(node, struct proxy, node);
	impl = this->impl;

	for (i = 0; i < MAX_INPUTS; i++) {
		struct spa_port_io *io = this->in_ports[i].io;

		if (!io)
			continue;

		pw_log_trace("%d %d", io->status, io->buffer_id);

		impl->transport->inputs[i] = *io;
		io->status = SPA_RESULT_OK;
	}
	send_have_output(this);

	return SPA_RESULT_OK;
}

static int spa_proxy_node_process_output(struct spa_node *node)
{
	struct proxy *this;
	struct impl *impl;
	int i;
	bool send_need = false, flush = false;

	this = SPA_CONTAINER_OF(node, struct proxy, node);
	impl = this->impl;

	for (i = 0; i < MAX_OUTPUTS; i++) {
		struct spa_port_io *io = this->out_ports[i].io, tmp;

		if (!io)
			continue;

		if (io->buffer_id != SPA_ID_INVALID) {
			struct pw_event_transport_reuse_buffer rb =
			    PW_EVENT_TRANSPORT_REUSE_BUFFER_INIT(impl->core->type.event_transport.
								 ReuseBuffer, i, io->buffer_id);

			spa_log_trace(this->log, "reuse buffer %d", io->buffer_id);

			pw_transport_add_event(impl->transport, (struct spa_event *) &rb);
			io->buffer_id = SPA_ID_INVALID;
			flush = true;
		}

		tmp = impl->transport->outputs[i];
		impl->transport->outputs[i] = *io;

		pw_log_trace("%d %d  %d %d", io->status, io->buffer_id, tmp.status, tmp.buffer_id);

		if (io->status == SPA_RESULT_NEED_BUFFER)
			send_need = true;

		*io = tmp;
	}
	if (send_need)
		send_need_input(this);
	else if (flush)
		do_flush(this);

	return SPA_RESULT_HAVE_BUFFER;
}

static int handle_node_event(struct proxy *this, struct spa_event *event)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, proxy);
	int i;

	if (SPA_EVENT_TYPE(event) == impl->core->type.event_transport.HaveOutput) {
		for (i = 0; i < MAX_OUTPUTS; i++) {
			struct spa_port_io *io = this->out_ports[i].io;

			if (!io)
				continue;

			*io = impl->transport->outputs[i];
			pw_log_trace("%d %d", io->status, io->buffer_id);
		}
		this->callbacks.have_output(&this->node, this->user_data);
	} else if (SPA_EVENT_TYPE(event) == impl->core->type.event_transport.NeedInput) {
		this->callbacks.need_input(&this->node, this->user_data);
	} else if (SPA_EVENT_TYPE(event) == impl->core->type.event_transport.ReuseBuffer) {
		struct pw_event_transport_reuse_buffer *p =
		    (struct pw_event_transport_reuse_buffer *) event;
		this->callbacks.reuse_buffer(&this->node, p->body.port_id.value,
					     p->body.buffer_id.value, this->user_data);
	}
	return SPA_RESULT_OK;
}

static void
client_node_done(void *object, int seq, int res)
{
	struct pw_resource *resource = object;
	struct pw_client_node *node = resource->object;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	struct proxy *this = &impl->proxy;

	this->callbacks.done(&this->node, seq, res, this->user_data);
}


static void
client_node_update(void *object,
		   uint32_t change_mask,
		   uint32_t max_input_ports,
		   uint32_t max_output_ports, const struct spa_props *props)
{
	struct pw_resource *resource = object;
	struct pw_client_node *node = resource->object;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	struct proxy *this = &impl->proxy;

	if (change_mask & PW_CLIENT_NODE_UPDATE_MAX_INPUTS)
		this->max_inputs = max_input_ports;
	if (change_mask & PW_CLIENT_NODE_UPDATE_MAX_OUTPUTS)
		this->max_outputs = max_output_ports;

	spa_log_info(this->log, "proxy %p: got node update max_in %u, max_out %u", this,
		     this->max_inputs, this->max_outputs);
}

static void
client_node_port_update(void *object,
			enum spa_direction direction,
			uint32_t port_id,
			uint32_t change_mask,
			uint32_t n_possible_formats,
			const struct spa_format **possible_formats,
			const struct spa_format *format,
			uint32_t n_params,
			const struct spa_param **params, const struct spa_port_info *info)
{
	struct pw_resource *resource = object;
	struct pw_client_node *node = resource->object;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	struct proxy *this = &impl->proxy;
	bool remove;

	spa_log_info(this->log, "proxy %p: got port update", this);
	if (!CHECK_PORT_ID(this, direction, port_id))
		return;

	remove = (change_mask == 0);

	if (remove) {
		do_uninit_port(this, direction, port_id);
	} else {
		do_update_port(this,
			       direction,
			       port_id,
			       change_mask,
			       n_possible_formats,
			       possible_formats, format, n_params, params, info);
	}
}

static void client_node_event(void *object, struct spa_event *event)
{
	struct pw_resource *resource = object;
	struct pw_client_node *node = resource->object;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	struct proxy *this = &impl->proxy;

	this->callbacks.event(&this->node, event, this->user_data);
}

static void client_node_destroy(void *object)
{
	struct pw_resource *resource = object;
	struct pw_client_node *node = resource->object;
	pw_client_node_destroy(node);
}

static struct pw_client_node_methods client_node_methods = {
	&client_node_done,
	&client_node_update,
	&client_node_port_update,
	&client_node_event,
	&client_node_destroy,
};

static void proxy_on_data_fd_events(struct spa_source *source)
{
	struct proxy *this = source->data;
	struct impl *impl = this->impl;

	if (source->rmask & (SPA_IO_ERR | SPA_IO_HUP)) {
		spa_log_warn(this->log, "proxy %p: got error", this);
		return;
	}

	if (source->rmask & SPA_IO_IN) {
		struct spa_event event;
		uint64_t cmd;

		read(this->data_source.fd, &cmd, 8);

		while (pw_transport_next_event(impl->transport, &event) == SPA_RESULT_OK) {
			struct spa_event *ev = alloca(SPA_POD_SIZE(&event));
			pw_transport_parse_event(impl->transport, ev);
			handle_node_event(this, ev);
		}
	}
}

static const struct spa_node proxy_node = {
	sizeof(struct spa_node),
	NULL,
	spa_proxy_node_get_props,
	spa_proxy_node_set_props,
	spa_proxy_node_send_command,
	spa_proxy_node_set_callbacks,
	spa_proxy_node_get_n_ports,
	spa_proxy_node_get_port_ids,
	spa_proxy_node_add_port,
	spa_proxy_node_remove_port,
	spa_proxy_node_port_enum_formats,
	spa_proxy_node_port_set_format,
	spa_proxy_node_port_get_format,
	spa_proxy_node_port_get_info,
	spa_proxy_node_port_enum_params,
	spa_proxy_node_port_set_param,
	spa_proxy_node_port_use_buffers,
	spa_proxy_node_port_alloc_buffers,
	spa_proxy_node_port_set_io,
	spa_proxy_node_port_reuse_buffer,
	spa_proxy_node_port_send_command,
	spa_proxy_node_process_input,
	spa_proxy_node_process_output,
};

static int
proxy_init(struct proxy *this,
	   struct spa_dict *info,
	   const struct spa_support *support,
	   uint32_t n_support)
{
	uint32_t i;

	for (i = 0; i < n_support; i++) {
		if (strcmp(support[i].type, SPA_TYPE__Log) == 0)
			this->log = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE_LOOP__MainLoop) == 0)
			this->main_loop = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE_LOOP__DataLoop) == 0)
			this->data_loop = support[i].data;
	}
	if (this->main_loop == NULL) {
		spa_log_error(this->log, "a main-loop is needed");
	}
	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data-loop is needed");
	}

	this->node = proxy_node;

	this->data_source.func = proxy_on_data_fd_events;
	this->data_source.data = this;
	this->data_source.fd = -1;
	this->data_source.mask = SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP;
	this->data_source.rmask = 0;

	return SPA_RESULT_RETURN_ASYNC(this->seq++);
}

static void on_initialized(struct pw_listener *listener, struct pw_node *node)
{
	struct impl *impl = SPA_CONTAINER_OF(listener, struct impl, initialized);
	struct pw_client_node *this = &impl->this;
	struct pw_transport_info info;
	int readfd, writefd;

	if (this->resource == NULL)
		return;

	impl->transport = pw_transport_new(node->info.max_input_ports, node->info.max_output_ports);
	impl->transport->area->n_input_ports = node->info.n_input_ports;
	impl->transport->area->n_output_ports = node->info.n_output_ports;

	pw_client_node_get_fds(this, &readfd, &writefd);
	pw_transport_get_info(impl->transport, &info);

	pw_client_node_notify_transport(this->resource, readfd, writefd, info.memfd, info.offset, info.size);
}

static void on_loop_changed(struct pw_listener *listener, struct pw_node *node)
{
	struct impl *impl = SPA_CONTAINER_OF(listener, struct impl, loop_changed);
	impl->proxy.data_loop = node->data_loop->loop->loop;
}

static void
on_global_added(struct pw_listener *listener, struct pw_core *core, struct pw_global *global)
{
	struct impl *impl = SPA_CONTAINER_OF(listener, struct impl, global_added);

	if (global->object == impl->this.node)
		global->owner = impl->this.client;
}

static int proxy_clear(struct proxy *this)
{
	uint32_t i;

	for (i = 0; i < MAX_INPUTS; i++) {
		if (this->in_ports[i].valid)
			clear_port(this, &this->in_ports[i], SPA_DIRECTION_INPUT, i);
	}
	for (i = 0; i < MAX_OUTPUTS; i++) {
		if (this->out_ports[i].valid)
			clear_port(this, &this->out_ports[i], SPA_DIRECTION_OUTPUT, i);
	}

	return SPA_RESULT_OK;
}

static void client_node_resource_destroy(struct pw_resource *resource)
{
	struct pw_client_node *this = resource->object;
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct proxy *proxy = &impl->proxy;

	pw_log_debug("client-node %p: destroy", impl);
	pw_signal_emit(&this->destroy_signal, this);

	impl->proxy.resource = this->resource = NULL;

	pw_signal_remove(&impl->global_added);
	pw_signal_remove(&impl->loop_changed);
	pw_signal_remove(&impl->initialized);

	if (proxy->data_source.fd != -1)
		spa_loop_remove_source(proxy->data_loop, &proxy->data_source);

	pw_node_destroy(this->node);
}

static void on_node_free(struct pw_listener *listener, struct pw_node *node)
{
	struct impl *impl = SPA_CONTAINER_OF(listener, struct impl, node_free);

	pw_log_debug("client-node %p: free", &impl->this);
	proxy_clear(&impl->proxy);

	pw_signal_remove(&impl->node_free);

	if (impl->transport)
		pw_transport_destroy(impl->transport);

	if (impl->fds[0] != -1)
		close(impl->fds[0]);
	if (impl->fds[1] != -1)
		close(impl->fds[1]);
	free(impl);
}

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
struct pw_client_node *pw_client_node_new(struct pw_client *client,
					  uint32_t id,
					  const char *name,
					  struct pw_properties *properties)
{
	struct impl *impl;
	struct pw_client_node *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	this->client = client;

	impl->core = client->core;
	impl->fds[0] = impl->fds[1] = -1;
	pw_log_debug("client-node %p: new", impl);

	pw_signal_init(&this->destroy_signal);

	proxy_init(&impl->proxy, NULL, client->core->support, client->core->n_support);

	this->node = pw_node_new(client->core,
				 client, name, true, &impl->proxy.node, NULL, properties);
	if (this->node == NULL)
		goto error_no_node;

	impl->proxy.impl = impl;

	this->resource = pw_resource_new(client,
					 id,
					 client->core->type.client_node,
					 this,
					 &client_node_methods,
					 (pw_destroy_t) client_node_resource_destroy);
	if (this->resource == NULL)
		goto error_no_resource;

	impl->proxy.resource = this->resource;

	pw_signal_add(&this->node->free_signal, &impl->node_free, on_node_free);
	pw_signal_add(&this->node->initialized, &impl->initialized, on_initialized);
	pw_signal_add(&this->node->loop_changed, &impl->loop_changed, on_loop_changed);
	pw_signal_add(&impl->core->global_added, &impl->global_added, on_global_added);

	return this;

      error_no_resource:
	pw_node_destroy(this->node);
      error_no_node:
	proxy_clear(&impl->proxy);
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

/** Get the set of fds for this \ref pw_client_node
 *
 * \param node a \ref pw_client_node
 * \param[out] readfd an fd for reading
 * \param[out] writefd an fd for writing
 * \return 0 on success < 0 on error
 *
 * Create or return a previously created set of fds for \a node.
 *
 * \memberof pw_client_node
 */
int pw_client_node_get_fds(struct pw_client_node *node, int *readfd, int *writefd)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);

	if (impl->fds[0] == -1) {
#if 0
		if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, impl->fds) !=
		    0)
			return SPA_RESULT_ERRNO;

		impl->proxy.data_source.fd = impl->fds[0];
		impl->proxy.writefd = impl->fds[0];
		impl->other_fds[0] = impl->fds[1];
		impl->other_fds[1] = impl->fds[1];
#else
		impl->fds[0] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		impl->fds[1] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		impl->proxy.data_source.fd = impl->fds[0];
		impl->proxy.writefd = impl->fds[1];
		impl->other_fds[0] = impl->fds[1];
		impl->other_fds[1] = impl->fds[0];
#endif

		spa_loop_add_source(impl->proxy.data_loop, &impl->proxy.data_source);
		pw_log_debug("client-node %p: add data fd %d", node, impl->proxy.data_source.fd);
	}
	*readfd = impl->other_fds[0];
	*writefd = impl->other_fds[1];

	return SPA_RESULT_OK;
}
