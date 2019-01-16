/* Spa FFMpeg Decoder
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
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <spa/support/log.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/format.h>
#include <spa/pod/filter.h>

#define IS_VALID_PORT(this,d,id)	((id) == 0)
#define GET_IN_PORT(this,p)		(&this->in_ports[p])
#define GET_OUT_PORT(this,p)		(&this->out_ports[p])
#define GET_PORT(this,d,p)		(d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,p) : GET_OUT_PORT(this,p))

#define MAX_BUFFERS    32

struct buffer {
	struct spa_buffer *outbuf;
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

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;

	const struct spa_node_callbacks *callbacks;
	void *user_data;

	struct port in_ports[1];
	struct port out_ports[1];

	bool started;
};

static int spa_ffmpeg_dec_node_enum_params(struct spa_node *node,
					   uint32_t id, uint32_t *index,
					   const struct spa_pod *filter,
					   struct spa_pod **result,
					   struct spa_pod_builder *builder)
{
	return -ENOTSUP;
}

static int spa_ffmpeg_dec_node_set_param(struct spa_node *node,
					 uint32_t id, uint32_t flags,
					 const struct spa_pod *param)
{
	return -ENOTSUP;
}

static int spa_ffmpeg_dec_node_set_io(struct spa_node *node, uint32_t id, void *data, size_t size)
{
	return -ENOTSUP;
}

static int spa_ffmpeg_dec_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct impl *this;

	if (node == NULL || command == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct impl, node);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		this->started = true;
		break;
	case SPA_NODE_COMMAND_Pause:
		this->started = false;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static int
spa_ffmpeg_dec_node_set_callbacks(struct spa_node *node,
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
spa_ffmpeg_dec_node_get_n_ports(struct spa_node *node,
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
spa_ffmpeg_dec_node_get_port_ids(struct spa_node *node,
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
spa_ffmpeg_dec_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
spa_ffmpeg_dec_node_remove_port(struct spa_node *node,
				enum spa_direction direction,
				uint32_t port_id)
{
	return -ENOTSUP;
}

static int
spa_ffmpeg_dec_node_port_get_info(struct spa_node *node,
				  enum spa_direction direction,
				  uint32_t port_id,
				  const struct spa_port_info **info)
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
	//struct impl *this;

	if (node == NULL || index == NULL)
		return -EINVAL;

	//this = SPA_CONTAINER_OF (node, struct impl, node);

	if (!IS_VALID_PORT(this, direction, port_id))
		return -EINVAL;

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
spa_ffmpeg_dec_node_port_enum_params(struct spa_node *node,
				     enum spa_direction direction, uint32_t port_id,
				     uint32_t id, uint32_t *index,
				     const struct spa_pod *filter,
				     struct spa_pod **result,
				     struct spa_pod_builder *builder)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	int res;

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_EnumFormat,
				    SPA_PARAM_Format };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_add_object(&b,
					SPA_TYPE_OBJECT_ParamList, id,
					SPA_PARAM_LIST_id, SPA_POD_Id(list[*index]));
		else
			return 0;
		break;
	}
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(node, direction, port_id, index, filter, &param, &b)) <= 0)
			return res;
		break;

	case SPA_PARAM_Format:
		if ((res = port_get_format(node, direction, port_id, index, filter, &param, &b)) <= 0)
			return res;
		break;

	default:
		return -ENOENT;
	}

	(*index)++;

	if (spa_pod_filter(builder, result, param, filter) < 0)
		goto next;

	return 1;
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	struct impl *this;
	struct port *port;
	int res;

	if (node == NULL || format == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (!IS_VALID_PORT(this, direction, port_id))
		return -EINVAL;

	port = GET_PORT(this, direction, port_id);

	if (format == NULL) {
		port->have_format = false;
		return 0;
	} else {
		struct spa_video_info info = { 0 };

		if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return res;

		if (info.media_type != SPA_MEDIA_TYPE_video &&
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;

		if (spa_format_video_raw_parse(format, &info.info.raw) < 0)
			return -EINVAL;

		if (!(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)) {
			port->current_format = info;
			port->have_format = true;
		}
	}
	return 0;
}

static int
spa_ffmpeg_dec_node_port_set_param(struct spa_node *node,
				   enum spa_direction direction, uint32_t port_id,
				   uint32_t id, uint32_t flags,
				   const struct spa_pod *param)
{
	if (id == SPA_PARAM_Format) {
		return port_set_format(node, direction, port_id, flags, param);
	}
	else
		return -ENOENT;
}

static int
spa_ffmpeg_dec_node_port_use_buffers(struct spa_node *node,
				     enum spa_direction direction,
				     uint32_t port_id,
				     struct spa_buffer **buffers,
				     uint32_t n_buffers)
{
	if (node == NULL)
		return -EINVAL;

	if (!IS_VALID_PORT(node, direction, port_id))
		return -EINVAL;

	return -ENOTSUP;
}

static int
spa_ffmpeg_dec_node_port_alloc_buffers(struct spa_node *node,
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
spa_ffmpeg_dec_node_port_set_io(struct spa_node *node,
				enum spa_direction direction,
				uint32_t port_id,
				uint32_t id,
				void *data, size_t size)
{
	struct impl *this;
	struct port *port;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (!IS_VALID_PORT(this, direction, port_id))
		return -EINVAL;

	port = GET_PORT(this, direction, port_id);

	if (id == SPA_IO_Buffers)
		port->io = data;
	else
		return -ENOENT;

	return 0;
}

static int spa_ffmpeg_dec_node_process(struct spa_node *node)
{
	struct impl *this;
	struct port *port;
	struct spa_io_buffers *output;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct impl, node);

	port = &this->out_ports[0];

	if ((output = port->io) == NULL)
		return -EIO;

	if (!port->have_format) {
		output->status = -EIO;
		return -EIO;
	}
	output->status = SPA_STATUS_OK;

	return SPA_STATUS_OK;
}

static int
spa_ffmpeg_dec_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	if (node == NULL)
		return -EINVAL;

	if (port_id != 0)
		return -EINVAL;

	return -ENOTSUP;
}

static int
spa_ffmpeg_dec_node_port_send_command(struct spa_node *node,
				      enum spa_direction direction,
				      uint32_t port_id,
				      const struct spa_command *command)
{
	return -ENOTSUP;
}


static const struct spa_node ffmpeg_dec_node = {
	SPA_VERSION_NODE,
	spa_ffmpeg_dec_node_enum_params,
	spa_ffmpeg_dec_node_set_param,
	spa_ffmpeg_dec_node_set_io,
	spa_ffmpeg_dec_node_send_command,
	spa_ffmpeg_dec_node_set_callbacks,
	spa_ffmpeg_dec_node_get_n_ports,
	spa_ffmpeg_dec_node_get_port_ids,
	spa_ffmpeg_dec_node_add_port,
	spa_ffmpeg_dec_node_remove_port,
	spa_ffmpeg_dec_node_port_get_info,
	spa_ffmpeg_dec_node_port_enum_params,
	spa_ffmpeg_dec_node_port_set_param,
	spa_ffmpeg_dec_node_port_use_buffers,
	spa_ffmpeg_dec_node_port_alloc_buffers,
	spa_ffmpeg_dec_node_port_set_io,
	spa_ffmpeg_dec_node_port_reuse_buffer,
	spa_ffmpeg_dec_node_port_send_command,
	spa_ffmpeg_dec_node_process,
};

static int
spa_ffmpeg_dec_get_interface(struct spa_handle *handle, uint32_t type, void **interface)
{
	struct impl *this;

	if (handle == NULL || interface == NULL)
		return -EINVAL;

	this = (struct impl *) handle;

	if (type == SPA_TYPE_INTERFACE_Node)
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

int
spa_ffmpeg_dec_init(struct spa_handle *handle,
		    const struct spa_dict *info,
		    const struct spa_support *support,
		    uint32_t n_support)
{
	struct impl *this;
	uint32_t i;

	handle->get_interface = spa_ffmpeg_dec_get_interface;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		if (support[i].type == SPA_TYPE_INTERFACE_Log)
			this->log = support[i].data;
	}

	this->node = ffmpeg_dec_node;

	this->in_ports[0].info.flags = 0;
	this->out_ports[0].info.flags = 0;

	return 0;
}
