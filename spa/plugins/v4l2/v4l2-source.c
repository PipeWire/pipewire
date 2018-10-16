/* Spa V4l2 Source
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

#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <linux/videodev2.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/utils/list.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/param.h>
#include <spa/pod/filter.h>
#include <spa/control/control.h>
#include <spa/debug/pod.h>

#define NAME "v4l2-source"

static const char default_device[] = "/dev/video0";

struct props {
	char device[64];
	char device_name[128];
	int device_fd;
};

static void reset_props(struct props *props)
{
	strncpy(props->device, default_device, 64);
}

#define MAX_BUFFERS     64

#define BUFFER_FLAG_OUTSTANDING	(1<<0)
#define BUFFER_FLAG_ALLOCATED	(1<<1)
#define BUFFER_FLAG_MAPPED	(1<<2)

struct buffer {
	struct spa_list link;
	struct spa_buffer *outbuf;
	struct spa_meta_header *h;
	uint32_t flags;
	struct v4l2_buffer v4l2_buffer;
	void *ptr;
};

#define MAX_CONTROLS	64

struct control {
	uint32_t id;
	uint32_t ctrl_id;
	double value;
};

struct port {
	struct impl *impl;

	bool export_buf;
	bool started;

	bool next_fmtdesc;
	struct v4l2_fmtdesc fmtdesc;
	bool next_frmsize;
	struct v4l2_frmsizeenum frmsize;
	struct v4l2_frmivalenum frmival;

	bool have_format;
	struct spa_video_info current_format;
	struct spa_fraction rate;

	int fd;
	bool opened;
	bool have_query_ext_ctrl;
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	enum v4l2_buf_type type;
	enum v4l2_memory memtype;

	struct control controls[MAX_CONTROLS];
	uint32_t n_controls;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;
	struct spa_list queue;

	struct spa_source source;

	struct spa_port_info info;
	struct spa_io_buffers *io;
	struct spa_io_clock *clock;
	struct spa_io_sequence *control;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_loop *data_loop;

	uint32_t seq;

	struct props props;

	const struct spa_node_callbacks *callbacks;
	void *callbacks_data;

	struct port out_ports[1];
};

#define CHECK_PORT(this,direction,port_id)  ((direction) == SPA_DIRECTION_OUTPUT && (port_id) == 0)

#define GET_OUT_PORT(this,p)         (&this->out_ports[p])
#define GET_PORT(this,d,p)           GET_OUT_PORT(this,p)

#include "v4l2-utils.c"

static int impl_node_enum_params(struct spa_node *node,
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
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_PropInfo,
				    SPA_PARAM_Props };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b,
					SPA_TYPE_OBJECT_ParamList, id,
					SPA_PARAM_LIST_id, &SPA_POD_Id(list[*index]),
					0);
		else
			return 0;
		break;
	}
	case SPA_PARAM_PropInfo:
	{
		struct props *p = &this->props;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   &SPA_POD_Id(SPA_PROP_device),
				SPA_PROP_INFO_name, &SPA_POD_Stringc("The V4L2 device"),
				SPA_PROP_INFO_type, &SPA_POD_Stringv(p->device),
				0);
			break;
		case 1:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   &SPA_POD_Id(SPA_PROP_deviceName),
				SPA_PROP_INFO_name, &SPA_POD_Stringc("The V4L2 device name"),
				SPA_PROP_INFO_type, &SPA_POD_Stringv(p->device_name),
				0);
			break;
		case 2:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   &SPA_POD_Id(SPA_PROP_deviceFd),
				SPA_PROP_INFO_name, &SPA_POD_Stringc("The V4L2 fd"),
				SPA_PROP_INFO_type, &SPA_POD_Int(p->device_fd),
				0);
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_Props, id,
				SPA_PROP_device,     &SPA_POD_Stringv(p->device),
				SPA_PROP_deviceName, &SPA_POD_Stringv(p->device_name),
				SPA_PROP_deviceFd,   &SPA_POD_Int(p->device_fd),
				0);
			break;
		default:
			return 0;
		}
		break;
	}
	default:
		return -ENOENT;
	}

	(*index)++;

	if (spa_pod_filter(builder, result, param, filter) < 0)
		goto next;

	return 1;
}

static int impl_node_set_param(struct spa_node *node,
			       uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	switch (id) {
	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;

		if (param == NULL) {
			reset_props(p);
			return 0;
		}
		spa_pod_object_parse(param,
			":", SPA_PROP_device, "?S", p->device, sizeof(p->device), NULL);
		break;
	}
	default:
		return -ENOENT;
	}
	return 0;
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
	{
		struct port *port = GET_OUT_PORT(this, 0);

		if (!port->have_format)
			return -EIO;
		if (port->n_buffers == 0)
			return -EIO;

		if ((res = spa_v4l2_stream_on(this)) < 0)
			return res;
		break;
	}
	case SPA_NODE_COMMAND_Pause:
		if ((res = spa_v4l2_stream_off(this)) < 0)
			return res;
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

static int impl_node_set_callbacks(struct spa_node *node,
				   const struct spa_node_callbacks *callbacks,
				   void *data)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

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
	spa_return_val_if_fail(node != NULL, -EINVAL);

	if (n_input_ports)
		*n_input_ports = 0;
	if (max_input_ports)
		*max_input_ports = 0;
	if (n_output_ports)
		*n_output_ports = 1;
	if (max_output_ports)
		*max_output_ports = 1;

	return 0;
}

static int impl_node_get_port_ids(struct spa_node *node,
				  uint32_t *input_ids,
				  uint32_t n_input_ids,
				  uint32_t *output_ids,
				  uint32_t n_output_ids)
{
	spa_return_val_if_fail(node != NULL, -EINVAL);

	if (n_output_ids > 0 && output_ids != NULL)
		output_ids[0] = 0;

	return 0;
}


static int impl_node_add_port(struct spa_node *node,
			      enum spa_direction direction,
			      uint32_t port_id)
{
	return -ENOTSUP;
}

static int impl_node_remove_port(struct spa_node *node,
		                 enum spa_direction direction,
				 uint32_t port_id)
{
	return -ENOTSUP;
}

static int impl_node_port_get_info(struct spa_node *node,
				   enum spa_direction direction,
				   uint32_t port_id,
				   const struct spa_port_info **info)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	*info = &GET_PORT(this, direction, port_id)->info;

	return 0;
}

static int port_get_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t *index,
			   const struct spa_pod *filter,
			   struct spa_pod **param,
			   struct spa_pod_builder *builder)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *port = GET_PORT(this, direction, port_id);

	if (!port->have_format)
		return -EIO;
	if (*index > 0)
		return 0;

	spa_pod_builder_push_object(builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_Format);
	spa_pod_builder_props(builder,
		SPA_FORMAT_mediaType,    &SPA_POD_Id(port->current_format.media_type),
		SPA_FORMAT_mediaSubtype, &SPA_POD_Id(port->current_format.media_subtype),
		0);

	switch (port->current_format.media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		spa_pod_builder_props(builder,
			SPA_FORMAT_VIDEO_format,    &SPA_POD_Id(port->current_format.info.raw.format),
			SPA_FORMAT_VIDEO_size,      &SPA_POD_Rectangle(port->current_format.info.raw.size),
			SPA_FORMAT_VIDEO_framerate, &SPA_POD_Fraction(port->current_format.info.raw.framerate),
			0);
		break;
	case SPA_MEDIA_SUBTYPE_mjpg:
	case SPA_MEDIA_SUBTYPE_jpeg:
		spa_pod_builder_props(builder,
			SPA_FORMAT_VIDEO_size,      &SPA_POD_Rectangle(port->current_format.info.mjpg.size),
			SPA_FORMAT_VIDEO_framerate, &SPA_POD_Fraction(port->current_format.info.mjpg.framerate),
			0);
		break;
	case SPA_MEDIA_SUBTYPE_h264:
		spa_pod_builder_props(builder,
			SPA_FORMAT_VIDEO_size,      &SPA_POD_Rectangle(port->current_format.info.h264.size),
			SPA_FORMAT_VIDEO_framerate, &SPA_POD_Fraction(port->current_format.info.h264.framerate),
			0);
		break;
	default:
		return -EIO;
	}

	*param = spa_pod_builder_pop(builder);

	return 1;
}

static int impl_node_port_enum_params(struct spa_node *node,
				      enum spa_direction direction,
				      uint32_t port_id,
				      uint32_t id, uint32_t *index,
				      const struct spa_pod *filter,
				      struct spa_pod **result,
				      struct spa_pod_builder *builder)
{

	struct impl *this;
	struct port *port;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

     next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_PropInfo,
				    SPA_PARAM_EnumFormat,
				    SPA_PARAM_Format,
				    SPA_PARAM_Buffers,
				    SPA_PARAM_Meta,
				    SPA_PARAM_IO };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b,
					SPA_TYPE_OBJECT_ParamList, id,
					SPA_PARAM_LIST_id, &SPA_POD_Id(list[*index]),
					0);
		else
			return 0;
		break;
	}
	case SPA_PARAM_PropInfo:
		return spa_v4l2_enum_controls(this, index, filter, result, builder);

	case SPA_PARAM_EnumFormat:
		return spa_v4l2_enum_format(this, index, filter, result, builder);

	case SPA_PARAM_Format:
		if((res = port_get_format(node, direction, port_id, index, filter, &param, &b)) <= 0)
			return res;
		break;
	case SPA_PARAM_Buffers:
		if (!port->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		param = spa_pod_builder_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, &SPA_POD_CHOICE_RANGE_Int(MAX_BUFFERS, 2, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  &SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    &SPA_POD_Int(port->fmt.fmt.pix.sizeimage),
			SPA_PARAM_BUFFERS_stride,  &SPA_POD_Int(port->fmt.fmt.pix.bytesperline),
			SPA_PARAM_BUFFERS_align,   &SPA_POD_Int(16),
			0);
		break;

	case SPA_PARAM_Meta:
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, &SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, &SPA_POD_Int(sizeof(struct spa_meta_header)),
				0);
			break;
		default:
			return 0;
		}
		break;
	case SPA_PARAM_IO:
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   &SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, &SPA_POD_Int(sizeof(struct spa_io_buffers)),
				0);
			break;
		case 1:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   &SPA_POD_Id(SPA_IO_Clock),
				SPA_PARAM_IO_size, &SPA_POD_Int(sizeof(struct spa_io_clock)),
				0);
			break;
		case 2:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   &SPA_POD_Id(SPA_IO_Control),
				SPA_PARAM_IO_size, &SPA_POD_Int(sizeof(struct spa_io_sequence)),
				0);
			break;
		default:
			return 0;
		}
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
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct spa_video_info info;
	struct port *port = GET_PORT(this, direction, port_id);
	int res;

	if (format == NULL) {
		spa_v4l2_stream_off(this);
		spa_v4l2_clear_buffers(this);
		port->have_format = false;
		spa_v4l2_close(this);
		return 0;
	} else {
		if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return res;

		if (info.media_type != SPA_MEDIA_TYPE_video) {
			spa_log_error(this->log, "media type must be video");
			return -EINVAL;
		}

		switch (info.media_subtype) {
		case SPA_MEDIA_SUBTYPE_raw:
			if (spa_format_video_raw_parse(format, &info.info.raw) < 0) {
				spa_log_error(this->log, "can't parse video raw");
				return -EINVAL;
			}

			if (port->have_format && info.media_type == port->current_format.media_type &&
			    info.media_subtype == port->current_format.media_subtype &&
			    info.info.raw.format == port->current_format.info.raw.format &&
			    info.info.raw.size.width == port->current_format.info.raw.size.width &&
			    info.info.raw.size.height == port->current_format.info.raw.size.height)
				return 0;
			break;
		case SPA_MEDIA_SUBTYPE_mjpg:
			if (spa_format_video_mjpg_parse(format, &info.info.mjpg) < 0)
				return -EINVAL;

			if (port->have_format && info.media_type == port->current_format.media_type &&
			    info.media_subtype == port->current_format.media_subtype &&
			    info.info.mjpg.size.width == port->current_format.info.mjpg.size.width &&
			    info.info.mjpg.size.height == port->current_format.info.mjpg.size.height)
				return 0;
			break;
		case SPA_MEDIA_SUBTYPE_h264:
			if (spa_format_video_h264_parse(format, &info.info.h264) < 0)
				return -EINVAL;

			if (port->have_format && info.media_type == port->current_format.media_type &&
			    info.media_subtype == port->current_format.media_subtype &&
			    info.info.h264.size.width == port->current_format.info.h264.size.width &&
			    info.info.h264.size.height == port->current_format.info.h264.size.height)
				return 0;
			break;
		default:
			return -EINVAL;
		}
	}

	if (port->have_format && !(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)) {
		spa_v4l2_use_buffers(this, NULL, 0);
		port->have_format = false;
	}

	if (spa_v4l2_set_format(this, &info, flags & SPA_NODE_PARAM_FLAG_TEST_ONLY) < 0)
		return -EINVAL;

	if (!(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)) {
		port->current_format = info;
		port->have_format = true;
	}

	return 0;
}

static int impl_node_port_set_param(struct spa_node *node,
				    enum spa_direction direction, uint32_t port_id,
				    uint32_t id, uint32_t flags,
				    const struct spa_pod *param)
{
	spa_return_val_if_fail(node != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(node, direction, port_id), -EINVAL);

	if (id == SPA_PARAM_Format) {
		return port_set_format(node, direction, port_id, flags, param);
	}
	else
		return -ENOENT;
}

static int impl_node_port_use_buffers(struct spa_node *node,
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

	if (!port->have_format)
		return -EIO;

	if (port->n_buffers) {
		spa_v4l2_stream_off(this);
		if ((res = spa_v4l2_clear_buffers(this)) < 0)
			return res;
	}
	if (buffers != NULL) {
		if ((res = spa_v4l2_use_buffers(this, buffers, n_buffers)) < 0)
			return res;
	}
	return 0;
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
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(buffers != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	if (!port->have_format)
		return -EIO;

	res = spa_v4l2_alloc_buffers(this, params, n_params, buffers, n_buffers);

	return res;
}

static int impl_node_port_set_io(struct spa_node *node,
				 enum spa_direction direction,
				 uint32_t port_id,
				 uint32_t id,
				 void *data, size_t size)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	switch (id) {
	case SPA_IO_Buffers:
		port->io = data;
		break;
	case SPA_IO_Clock:
		port->clock = data;
		break;
	case SPA_IO_Control:
		port->control = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_port_reuse_buffer(struct spa_node *node,
				       uint32_t port_id,
				       uint32_t buffer_id)
{
	struct impl *this;
	struct port *port;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(port_id == 0, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	port = GET_OUT_PORT(this, port_id);

	spa_return_val_if_fail(buffer_id < port->n_buffers, -EINVAL);

	res = spa_v4l2_buffer_recycle(this, buffer_id);

	return res;
}

static int impl_node_port_send_command(struct spa_node *node,
				       enum spa_direction direction,
				       uint32_t port_id,
				       const struct spa_command *command)
{
	return -ENOTSUP;
}

static uint32_t prop_to_control_id(uint32_t prop)
{
	switch (prop) {
	case SPA_PROP_brightness:
		return V4L2_CID_BRIGHTNESS;
	case SPA_PROP_contrast:
		return V4L2_CID_CONTRAST;
	case SPA_PROP_saturation:
		return V4L2_CID_SATURATION;
	case SPA_PROP_hue:
		return V4L2_CID_HUE;
	case SPA_PROP_gamma:
		return V4L2_CID_GAMMA;
	case SPA_PROP_exposure:
		return V4L2_CID_EXPOSURE;
	case SPA_PROP_gain:
		return V4L2_CID_GAIN;
	case SPA_PROP_sharpness:
		return V4L2_CID_SHARPNESS;
	default:
		return 0;
	}
}

static void set_control(struct impl *this, struct port *port, uint32_t control_id, float value)
{
	struct v4l2_control c;

	spa_zero(c);
	c.id = control_id;
	c.value = value;
	if (ioctl(port->fd, VIDIOC_S_CTRL, &c) < 0)
		spa_log_error(this->log, "VIDIOC_S_CTRL %m");
}

static int process_control(struct impl *this, struct spa_pod_sequence *control)
{
	struct spa_pod_control *c;
	struct port *port;

	SPA_POD_SEQUENCE_FOREACH(control, c) {
		switch (c->type) {
		case SPA_CONTROL_Properties:
		{
			struct spa_pod_prop *prop;
			struct spa_pod_object *obj = (struct spa_pod_object *) &c->value;

			SPA_POD_OBJECT_FOREACH(obj, prop) {
				uint32_t control_id;

				if ((control_id = prop_to_control_id(prop->key)) == 0)
					continue;

				port = GET_OUT_PORT(this, prop->context);
				set_control(this, port, control_id,
						SPA_POD_VALUE(struct spa_pod_float, &prop->value));
			}
			break;
		}
		default:
			break;
		}
	}
	return 0;
}

static int impl_node_process(struct spa_node *node)
{
	struct impl *this;
	int res;
	struct spa_io_buffers *io;
	struct port *port;
	struct buffer *b;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	port = GET_OUT_PORT(this, 0);
	io = port->io;
	spa_return_val_if_fail(io != NULL, -EIO);

	if (port->control)
		process_control(this, &port->control->sequence);

	spa_log_trace(this->log, NAME " %p; status %d", node, io->status);

	if (io->status == SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_HAVE_BUFFER;

	if (io->buffer_id < port->n_buffers) {
		if ((res = spa_v4l2_buffer_recycle(this, io->buffer_id)) < 0)
			return res;

		io->buffer_id = SPA_ID_INVALID;
	}

	if (spa_list_is_empty(&port->queue))
		return -EPIPE;

	b = spa_list_first(&port->queue, struct buffer, link);
	spa_list_remove(&b->link);

	spa_log_trace(this->log, NAME " %p: dequeue buffer %d", node, b->outbuf->id);

	io->buffer_id = b->outbuf->id;
	io->status = SPA_STATUS_HAVE_BUFFER;

	return SPA_STATUS_HAVE_BUFFER;
}

static const struct spa_dict_item info_items[] = {
	{ "media.class", "Video/Source" },
	{ "node.pause-on-idle", "false" },
	{ "node.driver", "true" },
};

static const struct spa_dict info = {
	info_items,
	SPA_N_ELEMENTS(info_items)
};

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	&info,
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
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
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
	const char *str;
	struct port *port;
	int res;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear, this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		if (support[i].type == SPA_TYPE_INTERFACE_Log)
			this->log = support[i].data;
		else if (support[i].type == SPA_TYPE_INTERFACE_MainLoop)
			this->main_loop = support[i].data;
		else if (support[i].type == SPA_TYPE_INTERFACE_DataLoop)
			this->data_loop = support[i].data;
	}
	if (this->main_loop == NULL) {
		spa_log_error(this->log, "a main_loop is needed");
		return -EINVAL;
	}
	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data_loop is needed");
		return -EINVAL;
	}

	this->node = impl_node;

	reset_props(&this->props);

	port = GET_OUT_PORT(this, 0);
	spa_list_init(&port->queue);
	port->info.flags = SPA_PORT_INFO_FLAG_LIVE |
			   SPA_PORT_INFO_FLAG_PHYSICAL |
			   SPA_PORT_INFO_FLAG_TERMINAL;
	port->export_buf = true;
	port->have_query_ext_ctrl = true;

	if (info && (str = spa_dict_lookup(info, "device.path"))) {
		strncpy(this->props.device, str, 63);
		if ((res = spa_v4l2_open(this)) < 0)
			return res;
		spa_v4l2_close(this);
	}

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int impl_enum_interface_info(const struct spa_handle_factory *factory,
				    const struct spa_interface_info **info,
				    uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;

	*info = &impl_interfaces[(*index)++];

	return 1;
}

const struct spa_handle_factory spa_v4l2_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
