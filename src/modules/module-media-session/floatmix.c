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
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/param/meta.h>
#include <spa/param/io.h>

#include <spa/lib/pod.h>

#define NAME "floatmix"

#define MAX_BUFFERS     64
#define MAX_PORTS       128
#define MAX_SAMPLES     1024

#define PORT_DEFAULT_VOLUME	1.0
#define PORT_DEFAULT_MUTE	false

struct port_props {
	double volume;
	int32_t mute;
};

static void port_props_reset(struct port_props *props)
{
	props->volume = PORT_DEFAULT_VOLUME;
	props->mute = PORT_DEFAULT_MUTE;
}

struct buffer {
	struct spa_list link;
#define BUFFER_FLAG_QUEUED	(1 << 0)
	uint32_t flags;

	struct spa_buffer *buffer;
	struct spa_meta_header *h;

	uint32_t id;
	struct spa_buffer buf;
        struct spa_data datas[1];
        struct spa_chunk chunk[1];
};

struct port {
	bool valid;
	uint32_t id;

	struct port_props props;

	struct spa_io_buffers *io;
	struct spa_io_control_range *ctrl;
	double *io_volume;
	int32_t *io_mute;

	struct spa_port_info info;

	bool have_format;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_list queue;
	size_t queued_bytes;
};

struct type {
	uint32_t node;
	uint32_t format;
	uint32_t prop_volume;
	uint32_t prop_mute;
	uint32_t io_prop_volume;
	uint32_t io_prop_mute;
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
	type->prop_volume = spa_type_map_get_id(map, SPA_TYPE_PROPS__volume);
	type->prop_mute = spa_type_map_get_id(map, SPA_TYPE_PROPS__mute);
	type->io_prop_volume = spa_type_map_get_id(map, SPA_TYPE_IO_PROP_BASE "volume");
	type->io_prop_mute = spa_type_map_get_id(map, SPA_TYPE_IO_PROP_BASE "mute");
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

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct type type;
	struct spa_type_map *map;
	struct spa_log *log;

	const struct spa_node_callbacks *callbacks;
	void *user_data;

	int port_count;
	int last_port;
	struct port in_ports[MAX_PORTS];
	struct port out_ports[1];

	bool have_format;
	int n_formats;
	struct spa_audio_info format;
	uint32_t stride;

	bool started;
        float empty[MAX_SAMPLES];
};

#define CHECK_FREE_IN_PORT(this,d,p) ((d) == SPA_DIRECTION_INPUT && (p) < MAX_PORTS && !this->in_ports[(p)].valid)
#define CHECK_IN_PORT(this,d,p)      ((d) == SPA_DIRECTION_INPUT && (p) < MAX_PORTS && this->in_ports[(p)].valid)
#define CHECK_OUT_PORT(this,d,p)     ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)
#define CHECK_PORT(this,d,p)         (CHECK_OUT_PORT(this,d,p) || CHECK_IN_PORT (this,d,p))
#define GET_IN_PORT(this,p)          (&this->in_ports[p])
#define GET_OUT_PORT(this,p)         (&this->out_ports[p])
#define GET_PORT(this,d,p)           (d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,p) : GET_OUT_PORT(this,p))

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
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (n_input_ports)
		*n_input_ports = this->port_count;
	if (max_input_ports)
		*max_input_ports = MAX_PORTS;
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
	struct impl *this;
	int i, idx;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (input_ids) {
		for (i = 0, idx = 0; i < this->last_port && idx < n_input_ids; i++) {
			if (this->in_ports[i].valid)
				input_ids[idx++] = i;
		}
	}
	if (n_output_ids > 0 && output_ids)
		output_ids[0] = 0;

	return 0;
}

static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_FREE_IN_PORT(this, direction, port_id), -EINVAL);

	port = GET_IN_PORT (this, port_id);
	port->id = port_id;

	port_props_reset(&port->props);
	port->io_volume = &port->props.volume;
	port->io_mute = &port->props.mute;

	spa_list_init(&port->queue);
	port->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
			   SPA_PORT_INFO_FLAG_REMOVABLE |
			   SPA_PORT_INFO_FLAG_OPTIONAL |
			   SPA_PORT_INFO_FLAG_IN_PLACE;

	this->port_count++;
	if (this->last_port <= port_id)
		this->last_port = port_id + 1;
	port->valid = true;

	spa_log_info(this->log, NAME " %p: add port %d %d", this, port_id, this->last_port);

	return 0;
}

