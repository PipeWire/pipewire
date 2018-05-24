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
#include <spa/support/type-map.h>
#include <spa/utils/list.h>
#include <spa/node/node.h>
#include <spa/buffer/alloc.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/param/meta.h>
#include <spa/param/io.h>

#include <lib/pod.h>
#include <lib/debug.h>

#define NAME "audioconvert"

#define MAX_BUFFERS     32

#define PROP_DEFAULT_TRUNCATE	false
#define PROP_DEFAULT_DITHER	0

struct type {
	uint32_t node;
	uint32_t format;
	uint32_t prop_truncate;
	uint32_t prop_dither;
	struct spa_type_io io;
	struct spa_type_param param;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_audio format_audio;
	struct spa_type_audio_format audio_format;
	struct spa_type_command_node command_node;
	struct spa_type_meta meta;
	struct spa_type_data data;
	struct spa_type_param_buffers param_buffers;
	struct spa_type_param_meta param_meta;
	struct spa_type_param_io param_io;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->node = spa_type_map_get_id(map, SPA_TYPE__Node);
	type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
	type->prop_truncate = spa_type_map_get_id(map, SPA_TYPE_PROPS__truncate);
	type->prop_dither = spa_type_map_get_id(map, SPA_TYPE_PROPS__ditherType);
	spa_type_io_map(map, &type->io);
	spa_type_param_map(map, &type->param);
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_audio_map(map, &type->format_audio);
	spa_type_audio_format_map(map, &type->audio_format);
	spa_type_command_node_map(map, &type->command_node);
	spa_type_meta_map(map, &type->meta);
	spa_type_data_map(map, &type->data);
	spa_type_param_buffers_map(map, &type->param_buffers);
	spa_type_param_meta_map(map, &type->param_meta);
	spa_type_param_io_map(map, &type->param_io);
}
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

struct port {
	uint32_t id;

	struct spa_io_buffers *io;

	bool have_format;
	struct spa_audio_info format;

	struct spa_node *node;
};

struct link {
	struct spa_node *out_node;
	uint32_t out_port;
	const struct spa_port_info *out_info;
	struct spa_node *in_node;
	uint32_t in_port;
	const struct spa_port_info *in_info;
	struct spa_io_buffers io;
	struct spa_audio_info *info;
	bool negotiated;
	uint32_t n_buffers;
	struct spa_buffer **buffers;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct type type;
	struct spa_type_map *map;
	struct spa_log *log;

	struct props props;

	const struct spa_node_callbacks *callbacks;
	void *user_data;

	struct port in_port;
	struct port out_port;

	int n_links;
	struct link links[8];

	bool started;

	struct spa_handle *hnd_fmt_in;
	struct spa_handle *hnd_channelmix;
	struct spa_handle *hnd_resample;
	struct spa_handle *hnd_fmt_out;

	struct spa_node *fmt_in;
	struct spa_node *channelmix;
	struct spa_node *resample;
	struct spa_node *fmt_out;
};

#define CHECK_PORT(this,d,id)		(id == 0)
#define GET_IN_PORT(this,id)		(&this->in_port)
#define GET_OUT_PORT(this,id)		(&this->out_port)
#define GET_PORT(this,d,id)		(d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,id) : GET_OUT_PORT(this,id))

static int make_link(struct impl *this,
		struct spa_node *out_node, uint32_t out_port,
		struct spa_node *in_node, uint32_t in_port,
		struct spa_audio_info *info)
{
	struct link *l = &this->links[this->n_links++];
	struct type *t = &this->type;

	l->out_node = out_node;
	l->out_port = out_port;
	l->in_node = in_node;
	l->in_port = in_port;
	l->negotiated = false;
	l->io.status = SPA_STATUS_NEED_BUFFER;
	l->io.buffer_id = SPA_ID_INVALID;
	l->n_buffers = 0;
	l->info = info;

	if (out_node != NULL) {
		spa_node_port_get_info(out_node,
				       SPA_DIRECTION_OUTPUT, out_port,
				       &l->out_info);
		spa_node_port_set_io(out_node,
				     SPA_DIRECTION_OUTPUT, out_port,
				     t->io.Buffers,
				     &l->io, sizeof(l->io));
	}
	if (in_node != NULL) {
		spa_node_port_get_info(in_node,
				       SPA_DIRECTION_INPUT, in_port,
				       &l->in_info);
		spa_node_port_set_io(in_node,
				     SPA_DIRECTION_INPUT, in_port,
				     t->io.Buffers,
				     &l->io, sizeof(l->io));
	}
	return 0;
}

