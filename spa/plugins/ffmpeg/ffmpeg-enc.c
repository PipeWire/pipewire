/* Spa FFMpeg Encoder
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
#include <spa/pod/filter.h>

#define IS_VALID_PORT(this,d,id)	((id) == 0)
#define GET_IN_PORT(this,p)		(&this->in_ports[p])
#define GET_OUT_PORT(this,p)		(&this->out_ports[p])
#define GET_PORT(this,d,p)		(d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,p) : GET_OUT_PORT(this,p))

#define MAX_BUFFERS    32

struct buffer {
	uint32_t id;
	uint32_t flags;
	struct spa_buffer *outbuf;
	struct spa_list link;
};

struct port {
	struct spa_port_info info;
	struct spa_param_info params[8];

	struct spa_video_info current_format;
	int have_format:1;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_io_buffers *io;

	struct spa_list free;
	struct spa_list ready;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;

	struct spa_node_info info;
	struct spa_param_info params[2];

	const struct spa_node_callbacks *callbacks;
	void *user_data;

	struct port in_ports[1];
	struct port out_ports[1];

	bool started;
};

static int impl_node_enum_params(struct spa_node *node, int seq,
			uint32_t id, uint32_t start, uint32_t num,
			const struct spa_pod *filter)
{
	return -ENOTSUP;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
					 const struct spa_pod *param)
{
	return -ENOTSUP;
}

static int impl_node_set_io(struct spa_node *node, uint32_t id, void *data, size_t size)
{
	return -ENOTSUP;
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
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

static void emit_node_info(struct impl *this)
{
	if (this->callbacks && this->callbacks->info && this->info.change_mask) {
		this->callbacks->info(this->user_data, &this->info);
		this->info.change_mask = 0;
	}
}

static void emit_port_info(struct impl *this, enum spa_direction direction, uint32_t id)
{
	struct port *port = GET_PORT(this, direction, id);

	if (this->callbacks && this->callbacks->port_info && port->info.change_mask) {
		this->callbacks->port_info(this->user_data, direction, id, &port->info);
		port->info.change_mask = 0;
	}
}

static int
impl_node_set_callbacks(struct spa_node *node,
				  const struct spa_node_callbacks *callbacks,
				  void *user_data)
{
	struct impl *this;

	if (node == NULL)
		return -EINVAL;

	this = SPA_CONTAINER_OF(node, struct impl, node);

	this->callbacks = callbacks;
	this->user_data = user_data;

	emit_node_info(this);
	emit_port_info(this, SPA_DIRECTION_INPUT, 0);
	emit_port_info(this, SPA_DIRECTION_OUTPUT, 0);

	return 0;
}

static int
impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	return -ENOTSUP;
}

static int
impl_node_remove_port(struct spa_node *node,
				enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int port_enum_formats(struct spa_node *node,
			enum spa_direction direction, uint32_t port_id,
			uint32_t index,
			const struct spa_pod *filter,
			struct spa_pod **param,
			struct spa_pod_builder *builder)
{
	return -ENOTSUP;
}

static int port_get_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t index,
			   const struct spa_pod *filter,
			   struct spa_pod **param,
			   struct spa_pod_builder *builder)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *port;

	port = GET_PORT(this, direction, port_id);

	if (!port->have_format)
		return -EIO;

	if (index > 0)
		return 0;

	*param = NULL;

	return 1;
}

static int
impl_node_port_enum_params(struct spa_node *node, int seq,
			enum spa_direction direction, uint32_t port_id,
			uint32_t id, uint32_t start, uint32_t num,
			const struct spa_pod *filter)
{
	struct impl *this;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	this = SPA_CONTAINER_OF(node, struct impl, node);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(node, direction, port_id,
						result.index, filter, &param, &b)) <= 0)
			return res;
		break;

	case SPA_PARAM_Format:
		if ((res = port_get_format(node, direction, port_id,
						result.index, filter, &param, &b)) <= 0)
			return res;
		break;

	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	if ((res = this->callbacks->result(this->user_data, seq, 0, &result)) != 0)
		return res;

	if (++count != num)
		goto next;

	return 0;
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags, const struct spa_pod *format)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *port;
	int res;

	port = GET_PORT(this, direction, port_id);

	if (format == NULL) {
		port->have_format = false;
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
impl_node_port_set_param(struct spa_node *node,
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
impl_node_port_use_buffers(struct spa_node *node,
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
impl_node_port_alloc_buffers(struct spa_node *node,
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
impl_node_port_set_io(struct spa_node *node,
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

static int
impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	if (node == NULL)
		return -EINVAL;

	if (port_id != 0)
		return -EINVAL;

	return -ENOTSUP;
}

static int impl_node_process(struct spa_node *node)
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

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.set_callbacks = impl_node_set_callbacks,
	.add_port = impl_node_add_port,
	.remove_port = impl_node_remove_port,
	.port_enum_params = impl_node_port_enum_params,
	.port_set_param = impl_node_port_set_param,
	.port_use_buffers = impl_node_port_use_buffers,
	.port_alloc_buffers = impl_node_port_alloc_buffers,
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.process = impl_node_process,
};

static int
impl_get_interface(struct spa_handle *handle, uint32_t type, void **interface)
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
spa_ffmpeg_enc_init(struct spa_handle *handle,
		    const struct spa_dict *info,
		    const struct spa_support *support, uint32_t n_support)
{
	struct impl *this;
	struct port *port;
	uint32_t i;

	handle->get_interface = impl_get_interface;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		if (support[i].type == SPA_TYPE_INTERFACE_Log)
			this->log = support[i].data;
	}

	this->node = impl_node;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = 1;
	this->info.max_output_ports = 1;
	this->info.change_mask |= SPA_NODE_CHANGE_MASK_FLAGS;
	this->info.flags = SPA_NODE_FLAG_RT;
	this->info.params = this->params;

	port = GET_IN_PORT(this, 0);
	port->info = SPA_PORT_INFO_INIT();
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_FLAGS;
	port->info.flags = 0;
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	port->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->info.params = port->params;
	port->info.n_params = 2;

	port = GET_OUT_PORT(this, 0);
	port->info = SPA_PORT_INFO_INIT();
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_FLAGS;
	port->info.flags = 0;
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	port->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->info.params = port->params;
	port->info.n_params = 2;

	return 0;
}
