/* Spa
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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
#include <string.h>
#include <stdio.h>

#include <spa/support/log.h>
#include <spa/utils/list.h>
#include <spa/node/node.h>
#include <spa/buffer/alloc.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/pod/filter.h>
#include <spa/debug/pod.h>
#include <spa/debug/types.h>

#define NAME "audioconvert"

#define PROP_DEFAULT_TRUNCATE	false
#define PROP_DEFAULT_DITHER	0

struct props {
	bool truncate;
	uint32_t dither;
};

static void props_reset(struct props *props)
{
	props->truncate = PROP_DEFAULT_TRUNCATE;
	props->dither = PROP_DEFAULT_DITHER;
}

struct buffer {
	struct spa_list link;
#define BUFFER_FLAG_OUT		(1 << 0)
	uint32_t flags;
	struct spa_buffer *outbuf;
	struct spa_meta_header *h;
};

struct link {
	struct spa_node *out_node;
	uint32_t out_port;
	const struct spa_port_info *out_info;
	struct spa_node *in_node;
	uint32_t in_port;
	const struct spa_port_info *in_info;
	struct spa_io_buffers io;
	bool negotiated;
	uint32_t n_buffers;
	struct spa_buffer **buffers;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;

	struct props props;

	const struct spa_node_callbacks *callbacks;
	void *user_data;

	int n_links;
	struct link links[8];
	int n_nodes;
	struct spa_node *nodes[8];

	bool started;

	struct spa_handle *hnd_fmt[2];
	struct spa_handle *hnd_channelmix;
	struct spa_handle *hnd_resample;

	struct spa_node *fmt[2];
	struct spa_node *channelmix;
	struct spa_node *resample;
};

static int make_link(struct impl *this,
		struct spa_node *out_node, uint32_t out_port,
		struct spa_node *in_node, uint32_t in_port,
		struct spa_audio_info *info)
{
	struct link *l = &this->links[this->n_links++];

	l->out_node = out_node;
	l->out_port = out_port;
	l->in_node = in_node;
	l->in_port = in_port;
	l->negotiated = false;
	l->io.status = SPA_STATUS_NEED_BUFFER;
	l->io.buffer_id = SPA_ID_INVALID;
	l->n_buffers = 0;

	spa_node_port_get_info(out_node,
			       SPA_DIRECTION_OUTPUT, out_port,
			       &l->out_info);
	spa_node_port_set_io(out_node,
			     SPA_DIRECTION_OUTPUT, out_port,
			     SPA_IO_Buffers,
			     &l->io, sizeof(l->io));
	spa_node_port_get_info(in_node,
			       SPA_DIRECTION_INPUT, in_port,
			       &l->in_info);
	spa_node_port_set_io(in_node,
			     SPA_DIRECTION_INPUT, in_port,
			     SPA_IO_Buffers,
			     &l->io, sizeof(l->io));
	return 0;
}

static void clean_link(struct impl *this, struct link *link)
{
	spa_node_port_set_param(link->in_node,
				SPA_DIRECTION_INPUT, link->in_port,
				SPA_PARAM_Format, 0, NULL);
	spa_node_port_set_param(link->out_node,
				SPA_DIRECTION_OUTPUT, link->out_port,
				SPA_PARAM_Format, 0, NULL);
	if (link->buffers)
		free(link->buffers);
	link->buffers = NULL;
}

static int debug_params(struct impl *this, struct spa_node *node,
		enum spa_direction direction, uint32_t port_id, uint32_t id, struct spa_pod *filter)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];
	uint32_t state;
	struct spa_pod *param;
	int res;

	spa_log_error(this->log, "params:");

	state = 0;
	while (true) {
		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		res = spa_node_port_enum_params(node,
				       direction, port_id,
				       id, &state,
				       NULL, &param, &b);
		if (res <= 0)
			break;

		spa_debug_pod(2, NULL, param);
	}

	spa_log_error(this->log, "failed filter:");
	if (filter)
		spa_debug_pod(2, NULL, filter);

	return 0;
}

static int negotiate_link_format(struct impl *this, struct link *link)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];
	uint32_t state;
	struct spa_pod *format, *filter;
	int res;

	if (link->negotiated)
		return 0;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	state = 0;
	filter = NULL;
	if ((res = spa_node_port_enum_params(link->out_node,
			       SPA_DIRECTION_OUTPUT, link->out_port,
			       SPA_PARAM_EnumFormat, &state,
			       filter, &format, &b)) <= 0) {
		debug_params(this, link->out_node, SPA_DIRECTION_OUTPUT, link->out_port,
				SPA_PARAM_EnumFormat, filter);
		return -ENOTSUP;
	}
	filter = format;
	state = 0;
	if ((res = spa_node_port_enum_params(link->in_node,
			       SPA_DIRECTION_INPUT, link->in_port,
			       SPA_PARAM_EnumFormat, &state,
			       filter, &format, &b)) <= 0) {
		debug_params(this, link->in_node, SPA_DIRECTION_INPUT, link->in_port,
				SPA_PARAM_EnumFormat, filter);
		return -ENOTSUP;
	}
	filter = format;

	spa_pod_fixate(filter);

	if ((res = spa_node_port_set_param(link->out_node,
				   SPA_DIRECTION_OUTPUT, link->out_port,
				   SPA_PARAM_Format, 0,
				   filter)) < 0)
		return res;

	if ((res = spa_node_port_set_param(link->in_node,
				   SPA_DIRECTION_INPUT, link->in_port,
				   SPA_PARAM_Format, 0,
				   filter)) < 0)
		return res;

	link->negotiated = true;

	return 0;
}

static int setup_convert(struct impl *this)
{
	int i, j, res;

	if (this->n_links > 0)
		return 0;

	this->n_nodes = 0;
	/* unpack */
	this->nodes[this->n_nodes++] = this->fmt[SPA_DIRECTION_INPUT];
	/* down mix */
	this->nodes[this->n_nodes++] = this->channelmix;
	/* resample */
	this->nodes[this->n_nodes++] = this->resample;
	/* pack */
	this->nodes[this->n_nodes++] = this->fmt[SPA_DIRECTION_OUTPUT];

	for (i = 0; i < this->n_nodes - 1; i++)
		make_link(this, this->nodes[i], 0, this->nodes[i+1], 0, NULL);

	for (i = 0, j = this->n_links - 1; j >= i; i++, j--) {
		if ((res = negotiate_link_format(this, &this->links[i])) < 0)
			return res;
		if ((res = negotiate_link_format(this, &this->links[j])) < 0)
			return res;
	}
	return 0;
}