static void clean_link(struct impl *this, struct link *link)
{
	struct type *t = &this->type;

	if (link->in_node) {
		spa_node_port_set_param(link->in_node,
					SPA_DIRECTION_INPUT, link->in_port,
					t->param.idFormat, 0, NULL);
	}
	if (link->out_node) {
		spa_node_port_set_param(link->out_node,
					SPA_DIRECTION_OUTPUT, link->out_port,
					t->param.idFormat, 0, NULL);
	}
	if (link->buffers)
		free(link->buffers);
	link->buffers = NULL;
}

static int negotiate_link_format(struct impl *this, struct link *link)
{
	struct type *t = &this->type;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];
	uint32_t state;
	struct spa_pod *format, *filter;
	int res;

	if (link->negotiated)
		return 0;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	if (link->info) {
		filter = spa_pod_builder_object(&b,
			0, t->format,
			"I", link->info->media_type,
			"I", link->info->media_subtype,
			":", t->format_audio.format,   "I", link->info->info.raw.format,
			":", t->format_audio.layout,   "i", link->info->info.raw.layout,
			":", t->format_audio.rate,     "i", link->info->info.raw.rate,
			":", t->format_audio.channels, "i", link->info->info.raw.channels);
	}
	else
		filter = NULL;

	if (link->out_node != NULL) {
		state = 0;
		if ((res = spa_node_port_enum_params(link->out_node,
				       SPA_DIRECTION_OUTPUT, link->out_port,
				       t->param.idEnumFormat, &state,
				       filter, &format, &b)) <= 0)
			return -ENOTSUP;

		filter = format;
	}
	if (link->in_node != NULL) {
		state = 0;
		if ((res = spa_node_port_enum_params(link->in_node,
				       SPA_DIRECTION_INPUT, link->in_port,
				       t->param.idEnumFormat, &state,
				       filter, &format, &b)) <= 0)
			return -ENOTSUP;

		filter = format;
	}

	spa_pod_fixate(filter);

	if (link->out_node != NULL) {
		if ((res = spa_node_port_set_param(link->out_node,
					   SPA_DIRECTION_OUTPUT, link->out_port,
					   t->param.idFormat, 0,
					   filter)) < 0)
			return res;
	}
	if (link->in_node != NULL) {
		if ((res = spa_node_port_set_param(link->in_node,
					   SPA_DIRECTION_INPUT, link->in_port,
					   t->param.idFormat, 0,
					   filter)) < 0)
			return res;
	}
	link->negotiated = true;

	return 0;
}

static int setup_convert(struct impl *this)
{
	struct port *inport, *outport;
	struct spa_node *prev = NULL;
	int i, j, res;
	struct type *t = &this->type;

	inport = GET_PORT(this, SPA_DIRECTION_INPUT, 0);
	outport = GET_PORT(this, SPA_DIRECTION_OUTPUT, 0);

	spa_log_info(this->log, NAME " %p: %d/%d@%d.%d->%d/%d@%d.%d", this,
			inport->format.info.raw.format,
			inport->format.info.raw.channels,
			inport->format.info.raw.rate,
			inport->format.info.raw.layout,
			outport->format.info.raw.format,
			outport->format.info.raw.channels,
			outport->format.info.raw.rate,
			outport->format.info.raw.layout);

	if (this->n_links > 0)
		return 0;

	/* unpack */
	make_link(this, NULL, 0, this->fmt_in, 0, &inport->format);
	prev = this->fmt_in;

	/* down mix */
	if (inport->format.info.raw.channels > outport->format.info.raw.channels) {
		make_link(this, prev, 0, this->channelmix, 0, NULL);
		prev = this->channelmix;
	}

	/* resample */
	if (inport->format.info.raw.rate != outport->format.info.raw.rate) {
		make_link(this, prev, 0, this->resample, 0, NULL);
		prev = this->resample;
	}

	/* up mix */
	if (inport->format.info.raw.channels < outport->format.info.raw.channels) {
		make_link(this, prev, 0, this->channelmix, 0, NULL);
		prev = this->channelmix;
	}

	make_link(this, prev, 0, this->fmt_out, 0, NULL);

	/* pack */
	make_link(this, this->fmt_out, 0, NULL, 0, &outport->format);

	for (i = 0, j = this->n_links - 1; j >= i; i++, j--) {
		if ((res = negotiate_link_format(this, &this->links[i])) < 0)
			return res;
		if ((res = negotiate_link_format(this, &this->links[j])) < 0)
			return res;
	}


	spa_node_port_set_io(inport->node, SPA_DIRECTION_INPUT, 0,
			t->io.Buffers, inport->io, sizeof(struct spa_io_buffers));
	spa_node_port_set_io(outport->node, SPA_DIRECTION_OUTPUT, 0,
			t->io.Buffers, outport->io, sizeof(struct spa_io_buffers));

	return 0;
}

