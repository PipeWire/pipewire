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

#include <spa/support/type-map.h>
#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/utils/list.h>
#include <spa/clock/clock.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/param/meta.h>
#include <spa/param/io.h>

#include <lib/debug.h>
#include <lib/pod.h>

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
	struct spa_buffer *outbuf;
	struct spa_meta_header *h;
	uint32_t flags;
	struct v4l2_buffer v4l2_buffer;
	void *ptr;
};

struct type {
	uint32_t node;
	uint32_t clock;
	uint32_t format;
	uint32_t props;
	uint32_t prop_unknown;
	uint32_t prop_device;
	uint32_t prop_device_name;
	uint32_t prop_device_fd;
	uint32_t prop_brightness;
	uint32_t prop_contrast;
	uint32_t prop_saturation;
	uint32_t prop_hue;
	uint32_t prop_gamma;
	uint32_t prop_exposure;
	uint32_t prop_gain;
	uint32_t prop_sharpness;
	struct spa_type_io io;
	struct spa_type_param param;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_media_subtype_video media_subtype_video;
	struct spa_type_format_video format_video;
	struct spa_type_video_format video_format;
	struct spa_type_event_node event_node;
	struct spa_type_command_node command_node;
	struct spa_type_param_buffers param_buffers;
	struct spa_type_param_meta param_meta;
	struct spa_type_param_io param_io;
	struct spa_type_meta meta;
	struct spa_type_data data;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->node = spa_type_map_get_id(map, SPA_TYPE__Node);
	type->clock = spa_type_map_get_id(map, SPA_TYPE__Clock);
	type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
	type->props = spa_type_map_get_id(map, SPA_TYPE__Props);
	type->prop_unknown = spa_type_map_get_id(map, SPA_TYPE_PROPS__unknown);
	type->prop_device = spa_type_map_get_id(map, SPA_TYPE_PROPS__device);
	type->prop_device_name = spa_type_map_get_id(map, SPA_TYPE_PROPS__deviceName);
	type->prop_device_fd = spa_type_map_get_id(map, SPA_TYPE_PROPS__deviceFd);
	type->prop_brightness = spa_type_map_get_id(map, SPA_TYPE_PROPS__brightness);
	type->prop_contrast = spa_type_map_get_id(map, SPA_TYPE_PROPS__contrast);
	type->prop_saturation = spa_type_map_get_id(map, SPA_TYPE_PROPS__saturation);
	type->prop_hue = spa_type_map_get_id(map, SPA_TYPE_PROPS__hue);
	type->prop_gamma = spa_type_map_get_id(map, SPA_TYPE_PROPS__gamma);
	type->prop_exposure = spa_type_map_get_id(map, SPA_TYPE_PROPS__exposure);
	type->prop_gain = spa_type_map_get_id(map, SPA_TYPE_PROPS__gain);
	type->prop_sharpness = spa_type_map_get_id(map, SPA_TYPE_PROPS__sharpness);
	spa_type_meta_map(map, &type->meta);
	spa_type_data_map(map, &type->data);
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_media_subtype_video_map(map, &type->media_subtype_video);
	spa_type_format_video_map(map, &type->format_video);
	spa_type_video_format_map(map, &type->video_format);
	spa_type_event_node_map(map, &type->event_node);
	spa_type_command_node_map(map, &type->command_node);
	spa_type_param_map(map, &type->param);
	spa_type_param_buffers_map(map, &type->param_buffers);
	spa_type_param_meta_map(map, &type->param_meta);
	spa_type_io_map(map, &type->io);
	spa_type_param_io_map(map, &type->param_io);
}

#define MAX_CONTROLS	64

struct control {
	uint32_t id;
	uint32_t ctrl_id;
	double value;
	double *io;
};

struct port {
	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_loop *data_loop;

	bool export_buf;
	bool started;

	bool next_fmtdesc;
	struct v4l2_fmtdesc fmtdesc;
	bool next_frmsize;
	struct v4l2_frmsizeenum frmsize;
	struct v4l2_frmivalenum frmival;

	bool have_format;
	struct spa_video_info current_format;

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

	struct spa_source source;

	struct spa_port_info info;
	struct spa_io_buffers *io;

	int64_t last_ticks;
	int64_t last_monotonic;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;
	struct spa_clock clock;