static int negotiate_link_buffers(struct impl *this, struct link *link)
{
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t state;
	struct spa_pod *param = NULL;
	int res, i;
	bool in_alloc, out_alloc;
	int32_t size, buffers, blocks, align, flags;
	uint32_t *aligns;
	struct spa_data *datas;

	if (link->n_buffers > 0)
		return 0;

	state = 0;
	if ((res = spa_node_port_enum_params(link->in_node,
			       SPA_DIRECTION_INPUT, link->in_port,
			       SPA_PARAM_Buffers, &state,
			       param, &param, &b)) <= 0) {
		debug_params(this, link->out_node, SPA_DIRECTION_OUTPUT, link->out_port,
				SPA_PARAM_Buffers, param);
		return -ENOTSUP;
	}
	state = 0;
	if ((res = spa_node_port_enum_params(link->out_node,
			       SPA_DIRECTION_OUTPUT, link->out_port,
			       SPA_PARAM_Buffers, &state,
			       param, &param, &b)) <= 0) {
		debug_params(this, link->in_node, SPA_DIRECTION_INPUT, link->in_port,
				SPA_PARAM_Buffers, param);
		return -ENOTSUP;
	}

	spa_pod_fixate(param);

	if (link->in_info)
		in_alloc = SPA_FLAG_CHECK(link->in_info->flags,
				SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS);
	else
		in_alloc = false;

	if (link->out_info)
		out_alloc = SPA_FLAG_CHECK(link->out_info->flags,
				SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS);
	else
		out_alloc = false;

	flags = 0;
	if (out_alloc || in_alloc) {
		flags |= SPA_BUFFER_ALLOC_FLAG_NO_DATA;
		if (out_alloc)
			in_alloc = false;
	}

	if (spa_pod_object_parse(param,
		":", SPA_PARAM_BUFFERS_buffers, "i", &buffers,
		":", SPA_PARAM_BUFFERS_blocks, "i", &blocks,
		":", SPA_PARAM_BUFFERS_size, "i", &size,
		":", SPA_PARAM_BUFFERS_align, "i", &align,
		NULL) < 0)
		return -EINVAL;

	spa_log_debug(this->log, "%p: buffers %d, blocks %d, size %d, align %d",
			this, buffers, blocks, size, align);

	datas = alloca(sizeof(struct spa_data) * blocks);
	memset(datas, 0, sizeof(struct spa_data) * blocks);
	aligns = alloca(sizeof(uint32_t) * blocks);
	for (i = 0; i < blocks; i++) {
		datas[i].type = SPA_DATA_MemPtr;
		datas[i].maxsize = size;
		aligns[i] = align;
	}

	if (link->buffers)
		free(link->buffers);
	link->buffers = spa_buffer_alloc_array(buffers, flags, 0, NULL, blocks, datas, aligns);
	if (link->buffers == NULL)
		return -ENOMEM;

	link->n_buffers = buffers;

	if (out_alloc) {
		if ((res = spa_node_port_alloc_buffers(link->out_node,
			       SPA_DIRECTION_OUTPUT, link->out_port,
			       NULL, 0,
			       link->buffers, &link->n_buffers)) < 0)
			return res;
	}
	else {
		if ((res = spa_node_port_use_buffers(link->out_node,
			       SPA_DIRECTION_OUTPUT, link->out_port,
			       link->buffers, link->n_buffers)) < 0)
			return res;
	}
	if (in_alloc) {
		if ((res = spa_node_port_alloc_buffers(link->in_node,
			       SPA_DIRECTION_INPUT, link->in_port,
			       NULL, 0,
			       link->buffers, &link->n_buffers)) < 0)
			return res;
	}
	else {
		if ((res = spa_node_port_use_buffers(link->in_node,
			       SPA_DIRECTION_INPUT, link->in_port,
			       link->buffers, link->n_buffers)) < 0)
			return res;
	}
	return 0;
}