static int negotiate_link_buffers(struct impl *this, struct link *link)
{
	struct type *t = &this->type;
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

	if (link->out_node != NULL) {
		state = 0;
		if ((res = spa_node_port_enum_params(link->out_node,
				       SPA_DIRECTION_OUTPUT, link->out_port,
				       t->param.idBuffers, &state,
				       param, &param, &b)) <= 0)
			return -ENOTSUP;
	}
	if (link->in_node != NULL) {
		state = 0;
		if ((res = spa_node_port_enum_params(link->in_node,
				       SPA_DIRECTION_INPUT, link->in_port,
				       t->param.idBuffers, &state,
				       param, &param, &b)) <= 0)
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
		":", t->param_buffers.buffers, "i", &buffers,
		":", t->param_buffers.blocks, "i", &blocks,
		":", t->param_buffers.size, "i", &size,
		":", t->param_buffers.align, "i", &align,
		NULL) < 0)
		return -EINVAL;

	datas = alloca(sizeof(struct spa_data) * blocks);
	memset(datas, 0, sizeof(struct spa_data) * blocks);
	aligns = alloca(sizeof(uint32_t) * blocks);
	for (i = 0; i < blocks; i++) {
		datas[i].type = t->data.MemPtr;
		datas[i].maxsize = size;
		aligns[i] = align;
	}

	if (link->buffers)
		free(link->buffers);
	link->buffers = spa_buffer_alloc_array(buffers, flags, 0, NULL, blocks, datas, aligns);
	if (link->buffers == NULL)
		return -ENOMEM;

	link->n_buffers = buffers;

	if (link->out_node != NULL) {
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
	}
	if (link->in_node != NULL) {
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
		for (i = 1; i < this->n_links-1; i++) {
			if ((res = negotiate_link_buffers(this, &this->links[i])) < 0)
				spa_log_error(this->log, NAME " %p: buffers %d failed %s",
						this, i, spa_strerror(res));
		}
	} else {
		for (i = this->n_links-2; i > 0 ; i--) {
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

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (SPA_COMMAND_TYPE(command) == this->type.command_node.Start) {
		this->started = true;
	} else if (SPA_COMMAND_TYPE(command) == this->type.command_node.Pause) {
		this->started = false;
	} else
		return -ENOTSUP;

	return 0;
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
	spa_return_val_if_fail(node != NULL, -EINVAL);

	if (n_input_ports)
		*n_input_ports = 1;
	if (max_input_ports)
		*max_input_ports = 1;
	if (n_output_ports)
		*n_output_ports = 1;
	if (max_output_ports)
		*max_output_ports = 1;

	return 0;
}

static int
impl_node_get_port_ids(struct spa_node *node,
		       uint32_t *input_ids,
		       uint32_t n_input_ids,
		       uint32_t *output_ids,
		       uint32_t n_output_ids)
{
	spa_return_val_if_fail(node != NULL, -EINVAL);

	if (n_input_ids && input_ids)
		input_ids[0] = 0;
	if (n_output_ids > 0 && output_ids)
		output_ids[0] = 0;

	return 0;
}

static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_port_get_info(struct spa_node *node,
			enum spa_direction direction,
			uint32_t port_id,
			const struct spa_port_info **info)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	return spa_node_port_get_info(port->node, direction, port_id, info);
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
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	return spa_node_port_enum_params(port->node, direction, port_id, id, index,
			filter, result, builder);
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *port, *other;
	struct type *t = &this->type;
	int res = 0;

	port = GET_PORT(this, direction, port_id);
	other = GET_PORT(this, SPA_DIRECTION_REVERSE(direction), port_id);

	if (format == NULL) {
		clean_convert(this);
		port->have_format = false;
	} else {
		struct spa_audio_info info = { 0 };

		spa_pod_object_parse(format,
			"I", &info.media_type,
			"I", &info.media_subtype);

		if (info.media_type != t->media_type.audio ||
		    info.media_subtype != t->media_subtype.raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw, &t->format_audio) < 0)
			return -EINVAL;

		clean_convert(this);
		port->have_format = true;
		port->format = info;

		if (other->have_format)
			res = setup_convert(this);

		spa_log_debug(this->log, NAME " %p: set format on port %d %d", this, port_id, res);
	}
	return res;
}