	struct spa_type_map *map;
	struct spa_log *log;
	struct type type;

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
	struct type *t;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (id == t->param.idList) {
		uint32_t list[] = { t->param.idPropInfo,
				    t->param.idProps };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b, id, t->param.List,
				":", t->param.listId, "I", list[*index]);
		else
			return 0;
	}
	else if (id == t->param.idPropInfo) {
		struct props *p = &this->props;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, t->param.PropInfo,
				":", t->param.propId,   "I", t->prop_device,
				":", t->param.propName, "s", "The V4L2 device",
				":", t->param.propType, "S", p->device, sizeof(p->device));
			break;
		case 1:
			param = spa_pod_builder_object(&b,
				id, t->param.PropInfo,
				":", t->param.propId,   "I", t->prop_device_name,
				":", t->param.propName, "s", "The V4L2 device name",
				":", t->param.propType, "S-r", p->device_name, sizeof(p->device_name));
			break;
		case 2:
			param = spa_pod_builder_object(&b,
				id, t->param.PropInfo,
				":", t->param.propId,   "I", t->prop_device_fd,
				":", t->param.propName, "s", "The V4L2 fd",
				":", t->param.propType, "i-r", p->device_fd);
			break;
		default:
			return 0;
		}
	}
	else if (id == t->param.idProps) {
		struct props *p = &this->props;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, t->props,
				":", t->prop_device,      "S", p->device, sizeof(p->device),
				":", t->prop_device_name, "S-r", p->device_name, sizeof(p->device_name),
				":", t->prop_device_fd,   "i-r", p->device_fd);
			break;
		default:
			return 0;
		}
	}
	else
		return -ENOENT;

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
	struct type *t;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	if (id == t->param.idProps) {
		struct props *p = &this->props;

		if (param == NULL) {
			reset_props(p);
			return 0;
		}
		spa_pod_object_parse(param,
			":", t->prop_device, "?S", p->device, sizeof(p->device), NULL);
	}
	else
		return -ENOENT;

	return 0;
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct impl *this;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (SPA_COMMAND_TYPE(command) == this->type.command_node.Start) {
		struct port *port = GET_OUT_PORT(this, 0);

		if (!port->have_format)
			return -EIO;
		if (port->n_buffers == 0)
			return -EIO;

		if ((res = spa_v4l2_stream_on(this)) < 0)
			return res;

	} else if (SPA_COMMAND_TYPE(command) == this->type.command_node.Pause) {
		if ((res = spa_v4l2_stream_off(this)) < 0)
			return res;
	} else
		return -ENOTSUP;

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
	struct type *t = &this->type;
	struct port *port = GET_PORT(this, direction, port_id);

	if (!port->have_format)
		return -EIO;
	if (*index > 0)
		return 0;

	spa_pod_builder_push_object(builder, t->param.idFormat, t->format);

	spa_pod_builder_add(builder,
		"I", port->current_format.media_type,
		"I", port->current_format.media_subtype, 0);

	if (port->current_format.media_subtype == t->media_subtype.raw) {
		spa_pod_builder_add(builder,
			":", t->format_video.format,    "I", port->current_format.info.raw.format,
			":", t->format_video.size,      "R", &port->current_format.info.raw.size,
			":", t->format_video.framerate, "F", &port->current_format.info.raw.framerate, 0);
	} else if (port->current_format.media_subtype == t->media_subtype_video.mjpg ||
		   port->current_format.media_subtype == t->media_subtype_video.jpeg) {
		spa_pod_builder_add(builder,
			":", t->format_video.size,      "R", &port->current_format.info.mjpg.size,
			":", t->format_video.framerate, "F", &port->current_format.info.mjpg.framerate, 0);
	} else if (port->current_format.media_subtype == t->media_subtype_video.h264) {
		spa_pod_builder_add(builder,
			":", t->format_video.size,      "R", &port->current_format.info.h264.size,
			":", t->format_video.framerate, "F", &port->current_format.info.h264.framerate, 0);
	} else
		return -EIO;

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
	struct type *t;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

     next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (id == t->param.idList) {
		uint32_t list[] = { t->param.idEnumFormat,
				    t->param.idFormat,
				    t->param.idBuffers,
				    t->param.idMeta,
				    t->param_io.idPropsIn };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b, id, t->param.List,
				":", t->param.listId, "I", list[*index]);
		else
			return 0;
	}
	else if (id == t->param.idEnumFormat) {
		return spa_v4l2_enum_format(this, index, filter, result, builder);
	}
	else if (id == t->param.idFormat) {
		if((res = port_get_format(node, direction, port_id, index, filter, &param, &b)) <= 0)
			return res;
	}
	else if (id == t->param.idBuffers) {
		if (!port->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		param = spa_pod_builder_object(&b,
			id, t->param_buffers.Buffers,
			":", t->param_buffers.size,    "i", port->fmt.fmt.pix.sizeimage,
			":", t->param_buffers.stride,  "i", port->fmt.fmt.pix.bytesperline,
			":", t->param_buffers.buffers, "iru", MAX_BUFFERS,
				SPA_POD_PROP_MIN_MAX(2, MAX_BUFFERS),
			":", t->param_buffers.align,   "i", 16);
	}
	else if (id == t->param.idMeta) {
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, t->param_meta.Meta,
				":", t->param_meta.type, "I", t->meta.Header,
				":", t->param_meta.size, "i", sizeof(struct spa_meta_header));
			break;
		default:
			return 0;
		}
	}
	else if (id == t->param_io.idPropsIn) {
		return spa_v4l2_enum_controls(this, index, filter, result, builder);
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
			   uint32_t flags,
			   const struct spa_pod *format)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct spa_video_info info;
	struct type *t = &this->type;
	struct port *port = GET_PORT(this, direction, port_id);

	if (format == NULL) {
		spa_v4l2_stream_off(this);
		spa_v4l2_clear_buffers(this);
		port->have_format = false;
		spa_v4l2_close(this);
		return 0;
	} else {
		spa_pod_object_parse(format,
			"I", &info.media_type,
			"I", &info.media_subtype);

		if (info.media_type != t->media_type.video) {
			spa_log_error(this->log, "media type must be video");
			return -EINVAL;
		}

		if (info.media_subtype == t->media_subtype.raw) {
			if (spa_format_video_raw_parse(format, &info.info.raw, &t->format_video) < 0) {
				spa_log_error(this->log, "can't parse video raw");
				return -EINVAL;
			}

			if (port->have_format && info.media_type == port->current_format.media_type &&
			    info.media_subtype == port->current_format.media_subtype &&
			    info.info.raw.format == port->current_format.info.raw.format &&
			    info.info.raw.size.width == port->current_format.info.raw.size.width &&
			    info.info.raw.size.height == port->current_format.info.raw.size.height)
				return 0;
		} else if (info.media_subtype == t->media_subtype_video.mjpg) {
			if (spa_format_video_mjpg_parse(format, &info.info.mjpg, &t->format_video) < 0)
				return -EINVAL;

			if (port->have_format && info.media_type == port->current_format.media_type &&
			    info.media_subtype == port->current_format.media_subtype &&
			    info.info.mjpg.size.width == port->current_format.info.mjpg.size.width &&
			    info.info.mjpg.size.height == port->current_format.info.mjpg.size.height)
				return 0;
		} else if (info.media_subtype == t->media_subtype_video.h264) {
			if (spa_format_video_h264_parse(format, &info.info.h264, &t->format_video) < 0)
				return -EINVAL;

			if (port->have_format && info.media_type == port->current_format.media_type &&
			    info.media_subtype == port->current_format.media_subtype &&
			    info.info.h264.size.width == port->current_format.info.h264.size.width &&
			    info.info.h264.size.height == port->current_format.info.h264.size.height)
				return 0;
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
	struct impl *this;
	struct type *t;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (id == t->param.idFormat) {
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

static struct control *find_control(struct port *port, uint32_t id)
{
	int i;

	for (i = 0; i < port->n_controls; i++) {
		if (port->controls[i].id == id)
			return &port->controls[i];
	}
	return NULL;
}

static int impl_node_port_set_io(struct spa_node *node,
				 enum spa_direction direction,
				 uint32_t port_id,
				 uint32_t id,
				 void *data, size_t size)
{
	struct impl *this;
	struct type *t;
	struct port *port;
	struct control *control;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	if (id == t->io.Buffers) {
		port->io = data;
	}
	else if ((control = find_control(port, id))) {
		if (data && size >= sizeof(struct spa_pod_double))
			control->io = &SPA_POD_VALUE(struct spa_pod_double, data);
		else
			control->io = &control->value;
	}
	else
		return -ENOENT;

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

static int impl_node_process_input(struct spa_node *node)
{
	return -ENOTSUP;
}

static int impl_node_process_output(struct spa_node *node)
{
	struct impl *this;
	int i, res = SPA_STATUS_OK;
	struct spa_io_buffers *io;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	port = GET_OUT_PORT(this, 0);
	io = port->io;
	spa_return_val_if_fail(io != NULL, -EIO);

	if (io->status == SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_HAVE_BUFFER;

	if (io->buffer_id < port->n_buffers) {
		res = spa_v4l2_buffer_recycle(this, io->buffer_id);
		io->buffer_id = SPA_ID_INVALID;
	}
	for (i = 0; i < port->n_controls; i++) {
		struct control *control = &port->controls[i];

		if (control->io == NULL)
			continue;

		if (control->value != *control->io) {
			struct v4l2_control c;

			memset (&c, 0, sizeof (c));
			c.id = control->ctrl_id;
			c.value = *control->io;

			if (ioctl(port->fd, VIDIOC_S_CTRL, &c) < 0)
				spa_log_error(port->log, "VIDIOC_S_CTRL %m");

			control->value = *control->io = c.value;
		}
	}
	return res;
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
	impl_node_process_input,
	impl_node_process_output,
};

static int impl_clock_enum_params(struct spa_clock *clock, uint32_t id, uint32_t *index,
				  struct spa_pod **param,
				  struct spa_pod_builder *builder)
{
	return -ENOTSUP;
}

static int impl_clock_set_param(struct spa_clock *clock,
				uint32_t id, uint32_t flags,
				const struct spa_pod *param)
{
	return -ENOTSUP;
}

static int impl_clock_get_time(struct spa_clock *clock,
			       int32_t *rate,
			       int64_t *ticks,
			       int64_t *monotonic_time)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(clock != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(clock, struct impl, clock);
	port = GET_OUT_PORT(this, 0);

	if (rate)
		*rate = SPA_USEC_PER_SEC;
	if (ticks)
		*ticks = port->last_ticks;
	if (monotonic_time)
		*monotonic_time = port->last_monotonic;

	return 0;
}

static const struct spa_clock impl_clock = {
	SPA_VERSION_CLOCK,
	NULL,
	SPA_CLOCK_STATE_STOPPED,
	impl_clock_enum_params,
	impl_clock_set_param,
	impl_clock_get_time,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t interface_id, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (interface_id == this->type.node)
		*interface = &this->node;
	else if (interface_id == this->type.clock)
		*interface = &this->clock;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	return 0;
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

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear, this = (struct impl *) handle;

	port = GET_OUT_PORT(this, 0);

	for (i = 0; i < n_support; i++) {
		if (strcmp(support[i].type, SPA_TYPE__TypeMap) == 0)
			this->map = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE__Log) == 0)
			this->log = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE_LOOP__MainLoop) == 0)
			port->main_loop = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE_LOOP__DataLoop) == 0)
			port->data_loop = support[i].data;
	}
	if (this->map == NULL) {
		spa_log_error(this->log, "a type-map is needed");
		return -EINVAL;
	}
	if (port->main_loop == NULL) {
		spa_log_error(this->log, "a main_loop is needed");
		return -EINVAL;
	}
	if (port->data_loop == NULL) {
		spa_log_error(this->log, "a data_loop is needed");
		return -EINVAL;
	}
	init_type(&this->type, this->map);

	this->node = impl_node;
	this->clock = impl_clock;

	reset_props(&this->props);

	port->log = this->log;
	port->info.flags = SPA_PORT_INFO_FLAG_LIVE |
			   SPA_PORT_INFO_FLAG_PHYSICAL |
			   SPA_PORT_INFO_FLAG_TERMINAL;
	port->export_buf = true;
	port->have_query_ext_ctrl = true;

	if (info && (str = spa_dict_lookup(info, "device.path"))) {
		strncpy(this->props.device, str, 63);
	}

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE__Node,},
	{SPA_TYPE__Clock,},
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
	sizeof(struct impl),
	impl_init,
	impl_enum_interface_info,
};