static int
impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_IN_PORT(this, direction, port_id), -EINVAL);

	port = GET_IN_PORT (this, port_id);

	port->valid = false;
	this->port_count--;
	if (port->have_format && this->have_format) {
		if (--this->n_formats == 0)
			this->have_format = false;
	}
	spa_memzero(port, sizeof(struct port));

	if (port_id == this->last_port - 1) {
		int i;

		for (i = this->last_port - 1; i >= 0; i--)
			if (GET_IN_PORT (this, i)->valid)
				break;

		this->last_port = i + 1;
	}
	spa_log_info(this->log, NAME " %p: remove port %d %d", this, port_id, this->last_port);

	return 0;
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
	*info = &port->info;

	return 0;
}

static int port_enum_formats(struct spa_node *node,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t *index,
			     struct spa_pod **param,
			     struct spa_pod_builder *builder)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct type *t = &this->type;

	switch (*index) {
	case 0:
		if (this->have_format) {
			*param = spa_pod_builder_object(builder,
				t->param.idEnumFormat, t->format,
				"I", t->media_type.audio,
				"I", t->media_subtype.raw,
				":", t->format_audio.format,   "I", this->format.info.raw.format,
				":", t->format_audio.layout,   "i", this->format.info.raw.layout,
				":", t->format_audio.rate,     "i", this->format.info.raw.rate,
				":", t->format_audio.channels, "i", this->format.info.raw.channels);
		} else {
			*param = spa_pod_builder_object(builder,
				t->param.idEnumFormat, t->format,
				"I", t->media_type.audio,
				"I", t->media_subtype.raw,
				":", t->format_audio.format,   "I", t->audio_format.F32,
				":", t->format_audio.layout,   "i", SPA_AUDIO_LAYOUT_NON_INTERLEAVED,
				":", t->format_audio.rate,     "iru", 44100,
					SPA_POD_PROP_MIN_MAX(1, INT32_MAX),
				":", t->format_audio.channels, "iru", 1);
		}
		break;
	default:
		return 0;
	}
	return 1;
}

static int port_get_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t *index,
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

	*param = spa_pod_builder_object(builder,
		t->param.idFormat, t->format,
		"I", t->media_type.audio,
		"I", t->media_subtype.raw,
		":", t->format_audio.format,   "I", this->format.info.raw.format,
		":", t->format_audio.layout,   "i", this->format.info.raw.layout,
		":", t->format_audio.rate,     "i", this->format.info.raw.rate,
		":", t->format_audio.channels, "i", this->format.info.raw.channels);

	return 1;
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
	struct type *t;
	struct port *port;
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
				    t->param_io.idBuffers,
				    t->param_io.idControl,
				    t->param_io.idPropsIn };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b, id, t->param.List,
				":", t->param.listId, "I", list[*index]);
		else
			return 0;
	}
	else if (id == t->param.idEnumFormat) {
		if ((res = port_enum_formats(node, direction, port_id, index, &param, &b)) <= 0)
			return res;
	}
	else if (id == t->param.idFormat) {
		if ((res = port_get_format(node, direction, port_id, index, &param, &b)) <= 0)
			return res;
	}
	else if (id == t->param.idBuffers) {
		if (!port->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		param = spa_pod_builder_object(&b,
			id, t->param_buffers.Buffers,
			":", t->param_buffers.size,    "iru", 1024 * this->stride,
				SPA_POD_PROP_MIN_MAX(16 * this->stride, INT32_MAX / this->stride),
			":", t->param_buffers.stride,  "i", this->stride,
			":", t->param_buffers.buffers, "iru", 1,
				SPA_POD_PROP_MIN_MAX(1, MAX_BUFFERS),
			":", t->param_buffers.align,   "i", 16);
	}
	else if (id == t->param.idMeta) {
		if (!port->have_format)
			return -EIO;

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
	else if (id == t->param_io.idBuffers) {
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, t->param_io.Buffers,
				":", t->param_io.id,   "I", t->io.Buffers,
				":", t->param_io.size, "i", sizeof(struct spa_io_buffers));
			break;
		default:
			return 0;
		}
	}
	else if (id == t->param_io.idControl) {
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, t->param_io.Control,
				":", t->param_io.id,   "I", t->io.ControlRange,
				":", t->param_io.size, "i", sizeof(struct spa_io_control_range));
			break;
		default:
			return 0;
		}
	}
	else if (id == t->param_io.idPropsIn) {
		struct port_props *p = &port->props;

		if (direction == SPA_DIRECTION_OUTPUT)
			return 0;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				id, t->param_io.Prop,
				":", t->param_io.id,    "I", t->io_prop_volume,
				":", t->param_io.size,  "i", sizeof(struct spa_pod_double),
				":", t->param.propId,   "I", t->prop_volume,
				":", t->param.propType, "dru", p->volume,
					SPA_POD_PROP_MIN_MAX(0.0, 10.0));
			break;
		case 1:
			param = spa_pod_builder_object(&b,
				id, t->param_io.Prop,
				":", t->param_io.id,    "I", t->io_prop_mute,
				":", t->param_io.size,  "i", sizeof(struct spa_pod_bool),
				":", t->param.propId,   "I", t->prop_mute,
				":", t->param.propType, "b", p->mute);
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