static void clean_convert(struct impl *this)
{
	int i;
	for (i = 0; i < this->n_links; i++)
		clean_link(this, &this->links[i]);
	this->n_links = 0;
}

static int setup_buffers(struct impl *this, enum spa_direction direction)
{
	int i, res;

	spa_log_debug(this->log, NAME " %p: %d", this, direction);

	if (direction == SPA_DIRECTION_INPUT) {
		for (i = 0; i < this->n_links; i++) {
			if ((res = negotiate_link_buffers(this, &this->links[i])) < 0)
				spa_log_error(this->log, NAME " %p: buffers %d failed %s",
						this, i, spa_strerror(res));
		}
	} else {
		for (i = this->n_links-1; i >= 0 ; i--) {
			if ((res = negotiate_link_buffers(this, &this->links[i])) < 0)
				spa_log_error(this->log, NAME " %p: buffers %d failed %s",
						this, i, spa_strerror(res));
		}
	}

	return 0;
}

static int impl_node_enum_params(struct spa_node *node,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod *filter,
				 struct spa_pod **param,
				 struct spa_pod_builder *builder)
{
	return -ENOTSUP;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	return -ENOTSUP;
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct impl *this;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if ((res = setup_convert(this)) < 0)
			goto error;
		setup_buffers(this, SPA_DIRECTION_INPUT);
		this->started = true;
		break;
	case SPA_NODE_COMMAND_Pause:
		this->started = false;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;

      error:
	spa_log_error(this->log, "error %s", spa_strerror(res));
	return res;
}

static int
impl_node_set_callbacks(struct spa_node *node,
			const struct spa_node_callbacks *callbacks,
			void *user_data)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	this->callbacks = callbacks;
	this->user_data = user_data;

	return 0;
}

static int
impl_node_get_n_ports(struct spa_node *node,
		      uint32_t *n_input_ports,
		      uint32_t *max_input_ports,
		      uint32_t *n_output_ports,
		      uint32_t *max_output_ports)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_node_get_n_ports(this->fmt[SPA_DIRECTION_INPUT], n_input_ports, max_input_ports, NULL, NULL);
	spa_node_get_n_ports(this->fmt[SPA_DIRECTION_OUTPUT], NULL, NULL, n_output_ports, max_output_ports);

	return 0;
}

static int
impl_node_get_port_ids(struct spa_node *node,
		       uint32_t *input_ids,
		       uint32_t n_input_ids,
		       uint32_t *output_ids,
		       uint32_t n_output_ids)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_node_get_port_ids(this->fmt[SPA_DIRECTION_INPUT], input_ids, n_input_ids, NULL, 0);
	spa_node_get_port_ids(this->fmt[SPA_DIRECTION_OUTPUT], NULL, 0, output_ids, n_output_ids);

	return 0;
}

