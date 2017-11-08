/* Spa
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

#include <string.h>
#include <stddef.h>

#include <spa/log.h>
#include <spa/type-map.h>
#include <spa/node.h>
#include <spa/list.h>
#include <spa/audio/format-utils.h>
#include <spa/param-alloc.h>
#include <lib/props.h>
#include <lib/format.h>

#define NAME "volume"

#define MAX_BUFFERS     16

struct props {
	double volume;
	bool mute;
};

struct buffer {
	struct spa_buffer *outbuf;
	bool outstanding;
	struct spa_meta_header *h;
	void *ptr;
	size_t size;
	struct spa_list link;
};

struct port {
	bool have_format;

	struct spa_port_info info;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;
	struct spa_port_io *io;

	struct spa_list empty;
};

struct type {
	uint32_t node;
	uint32_t format;
	uint32_t props;
	uint32_t prop_volume;
	uint32_t prop_mute;
	struct spa_type_param param;
	struct spa_type_meta meta;
	struct spa_type_data data;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_audio format_audio;
	struct spa_type_audio_format audio_format;
	struct spa_type_event_node event_node;
	struct spa_type_command_node command_node;
	struct spa_type_param_alloc_buffers param_alloc_buffers;
	struct spa_type_param_alloc_meta_enable param_alloc_meta_enable;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->node = spa_type_map_get_id(map, SPA_TYPE__Node);
	type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
	type->props = spa_type_map_get_id(map, SPA_TYPE__Props);
	type->prop_volume = spa_type_map_get_id(map, SPA_TYPE_PROPS__volume);
	type->prop_mute = spa_type_map_get_id(map, SPA_TYPE_PROPS__mute);
	spa_type_param_map(map, &type->param);
	spa_type_meta_map(map, &type->meta);
	spa_type_data_map(map, &type->data);
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_audio_map(map, &type->format_audio);
	spa_type_audio_format_map(map, &type->audio_format);
	spa_type_event_node_map(map, &type->event_node);
	spa_type_command_node_map(map, &type->command_node);
	spa_type_param_alloc_buffers_map(map, &type->param_alloc_buffers);
	spa_type_param_alloc_meta_enable_map(map, &type->param_alloc_meta_enable);
}

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct type type;
	struct spa_type_map *map;
	struct spa_log *log;

	struct props props;

	const struct spa_node_callbacks *callbacks;
	void *callbacks_data;

	struct spa_audio_info current_format;
	int bpf;

	struct port in_ports[1];
	struct port out_ports[1];

	bool started;
};

#define CHECK_IN_PORT(this,d,p)  ((d) == SPA_DIRECTION_INPUT && (p) == 0)
#define CHECK_OUT_PORT(this,d,p) ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)
#define CHECK_PORT(this,d,p)     ((p) == 0)
#define GET_IN_PORT(this,p)	 (&this->in_ports[p])
#define GET_OUT_PORT(this,p)	 (&this->out_ports[p])
#define GET_PORT(this,d,p)	 (d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,p) : GET_OUT_PORT(this,p))

#define DEFAULT_VOLUME 1.0
#define DEFAULT_MUTE false

static void reset_props(struct props *props)
{
	props->volume = DEFAULT_VOLUME;
	props->mute = DEFAULT_MUTE;
}

static int impl_node_enum_params(struct spa_node *node,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod_object *filter,
				 struct spa_pod_builder *builder)
{
	struct impl *this;
	struct type *t;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(index != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(builder != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	if (id == t->param.idList) {
		if (*index > 0)
			return SPA_RESULT_ENUM_END;

		spa_pod_builder_object(builder,
			id, t->param.List,
			":", t->param.listId,   "I",  t->param.idProps);
	}
	else if (id == t->param.idProps) {
		struct props *p = &this->props;

		if(*index > 0)
			return SPA_RESULT_ENUM_END;

		spa_pod_builder_object(builder,
			id, t->props,
			":", t->prop_volume, "dr", p->volume, 2, 0.0, 10.0,
			":", t->prop_mute,   "b",  p->mute);
	}
	else
		return SPA_RESULT_UNKNOWN_PARAM;

	(*index)++;

	return SPA_RESULT_OK;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
			       const struct spa_pod_object *param)
{
	struct impl *this;
	struct type *t;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	if (id == t->param.idProps) {
		struct props *p = &this->props;

		if (param == NULL) {
			reset_props(p);
			return SPA_RESULT_OK;
		}
		spa_pod_object_parse(param,
			":", t->prop_volume, "?d", &p->volume,
			":", t->prop_mute,   "?b", &p->mute, NULL);
	}
	else
		return SPA_RESULT_UNKNOWN_PARAM;

	return SPA_RESULT_OK;
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(command != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (SPA_COMMAND_TYPE(command) == this->type.command_node.Start) {
		this->started = true;
	} else if (SPA_COMMAND_TYPE(command) == this->type.command_node.Pause) {
		this->started = false;
	} else
		return SPA_RESULT_NOT_IMPLEMENTED;

	return SPA_RESULT_OK;
}

static int
impl_node_set_callbacks(struct spa_node *node,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	this->callbacks = callbacks;
	this->callbacks_data = data;

	return SPA_RESULT_OK;
}

static int
impl_node_get_n_ports(struct spa_node *node,
		      uint32_t *n_input_ports,
		      uint32_t *max_input_ports,
		      uint32_t *n_output_ports,
		      uint32_t *max_output_ports)
{
	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	if (n_input_ports)
		*n_input_ports = 1;
	if (max_input_ports)
		*max_input_ports = 1;
	if (n_output_ports)
		*n_output_ports = 1;
	if (max_output_ports)
		*max_output_ports = 1;

	return SPA_RESULT_OK;
}

static int
impl_node_get_port_ids(struct spa_node *node,
		       uint32_t n_input_ports,
		       uint32_t *input_ids,
		       uint32_t n_output_ports,
		       uint32_t *output_ids)
{
	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	if (n_input_ports > 0 && input_ids)
		input_ids[0] = 0;
	if (n_output_ports > 0 && output_ids)
		output_ids[0] = 0;

	return SPA_RESULT_OK;
}


static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_port_get_info(struct spa_node *node,
			enum spa_direction direction,
			uint32_t port_id,
			const struct spa_port_info **info)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	port = GET_PORT(this, direction, port_id);
	*info = &port->info;

	return SPA_RESULT_OK;
}

static int port_enum_formats(struct spa_node *node,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t *index,
			     const struct spa_pod_object *filter,
			     struct spa_pod_builder *builder)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	int res;
	struct spa_pod_object *fmt;
	uint8_t buffer[1024];
	struct spa_pod_builder b = { NULL, };
	struct type *t = &this->type;

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (*index) {
	case 0:
		fmt = spa_pod_builder_object(&b,
			t->param.idEnumFormat, t->format,
			"I", t->media_type.audio,
			"I", t->media_subtype.raw,
			":", t->format_audio.format,  "Ieu", t->audio_format.S16,
									2, t->audio_format.S16,
								           t->audio_format.S32,
			":", t->format_audio.rate,    "iru", 44100,	2, 1, INT32_MAX,
			":", t->format_audio.channels,"iru", 2,		2, 1, INT32_MAX);
		break;
	default:
		return SPA_RESULT_ENUM_END;
	}
	(*index)++;

	if ((res = spa_pod_object_filter(fmt, filter, builder)) != SPA_RESULT_OK)
		goto next;

	return SPA_RESULT_OK;
}

static int port_get_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t *index,
			   const struct spa_pod_object *filter,
			   struct spa_pod_builder *builder)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *port;
	struct type *t = &this->type;

	port = GET_PORT(this, direction, port_id);

	if (!port->have_format)
		return SPA_RESULT_NO_FORMAT;
	if (*index > 0)
		return SPA_RESULT_ENUM_END;

	spa_pod_builder_object(builder,
			t->param.idFormat, t->format,
	                "I", t->media_type.audio,
			"I", t->media_subtype.raw,
			":", t->format_audio.format,   "I", this->current_format.info.raw.format,
			":", t->format_audio.rate,     "i", this->current_format.info.raw.rate,
			":", t->format_audio.channels, "i", this->current_format.info.raw.channels);

	return SPA_RESULT_OK;
}

static int
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t *index,
			   const struct spa_pod_object *filter,
			   struct spa_pod_builder *builder)
{
	struct impl *this;
	struct type *t;
	struct port *port;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(index != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(builder != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	port = GET_PORT(this, direction, port_id);

	if (id == t->param.idList) {
		uint32_t list[] = { t->param.idEnumFormat,
				    t->param.idFormat,
				    t->param.idBuffers,
				    t->param.idMeta };

		if (*index < SPA_N_ELEMENTS(list))
			spa_pod_builder_object(builder, id, t->param.List,
				":", t->param.listId, "I", list[*index]);
		else
			return SPA_RESULT_ENUM_END;
	}
	else if (id == t->param.idEnumFormat) {
		return port_enum_formats(node, direction, port_id, index, filter, builder);
	}
	else if (id == t->param.idFormat) {
		return port_get_format(node, direction, port_id, index, filter, builder);
	}
	else if (id == t->param.idBuffers) {
		if (!port->have_format)
			return SPA_RESULT_NO_FORMAT;
		if (*index > 0)
			return SPA_RESULT_ENUM_END;

		spa_pod_builder_object(builder,
			id, t->param_alloc_buffers.Buffers,
			":", t->param_alloc_buffers.size,    "iru", 1024 * this->bpf,
									2, 16 * this->bpf,
									   INT32_MAX / this->bpf,
			":", t->param_alloc_buffers.stride,  "i", 0,
			":", t->param_alloc_buffers.buffers, "iru", 2,
									2, 1, MAX_BUFFERS,
			":", t->param_alloc_buffers.align,   "i", 16);
	}
	else if (id == t->param.idMeta) {
		switch (*index) {
		case 0:
			spa_pod_builder_object(builder,
				id, t->param_alloc_meta_enable.MetaEnable,
				":", t->param_alloc_meta_enable.type, "I", t->meta.Header,
				":", t->param_alloc_meta_enable.size, "i", sizeof(struct spa_meta_header));
			break;
		default:
			return SPA_RESULT_ENUM_END;
		}
	}
	else
		return SPA_RESULT_UNKNOWN_PARAM;

	(*index)++;

	return SPA_RESULT_OK;
}

static int clear_buffers(struct impl *this, struct port *port)
{
	if (port->n_buffers > 0) {
		spa_log_info(this->log, NAME " %p: clear buffers", this);
		port->n_buffers = 0;
		spa_list_init(&port->empty);
	}
	return SPA_RESULT_OK;
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod_object *format)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *port;

	port = GET_PORT(this, direction, port_id);

	if (format == NULL) {
		port->have_format = false;
		clear_buffers(this, port);
	} else {
		struct spa_audio_info info = { 0 };

		spa_pod_object_parse(format,
			"I", &info.media_type,
			"I", &info.media_subtype);

		if (info.media_type != this->type.media_type.audio ||
		    info.media_subtype != this->type.media_subtype.raw)
			return SPA_RESULT_INVALID_MEDIA_TYPE;

		if (spa_format_audio_raw_parse(format, &info.info.raw, &this->type.format_audio) < 0)
			return SPA_RESULT_INVALID_MEDIA_TYPE;

		this->bpf = 2 * info.info.raw.channels;
		this->current_format = info;
		port->have_format = true;
	}

	return SPA_RESULT_OK;
}

static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod_object *param)
{
	struct impl *this;
	struct type *t;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	if (id == t->param.idFormat) {
		return port_set_format(node, direction, port_id, flags, param);
	}
	else
		return SPA_RESULT_UNKNOWN_PARAM;
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

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	port = GET_PORT(this, direction, port_id);

	if (!port->have_format)
		return SPA_RESULT_NO_FORMAT;

	clear_buffers(this, port);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

		b = &port->buffers[i];
		b->outbuf = buffers[i];
		b->outstanding = direction == SPA_DIRECTION_INPUT;
		b->h = spa_buffer_find_meta(buffers[i], this->type.meta.Header);

		if ((d[0].type == this->type.data.MemPtr ||
		     d[0].type == this->type.data.MemFd ||
		     d[0].type == this->type.data.DmaBuf) && d[0].data != NULL) {
			b->ptr = d[0].data;
			b->size = d[0].maxsize;
		} else {
			spa_log_error(this->log, NAME " %p: invalid memory on buffer %p", this,
				      buffers[i]);
			return SPA_RESULT_ERROR;
		}
		if (!b->outstanding)
			spa_list_append(&port->empty, &b->link);
	}
	port->n_buffers = n_buffers;

	return SPA_RESULT_OK;
}

static int
impl_node_port_alloc_buffers(struct spa_node *node,
			     enum spa_direction direction,
			     uint32_t port_id,
			     struct spa_pod_object **params,
			     uint32_t n_params,
			     struct spa_buffer **buffers,
			     uint32_t *n_buffers)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_port_set_io(struct spa_node *node,
		      enum spa_direction direction,
		      uint32_t port_id,
		      struct spa_port_io *io)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	port = GET_PORT(this, direction, port_id);
	port->io = io;

	return SPA_RESULT_OK;
}

static void recycle_buffer(struct impl *this, uint32_t id)
{
	struct port *port = GET_OUT_PORT(this, 0);
	struct buffer *b = &port->buffers[id];

	if (!b->outstanding) {
		spa_log_warn(this->log, NAME " %p: buffer %d not outstanding", this, id);
		return;
	}

	spa_list_append(&port->empty, &b->link);
	b->outstanding = false;
	spa_log_trace(this->log, NAME " %p: recycle buffer %d", this, id);
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id),
			       SPA_RESULT_INVALID_PORT);

	port = GET_OUT_PORT(this, port_id);

	if (port->n_buffers == 0)
		return SPA_RESULT_NO_BUFFERS;

	if (buffer_id >= port->n_buffers)
		return SPA_RESULT_INVALID_BUFFER_ID;

	recycle_buffer(this, buffer_id);

	return SPA_RESULT_OK;
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    const struct spa_command *command)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static struct spa_buffer *find_free_buffer(struct impl *this, struct port *port)
{
	struct buffer *b;

	if (spa_list_is_empty(&port->empty))
		return NULL;

	b = spa_list_first(&port->empty, struct buffer, link);
	spa_list_remove(&b->link);
	b->outstanding = true;

	return b->outbuf;
}

static void do_volume(struct impl *this, struct spa_buffer *dbuf, struct spa_buffer *sbuf)
{
	uint32_t si, di, i, n_samples, n_bytes, soff, doff;
	struct spa_data *sd, *dd;
	int16_t *src, *dst;
	double volume;

	volume = this->props.volume;

	si = di = 0;
	soff = doff = 0;

	while (true) {
		if (si == sbuf->n_datas || di == dbuf->n_datas)
			break;

		sd = &sbuf->datas[si];
		dd = &dbuf->datas[di];

		src = (int16_t *) ((uint8_t *) sd->data + sd->chunk->offset + soff);
		dst = (int16_t *) ((uint8_t *) dd->data + doff);

		n_bytes = SPA_MIN(sd->chunk->size - soff, dd->maxsize - doff);
		n_samples = n_bytes / sizeof(uint16_t);

		for (i = 0; i < n_samples; i++)
			dst[i] = src[i] * volume;

		soff += n_bytes;
		doff += n_bytes;

		dd->chunk->offset = 0;
		dd->chunk->size = doff;

		if (soff >= sd->chunk->size) {
			si++;
			soff = 0;
		}
		if (doff >= dd->maxsize) {
			di++;
			doff = 0;
		}
	}
}

static int impl_node_process_input(struct spa_node *node)
{
	struct impl *this;
	struct spa_port_io *input;
	struct spa_port_io *output;
	struct port *in_port, *out_port;
	struct spa_buffer *dbuf, *sbuf;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	out_port = GET_OUT_PORT(this, 0);
	output = out_port->io;
	spa_return_val_if_fail(output != NULL, SPA_RESULT_ERROR);

	if (output->status == SPA_RESULT_HAVE_BUFFER)
		return SPA_RESULT_HAVE_BUFFER;

	in_port = GET_IN_PORT(this, 0);
	input = in_port->io;
	spa_return_val_if_fail(input != NULL, SPA_RESULT_ERROR);

	if (input->buffer_id >= in_port->n_buffers)
		return SPA_RESULT_NEED_BUFFER;

	if ((dbuf = find_free_buffer(this, out_port)) == NULL) {
                spa_log_error(this->log, NAME " %p: out of buffers", this);
		return SPA_RESULT_OUT_OF_BUFFERS;
	}

	sbuf = in_port->buffers[input->buffer_id].outbuf;

	input->status = SPA_RESULT_OK;

	spa_log_trace(this->log, NAME " %p: do volume %d -> %d", this, sbuf->id, dbuf->id);
	do_volume(this, dbuf, sbuf);

	output->buffer_id = dbuf->id;
	output->status = SPA_RESULT_HAVE_BUFFER;

	return SPA_RESULT_HAVE_BUFFER;
}

static int impl_node_process_output(struct spa_node *node)
{
	struct impl *this;
	struct port *in_port, *out_port;
	struct spa_port_io *input, *output;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	out_port = GET_OUT_PORT(this, 0);
	output = out_port->io;
	spa_return_val_if_fail(output != NULL, SPA_RESULT_ERROR);

	if (output->status == SPA_RESULT_HAVE_BUFFER)
		return SPA_RESULT_HAVE_BUFFER;

	/* recycle */
	if (output->buffer_id < out_port->n_buffers) {
		recycle_buffer(this, output->buffer_id);
		output->buffer_id = SPA_ID_INVALID;
	}

	in_port = GET_IN_PORT(this, 0);
	input = in_port->io;
	spa_return_val_if_fail(input != NULL, SPA_RESULT_ERROR);

	input->range = output->range;
	input->status = SPA_RESULT_NEED_BUFFER;

	return SPA_RESULT_NEED_BUFFER;
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
	impl_node_process_input,
	impl_node_process_output,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t interface_id, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(interface != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = (struct impl *) handle;

	if (interface_id == this->type.node)
		*interface = &this->node;
	else
		return SPA_RESULT_UNKNOWN_INTERFACE;

	return SPA_RESULT_OK;
}

static int impl_clear(struct spa_handle *handle)
{
	return SPA_RESULT_OK;
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

	spa_return_val_if_fail(factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

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
		spa_log_error(this->log, "a type-map is needed");
		return SPA_RESULT_ERROR;
	}
	init_type(&this->type, this->map);

	this->node = impl_node;
	reset_props(&this->props);

	this->in_ports[0].info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
	    SPA_PORT_INFO_FLAG_IN_PLACE;
	spa_list_init(&this->in_ports[0].empty);

	this->out_ports[0].info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
	    SPA_PORT_INFO_FLAG_NO_REF;
	spa_list_init(&this->out_ports[0].empty);

	return SPA_RESULT_OK;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE__Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t index)
{
	spa_return_val_if_fail(factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	switch (index) {
	case 0:
		*info = &impl_interfaces[index];
		break;
	default:
		return SPA_RESULT_ENUM_END;
	}
	return SPA_RESULT_OK;
}

const struct spa_handle_factory spa_volume_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	sizeof(struct impl),
	impl_init,
	impl_enum_interface_info,
};