static int clear_buffers(struct impl *this, struct port *port)
{
	if (port->n_buffers > 0) {
		spa_log_info(this->log, NAME " %p: clear buffers %p", this, port);
		port->n_buffers = 0;
		spa_list_init(&port->queue);
	}
	return 0;
}

static int queue_buffer(struct impl *this, struct port *port, struct buffer *b)
{
	if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_QUEUED))
		return -EINVAL;

	spa_list_append(&port->queue, &b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_QUEUED);
	spa_log_trace(this->log, NAME " %p: queue buffer %d", this, b->buffer->id);
	return 0;
}

static struct buffer *dequeue_buffer(struct impl *this, struct port *port)
{
	struct buffer *b;

	if (spa_list_is_empty(&port->queue))
		return NULL;

	b = spa_list_first(&port->queue, struct buffer, link);
	spa_list_remove(&b->link);
	SPA_FLAG_UNSET(b->flags, BUFFER_FLAG_QUEUED);
	spa_log_trace(this->log, NAME " %p: dequeue buffer %d", this, b->buffer->id);
	return b;
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *port;
	struct type *t = &this->type;

	port = GET_PORT(this, direction, port_id);

	if (format == NULL) {
		if (port->have_format) {
			port->have_format = false;
			if (--this->n_formats == 0)
				this->have_format = false;
			clear_buffers(this, port);
		}
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

		if (this->have_format) {
			if (memcmp(&info, &this->format, sizeof(struct spa_audio_info)))
				return -EINVAL;
		} else {
			if (info.info.raw.format != t->audio_format.F32)
				return -EINVAL;

			this->stride = sizeof(float);
			this->have_format = true;
			this->format = info;
		}
		if (!port->have_format) {
			this->n_formats++;
			port->have_format = true;
			spa_log_info(this->log, NAME " %p: set format on port %d:%d",
					this, direction, port_id);
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

static int
impl_node_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this;
	struct port *port;
	uint32_t i;
	struct type *t;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	spa_return_val_if_fail(port->have_format, -EIO);

	spa_log_info(this->log, NAME " %p: use buffers %d on port %d:%d",
			this, n_buffers, direction, port_id);

	clear_buffers(this, port);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

		b = &port->buffers[i];
		b->buffer = buffers[i];
		b->flags = 0;
		b->id = i;
		b->h = spa_buffer_find_meta_data(buffers[i], t->meta.Header, sizeof(*b->h));

		if (!((d[0].type == t->data.MemPtr ||
		       d[0].type == t->data.MemFd ||
		       d[0].type == t->data.DmaBuf) && d[0].data != NULL)) {
			spa_log_error(this->log, NAME " %p: invalid memory on buffer %p", this,
				      buffers[i]);
			return -EINVAL;
		}
		if (direction == SPA_DIRECTION_OUTPUT)
			queue_buffer(this, port, b);
	}
	port->n_buffers = n_buffers;

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
	return -ENOTSUP;
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
	else if (id == t->io.ControlRange)
		port->ctrl = data;
	else if (id == t->io_prop_volume && direction == SPA_DIRECTION_INPUT)
		if (data && size >= sizeof(struct spa_pod_double))
			port->io_volume = &SPA_POD_VALUE(struct spa_pod_double, data);
		else
			port->io_volume = &port->props.volume;
	else if (id == t->io_prop_mute && direction == SPA_DIRECTION_INPUT)
		if (data && size >= sizeof(struct spa_pod_bool))
			port->io_mute = &SPA_POD_VALUE(struct spa_pod_bool, data);
		else
			port->io_mute = &port->props.mute;
	else
		return -ENOENT;

	return 0;
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);
	port = GET_OUT_PORT(this, 0);

	if (buffer_id >= port->n_buffers)
		return -EINVAL;

	return queue_buffer(this, port, &port->buffers[buffer_id]);
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    const struct spa_command *command)
{
	return -ENOTSUP;
}