static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	return spa_node_add_port(this->fmt[direction], direction, port_id);
}

static int
impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	return spa_node_remove_port(this->fmt[direction], direction, port_id);
}

static int
impl_node_port_get_info(struct spa_node *node,
			enum spa_direction direction,
			uint32_t port_id,
			const struct spa_port_info **info)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	return spa_node_port_get_info(this->fmt[direction], direction, port_id, info);
}

static int
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t *index,
			   const struct spa_pod *filter,
			   struct spa_pod **result,
			   struct spa_pod_builder *builder)
{
	struct impl *this;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_PropInfo:
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(builder,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   &SPA_POD_Id(SPA_PROP_volume),
				SPA_PROP_INFO_type, &SPA_POD_CHOICE_RANGE_Float(1.0, 0.0, 10.0),
				0);
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_IO:
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(builder,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id, &SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, &SPA_POD_Int(sizeof(struct spa_io_buffers)),
				0);
			break;
		case 1:
			param = spa_pod_builder_object(builder,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id, &SPA_POD_Id(SPA_IO_Range),
				SPA_PARAM_IO_size, &SPA_POD_Int(sizeof(struct spa_io_range)),
				0);
			break;
		case 2:
			param = spa_pod_builder_object(builder,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id, &SPA_POD_Id(SPA_IO_Control),
				SPA_PARAM_IO_size, &SPA_POD_Int(sizeof(struct spa_io_sequence)),
				0);
			break;
		default:
			return 0;
		}
		break;
	default:
		return spa_node_port_enum_params(this->fmt[direction], direction, port_id,
			id, index, filter, result, builder);
	}
	(*index)++;

	if (spa_pod_filter(builder, result, param, filter) < 0)
		goto next;

	return 1;
}

static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	return spa_node_port_set_param(this->fmt[direction], direction, port_id, id, flags, param);
}

static int
impl_node_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	return spa_node_port_use_buffers(this->fmt[direction], direction, port_id, buffers, n_buffers);
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
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	return spa_node_port_alloc_buffers(this->fmt[direction], direction, port_id,
			params, n_params, buffers, n_buffers);
}

static int
impl_node_port_set_io(struct spa_node *node,
		      enum spa_direction direction, uint32_t port_id,
		      uint32_t id, void *data, size_t size)
{
	struct impl *this;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_log_debug(this->log, "set io %d %d %d", id, direction, port_id);

	switch (id) {
	case SPA_IO_Range:
		res = spa_node_port_set_io(this->resample, direction, 0, id, data, size);
		break;
	case SPA_IO_Control:
		res = spa_node_port_set_io(this->channelmix, direction, 0, id, data, size);
		break;
	default:
		res = spa_node_port_set_io(this->fmt[direction], direction, port_id, id, data, size);
		break;
	}
	return res;
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	return spa_node_port_reuse_buffer(this->fmt[SPA_DIRECTION_OUTPUT], port_id, buffer_id);
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    const struct spa_command *command)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	return spa_node_port_send_command(this->fmt[direction], direction, port_id, command);
}

static int impl_node_process(struct spa_node *node)
{
	struct impl *this;
	int r, i, res = SPA_STATUS_OK;
	int ready;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_log_trace(this->log, NAME " %p: process %d", this, this->n_links);

	while (1) {
		res = SPA_STATUS_OK;
		ready = 0;
		for (i = 0; i < this->n_nodes; i++) {
			r = spa_node_process(this->nodes[i]);
			spa_log_trace(this->log, NAME " %p: process %d %d", this, i, r);

			if (r < 0)
				return r;

			if (r & SPA_STATUS_HAVE_BUFFER)
				ready++;

			if (i == 0)
				res |= r & SPA_STATUS_NEED_BUFFER;
			if (i == this->n_nodes-1)
				res |= r & SPA_STATUS_HAVE_BUFFER;
		}
		if (res & SPA_STATUS_HAVE_BUFFER)
			break;
		if (ready == 0)
			break;
	}

	spa_log_trace(this->log, NAME " %p: process result: %d", this, res);

	return res;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	NULL,
	impl_node_enum_params,
	impl_node_set_param,
	impl_node_send_command,
	impl_node_set_callbacks,
	impl_node_get_n_ports,
	impl_node_get_port_ids,
	impl_node_add_port,
	impl_node_remove_port,
	impl_node_port_get_info,
	impl_node_port_enum_params,
	impl_node_port_set_param,
	impl_node_port_use_buffers,
	impl_node_port_alloc_buffers,
	impl_node_port_set_io,
	impl_node_port_reuse_buffer,
	impl_node_port_send_command,
	impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (type == SPA_TYPE_INTERFACE_Node)
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	this = (struct impl *) handle;

	clean_convert(this);
	return 0;
}