static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this;
	struct port *port;
	struct type *t;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	if (id == t->param.idFormat)
		return port_set_format(node, direction, port_id, flags, param);
	else
		return spa_node_port_set_param(port->node, direction, port_id, id, flags, param);
}

static int
impl_node_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this;
	struct port *port;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	res = spa_node_port_use_buffers(port->node, direction, port_id, buffers, n_buffers);
	if (res < 0)
		return res;

	return setup_buffers(this, direction);
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
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	return spa_node_port_alloc_buffers(port->node, direction, port_id,
			params, n_params, buffers, n_buffers);
}

static int
impl_node_port_set_io(struct spa_node *node,
		      enum spa_direction direction, uint32_t port_id,
		      uint32_t id, void *data, size_t size)
{
	struct impl *this;
	struct port *port;
	struct type *t;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	if (id == t->io.Buffers)
		port->io = data;

	return spa_node_port_set_io(port->node, direction, port_id, id, data, size);
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	port = GET_PORT(this, SPA_DIRECTION_OUTPUT, port_id);

	return spa_node_port_reuse_buffer(port->node, port_id, buffer_id);
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    const struct spa_command *command)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	return spa_node_port_send_command(port->node, direction, port_id, command);
}

static int impl_node_process(struct spa_node *node)
{
	struct impl *this;
	int i, res = SPA_STATUS_OK;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_log_trace(this->log, NAME " %p: process %d", this, this->n_links);

	for (i = 1; i < this->n_links; i++) {
		int r = spa_node_process(this->links[i].out_node);
		if (i == 1)
			res |= r & SPA_STATUS_NEED_BUFFER;
		if (i == this->n_links - 1)
			res |= r & SPA_STATUS_HAVE_BUFFER;

		if (!SPA_FLAG_CHECK(r, SPA_STATUS_HAVE_BUFFER)) {
			if (SPA_FLAG_CHECK(r, SPA_STATUS_NEED_BUFFER) && i == 1)
				break;
			i = res = SPA_STATUS_OK;
			continue;
		}
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

static int impl_get_interface(struct spa_handle *handle, uint32_t interface_id, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (interface_id == this->type.node)
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

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	size_t size;

	size = sizeof(struct impl);
	size += spa_handle_factory_get_size(&spa_fmtconvert_factory, params) * 2;
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
	struct port *port;
	uint32_t i;
	size_t size;
	void *iface;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		if (strcmp(support[i].type, SPA_TYPE__TypeMap) == 0)
			this->map = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE__Log) == 0)
			this->log = support[i].data;
	}
	if (this->map == NULL) {
		spa_log_error(this->log, "an id-map is needed");
		return -EINVAL;
	}
	init_type(&this->type, this->map);

	this->node = impl_node;

	this->hnd_fmt_in = SPA_MEMBER(this, sizeof(struct impl), struct spa_handle);
	spa_handle_factory_init(&spa_fmtconvert_factory,
				this->hnd_fmt_in,
				info, support, n_support);
	size = spa_handle_factory_get_size(&spa_fmtconvert_factory, info);

	this->hnd_channelmix = SPA_MEMBER(this->hnd_fmt_in, size, struct spa_handle);
	spa_handle_factory_init(&spa_channelmix_factory,
				this->hnd_channelmix,
				info, support, n_support);
	size = spa_handle_factory_get_size(&spa_channelmix_factory, info);

	this->hnd_fmt_out = SPA_MEMBER(this->hnd_channelmix, size, struct spa_handle);
	spa_handle_factory_init(&spa_fmtconvert_factory,
				this->hnd_fmt_out,
				info, support, n_support);
	size = spa_handle_factory_get_size(&spa_fmtconvert_factory, info);

	this->hnd_resample = SPA_MEMBER(this->hnd_fmt_out, size, struct spa_handle);
	spa_handle_factory_init(&spa_resample_factory,
				this->hnd_resample,
				info, support, n_support);
	size = spa_handle_factory_get_size(&spa_resample_factory, info);

	spa_handle_get_interface(this->hnd_fmt_in, this->type.node, &iface);
	this->fmt_in = iface;
	spa_handle_get_interface(this->hnd_fmt_out, this->type.node, &iface);
	this->fmt_out = iface;
	spa_handle_get_interface(this->hnd_channelmix, this->type.node, &iface);
	this->channelmix = iface;
	spa_handle_get_interface(this->hnd_resample, this->type.node, &iface);
	this->resample = iface;

	port = GET_OUT_PORT(this, 0);
	port->id = 0;
	port->node = this->fmt_out;

	port = GET_IN_PORT(this, 0);
	port->id = 0;
	port->node = this->fmt_in;

	props_reset(&this->props);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE__Node,},
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