#if defined (__SSE__)
#include <xmmintrin.h>
static void mix_2(float *dst, float *src1, float *src2, int n_samples)
{
	int i, unrolled;
	__m128 in[2];

	unrolled = n_samples / 4;
	n_samples &= 3;

	for (i = 0; unrolled--; i += 4) {
		in[0] = _mm_loadu_ps(&src1[i]),
		in[1] = _mm_loadu_ps(&src2[i]),
		in[0] = _mm_add_ps(in[0], in[1]);
		_mm_storeu_ps(&dst[i], in[0]);
	}
	for (; n_samples--; i++) {
		in[0] = _mm_load_ss(&src1[i]),
		in[1] = _mm_load_ss(&src2[i]),
		in[0] = _mm_add_ss(in[0], in[1]);
		_mm_store_ss(&dst[i], in[0]);
	}
}
#else
static void mix_2(float *dst, float *src1, float *src2, int n_samples)
{
	int i;
	for (i = 0; i < n_samples; i++)
		dst[i] = src1[i] + src2[i];
}
#endif


static int impl_node_process(struct spa_node *node)
{
	struct impl *this;
	struct port *outport;
	struct spa_io_buffers *outio;
	int n_samples, n_buffers, i, maxsize;
        struct buffer **buffers;
        struct buffer *outb;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	outport = GET_OUT_PORT(this, 0);
	outio = outport->io;
	spa_return_val_if_fail(outio != NULL, -EIO);

	spa_log_trace(this->log, NAME " %p: status %d %d", this, outio->status, outio->buffer_id);

	if (outio->status == SPA_STATUS_HAVE_BUFFER)
		return outio->status;

	/* recycle */
	if (outio->buffer_id < outport->n_buffers) {
		queue_buffer(this, outport, &outport->buffers[outio->buffer_id]);
		outio->buffer_id = SPA_ID_INVALID;
	}

        buffers = alloca(MAX_PORTS * sizeof(struct buffer *));
        n_buffers = 0;

	maxsize = MAX_SAMPLES * sizeof(float);
	if (outport->ctrl)
		maxsize = SPA_MIN(outport->ctrl->max_size, maxsize);

	for (i = 0; i < this->last_port; i++) {
		struct port *inport = GET_IN_PORT(this, i);
		struct spa_io_buffers *inio = NULL;
		struct buffer *inb;

		if (!inport->valid ||
		    (inio = inport->io) == NULL ||
		    inio->buffer_id >= inport->n_buffers ||
		    inio->status != SPA_STATUS_HAVE_BUFFER) {
			spa_log_trace(this->log, NAME " %p: skip input %d %d %p %d %d %d", this,
				i, inport->valid, inio,
				inio ? inio->status : -1,
				inio ? inio->buffer_id : -1,
				inport->n_buffers);
			continue;
		}

		spa_log_trace(this->log, NAME " %p: mix input %d %p->%p %d %d", this,
				i, inio, outio, inio->status, inio->buffer_id);

		inb = &inport->buffers[inio->buffer_id];

		maxsize = SPA_MIN(inb->buffer->datas[0].chunk->size, maxsize);

		buffers[n_buffers++] = inb;
		inio->status = SPA_STATUS_NEED_BUFFER;
	}

	outb = dequeue_buffer(this, outport);
        if (outb == NULL) {
                spa_log_trace(this->log, NAME " %p: out of buffers", this);
                return -EPIPE;
        }

	n_samples = maxsize / sizeof(float);

	if (n_buffers == 1) {
		*outb->buffer = *buffers[0]->buffer;
		outb->buffer->id = outb->id;
	}
	else {
		float *dst;

		outb->buffer->id = outb->id;
		outb->buffer->n_datas = 1;
		outb->buffer->datas = outb->datas;
		outb->datas[0].data = this->empty;
		outb->datas[0].chunk = outb->chunk;
		outb->datas[0].chunk->offset = 0;
		outb->datas[0].chunk->size = n_samples * sizeof(float);
		outb->datas[0].chunk->stride = sizeof(float);

		dst = this->empty;
		if (n_buffers == 0) {
			memset(dst, 0, n_samples * sizeof(float));
		}
		else {
			/* first 2 buffers, add and store */
			mix_2(dst, buffers[0]->buffer->datas[0].data,
				   buffers[1]->buffer->datas[0].data, n_samples);
			/* next buffers */
			for (i = 2; i < n_buffers; i++)
				mix_2(dst, dst, buffers[i]->buffer->datas[0].data, n_samples);
		}
	}

	outio->buffer_id = outb->buffer->id;
	outio->status = SPA_STATUS_HAVE_BUFFER;

	return SPA_STATUS_HAVE_BUFFER | SPA_STATUS_NEED_BUFFER;
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
	struct port *port;
	uint32_t i;

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

	port = GET_OUT_PORT(this, 0);
	port->valid = true;
	port->id = 0;
	port->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
	    SPA_PORT_INFO_FLAG_NO_REF;
	spa_list_init(&port->queue);

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

const struct spa_handle_factory spa_floatmix_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