extern const struct spa_handle_factory spa_fmtconvert_factory;
extern const struct spa_handle_factory spa_channelmix_factory;
extern const struct spa_handle_factory spa_resample_factory;
extern const struct spa_handle_factory spa_splitter_factory;
extern const struct spa_handle_factory spa_merger_factory;

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	size_t size, max;

	max = spa_handle_factory_get_size(&spa_fmtconvert_factory, params);
	max = SPA_MAX(max, spa_handle_factory_get_size(&spa_splitter_factory, params));
	max = SPA_MAX(max, spa_handle_factory_get_size(&spa_merger_factory, params));

	size = sizeof(struct impl);
	size += max * 2;
	size += spa_handle_factory_get_size(&spa_channelmix_factory, params);
	size += spa_handle_factory_get_size(&spa_resample_factory, params);

	return size;
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	uint32_t i;
	size_t size;
	void *iface;
	const char *str;
	const struct spa_handle_factory *in_factory, *out_factory;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		if (support[i].type == SPA_TYPE_INTERFACE_Log)
			this->log = support[i].data;
	}
	this->node = impl_node;

	if (info == NULL || (str = spa_dict_lookup(info, "factory.mode")) == NULL)
		str = "convert";

	if (strcmp(str, "split") == 0) {
		in_factory = &spa_fmtconvert_factory;
		out_factory = &spa_splitter_factory;
	}
	else if (strcmp(str, "merge") == 0) {
		in_factory = &spa_merger_factory;
		out_factory = &spa_fmtconvert_factory;
	}
	else {
		in_factory = &spa_fmtconvert_factory;
		out_factory = &spa_fmtconvert_factory;
	}

	this->hnd_fmt[SPA_DIRECTION_INPUT] = SPA_MEMBER(this, sizeof(struct impl), struct spa_handle);
	spa_handle_factory_init(in_factory,
				this->hnd_fmt[SPA_DIRECTION_INPUT],
				info, support, n_support);
	size = spa_handle_factory_get_size(in_factory, info);

	this->hnd_channelmix = SPA_MEMBER(this->hnd_fmt[SPA_DIRECTION_INPUT], size, struct spa_handle);
	spa_handle_factory_init(&spa_channelmix_factory,
				this->hnd_channelmix,
				info, support, n_support);
	size = spa_handle_factory_get_size(&spa_channelmix_factory, info);

	this->hnd_resample = SPA_MEMBER(this->hnd_channelmix, size, struct spa_handle);
	spa_handle_factory_init(&spa_resample_factory,
				this->hnd_resample,
				info, support, n_support);
	size = spa_handle_factory_get_size(&spa_resample_factory, info);

	this->hnd_fmt[SPA_DIRECTION_OUTPUT] = SPA_MEMBER(this->hnd_resample, size, struct spa_handle);
	spa_handle_factory_init(out_factory,
				this->hnd_fmt[SPA_DIRECTION_OUTPUT],
				info, support, n_support);
	size = spa_handle_factory_get_size(out_factory, info);

	spa_handle_get_interface(this->hnd_fmt[SPA_DIRECTION_INPUT], SPA_TYPE_INTERFACE_Node, &iface);
	this->fmt[SPA_DIRECTION_INPUT] = iface;
	spa_handle_get_interface(this->hnd_channelmix, SPA_TYPE_INTERFACE_Node, &iface);
	this->channelmix = iface;
	spa_handle_get_interface(this->hnd_resample, SPA_TYPE_INTERFACE_Node, &iface);
	this->resample = iface;
	spa_handle_get_interface(this->hnd_fmt[SPA_DIRECTION_OUTPUT], SPA_TYPE_INTERFACE_Node, &iface);
	this->fmt[SPA_DIRECTION_OUTPUT] = iface;

	props_reset(&this->props);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{ SPA_TYPE_INTERFACE_Node, },
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

const struct spa_handle_factory spa_audioconvert_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
