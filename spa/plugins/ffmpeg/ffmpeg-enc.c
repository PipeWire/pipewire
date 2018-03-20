/* Spa FFMpeg Encoder
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <spa/support/log.h>
#include <spa/support/type-map.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/video/format-utils.h>

#include <lib/pod.h>

#define IS_VALID_PORT(this,d,id)	((id) == 0)
#define GET_IN_PORT(this,p)		(&this->in_ports[p])
#define GET_OUT_PORT(this,p)		(&this->out_ports[p])
#define GET_PORT(this,d,p)		(d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,p) : GET_OUT_PORT(this,p))

#define MAX_BUFFERS    32

struct buffer {
	struct spa_buffer buffer;
	struct spa_meta metas[1];
	struct spa_meta_header header;
	struct spa_data datas[1];
	struct spa_buffer *imported;
	bool outstanding;
	struct buffer *next;
};

struct port {
	bool have_format;
	struct spa_video_info current_format;
	bool have_buffers;
	struct buffer buffers[MAX_BUFFERS];
	struct spa_port_info info;
	struct spa_io_buffers *io;
};

struct type {
	uint32_t node;
	struct spa_type_io io;
	struct spa_type_param param;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_video format_video;
	struct spa_type_command_node command_node;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->node = spa_type_map_get_id(map, SPA_TYPE__Node);
	spa_type_io_map(map, &type->io);
	spa_type_param_map(map, &type->param);
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_video_map(map, &type->format_video);
	spa_type_command_node_map(map, &type->command_node);
}

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct type type;
	struct spa_type_map *map;
	struct spa_log *log;

	const struct spa_node_callbacks *callbacks;
	void *user_data;

	struct port in_ports[1];
	struct port out_ports[1];

	bool started;
};

static int spa_ffmpeg_enc_node_enum_params(struct spa_node *node,
					   uint32_t id, uint32_t *index,
					   const struct spa_pod *filter,
					   struct spa_pod **param,
					   struct spa_pod_builder *builder)
{
	return -ENOTSUP;
}

static int spa_ffmpeg_enc_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
					 const struct spa_pod *param)
{
	return -ENOTSUP;
}

static int spa_ffmpeg_enc_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct impl *this;

	if (node == NULL || command == NULL)
		return -EINVAL;

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
spa_ffmpeg_enc_node_set_callbacks(struct spa_node *node,
				  const struct spa_node_callbacks *callbacks,
				  void *user_data)
{
	struct impl *this;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct impl, node);

	this->callbacks = callbacks;
	this->user_data = user_data;

	return 0;
}

static int
spa_ffmpeg_enc_node_get_n_ports(struct spa_node *node,
				uint32_t *n_input_ports,
				uint32_t *max_input_ports,
				uint32_t *n_output_ports,
				uint32_t *max_output_ports)
{
	if (node == NULL)
		return -EINVAL;

	if (n_input_ports)
		*n_input_ports = 1;
	if (n_output_ports)
		*n_output_ports = 1;
	if (max_input_ports)
		*max_input_ports = 1;
	if (max_output_ports)
		*max_output_ports = 1;

	return 0;
}

static int
spa_ffmpeg_enc_node_get_port_ids(struct spa_node *node,
				 uint32_t *input_ids,
				 uint32_t n_input_ids,
				 uint32_t *output_ids,
				 uint32_t n_output_ids)
{
	if (node == NULL)
		return -EINVAL;

	if (n_input_ids > 0 && input_ids != NULL)
		input_ids[0] = 0;
	if (n_output_ids > 0 && output_ids != NULL)
		output_ids[0] = 0;

	return 0;
}


static int
spa_ffmpeg_enc_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
spa_ffmpeg_enc_node_remove_port(struct spa_node *node,
				enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
spa_ffmpeg_enc_node_port_get_info(struct spa_node *node,
				  enum spa_direction direction,
				  uint32_t port_id, const struct spa_port_info **info)
{
	struct impl *this;
	struct port *port;

	if (node == NULL || info == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (!IS_VALID_PORT(this, direction, port_id))
		return -EINVAL;

	port = GET_PORT(this, direction, port_id);
	*info = &port->info;

	return 0;
}

static int port_enum_formats(struct spa_node *node,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t *index,
			     const struct spa_pod *filter,
			     struct spa_pod **param,
			     struct spa_pod_builder *builder)
{
	//struct impl *this = SPA_CONTAINER_OF (node, struct impl, node);
	//struct port *port;

	//port = GET_PORT(this, direction, port_id);

	switch (*index) {
	case 0:
		*param = NULL;
		break;
	default:
		return 0;
	}
	return 1;
}

static int port_get_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t *index,
			   const struct spa_pod *filter,
			   struct spa_pod **param,
			   struct spa_pod_builder *builder)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *port;

	port = GET_PORT(this, direction, port_id);

	if (!port->have_format)
		return -EIO;

	if (*index > 0)
		return 0;

	*param = NULL;

	return 1;
}

static int
spa_ffmpeg_enc_node_port_enum_params(struct spa_node *node,
				     enum spa_direction direction, uint32_t port_id,
				     uint32_t id, uint32_t *index,
				     const struct spa_pod *filter,
				     struct spa_pod **result,
				     struct spa_pod_builder *builder)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct type *t = &this->type;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	int res;

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (id == t->param.idList) {
		uint32_t list[] = { t->param.idEnumFormat,
				    t->param.idFormat };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b, id, t->param.List,
				":", t->param.listId, "I", list[*index]);
		else
			return 0;
	}
	else if (id == t->param.idEnumFormat) {
		if ((res = port_enum_formats(node, direction, port_id, index, filter, &param, &b)) <= 0)
			return res;
	}
	else if (id == t->param.idFormat) {
		if ((res = port_get_format(node, direction, port_id, index, filter, &param, &b)) <= 0)
			return res;
	}
	else
		return -ENOENT;

	(*index)++;

	if (spa_pod_filter(builder, result, param, filter) < 0)
		goto next;

	return 1;
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags, const struct spa_pod *format)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *port;

	port = GET_PORT(this, direction, port_id);

	if (format == NULL) {
		port->have_format = false;
		return 0;
	} else {
		struct spa_video_info info = { 0 };

		spa_pod_object_parse(format,
			"I", &info.media_type,
			"I", &info.media_subtype);

		if (info.media_type != this->type.media_type.video &&
		    info.media_subtype != this->type.media_subtype.raw)
			return -EINVAL;

		if (spa_format_video_raw_parse(format, &info.info.raw, &this->type.format_video) < 0)
			return -EINVAL;

		if (!(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)) {
			port->current_format = info;
			port->have_format = true;
		}
	}
	return 0;
}

static int
spa_ffmpeg_enc_node_port_set_param(struct spa_node *node,
				   enum spa_direction direction, uint32_t port_id,
				   uint32_t id, uint32_t flags,
				   const struct spa_pod *param)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct type *t = &this->type;

	if (id == t->param.idFormat) {
		return port_set_format(node, direction, port_id, flags, param);
	}
	else
		return -ENOENT;
}

static int
spa_ffmpeg_enc_node_port_use_buffers(struct spa_node *node,
				     enum spa_direction direction,
				     uint32_t port_id,
				     struct spa_buffer **buffers, uint32_t n_buffers)
{
	if (node == NULL)
		return -EINVAL;

	if (!IS_VALID_PORT(node, direction, port_id))
		return -EINVAL;

	return -ENOTSUP;
}

static int
spa_ffmpeg_enc_node_port_alloc_buffers(struct spa_node *node,
				       enum spa_direction direction,
				       uint32_t port_id,
				       struct spa_pod **params,
				       uint32_t n_params,
				       struct spa_buffer **buffers,
				       uint32_t *n_buffers)
{
	return -ENOTSUP;
}

static int
spa_ffmpeg_enc_node_port_set_io(struct spa_node *node,
				enum spa_direction direction,
				uint32_t port_id,
				uint32_t id,
				void *data, size_t size)
{
	struct impl *this;
	struct port *port;
	struct type *t;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	if (!IS_VALID_PORT(this, direction, port_id))
		return -EINVAL;

	port = GET_PORT(this, direction, port_id);

	if (id == t->io.Buffers)
		port->io = data;
	else
		return -ENOENT;

	return 0;
}

static int
spa_ffmpeg_enc_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	if (node == NULL)
		return -EINVAL;

	if (port_id != 0)
		return -EINVAL;

	return -ENOTSUP;
}

static int
spa_ffmpeg_enc_node_port_send_command(struct spa_node *node,
				      enum spa_direction direction,
				      uint32_t port_id, const struct spa_command *command)
{
	return -ENOTSUP;
}

static int spa_ffmpeg_enc_node_process(struct spa_node *node)
{
	struct impl *this;
	struct port *port;
	struct spa_io_buffers *output;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if ((output = this->out_ports[0].io) == NULL)
		return -EIO;

	port = &this->out_ports[0];

	if (!port->have_format) {
		output->status = -EIO;
		return -EIO;
	}
	output->status = SPA_STATUS_OK;

	return SPA_STATUS_OK;
}

static const struct spa_node ffmpeg_enc_node = {
	SPA_VERSION_NODE,
	NULL,
	spa_ffmpeg_enc_node_enum_params,
	spa_ffmpeg_enc_node_set_param,
	spa_ffmpeg_enc_node_send_command,
	spa_ffmpeg_enc_node_set_callbacks,
	spa_ffmpeg_enc_node_get_n_ports,
	spa_ffmpeg_enc_node_get_port_ids,
	spa_ffmpeg_enc_node_add_port,
	spa_ffmpeg_enc_node_remove_port,
	spa_ffmpeg_enc_node_port_get_info,
	spa_ffmpeg_enc_node_port_enum_params,
	spa_ffmpeg_enc_node_port_set_param,
	spa_ffmpeg_enc_node_port_use_buffers,
	spa_ffmpeg_enc_node_port_alloc_buffers,
	spa_ffmpeg_enc_node_port_set_io,
	spa_ffmpeg_enc_node_port_reuse_buffer,
	spa_ffmpeg_enc_node_port_send_command,
	spa_ffmpeg_enc_node_process,
};

static int
spa_ffmpeg_enc_get_interface(struct spa_handle *handle, uint32_t interface_id, void **interface)
{
	struct impl *this;

	if (handle == NULL || interface == NULL)
		return -EINVAL;

	this = (struct impl *) handle;

	if (interface_id == this->type.node)
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

int
spa_ffmpeg_enc_init(struct spa_handle *handle,
		    const struct spa_dict *info,
		    const struct spa_support *support, uint32_t n_support)
{
	struct impl *this;
	uint32_t i;

	handle->get_interface = spa_ffmpeg_enc_get_interface;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		if (strcmp(support[i].type, SPA_TYPE__TypeMap) == 0)
			this->map = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE__Log) == 0)
			this->log = support[i].data;
	}
	if (this->map == NULL) {
		spa_log_error(this->log, "a type-map is needed");
		return -EINVAL;
	}

	this->node = ffmpeg_enc_node;

	this->in_ports[0].info.flags = 0;
	this->out_ports[0].info.flags = 0;

	return 0;
}
