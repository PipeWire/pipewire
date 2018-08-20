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
#include <spa/pod/filter.h>

#define NAME "fmtconvert"

#define DEFAULT_RATE		44100
#define DEFAULT_CHANNELS	2

#define MAX_BUFFERS     64
#define MAX_PORTS	128

#define PROP_DEFAULT_TRUNCATE	false
#define PROP_DEFAULT_DITHER	0

struct impl;

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

struct format {
	struct spa_audio_info format;
	uint32_t stride;
	uint32_t blocks;
	uint32_t size;
};

struct port {
	uint32_t id;
	bool valid;

	struct spa_io_buffers *io;
	struct spa_io_control_range *ctrl;

	struct spa_port_info info;
	struct spa_dict info_props;
	struct spa_dict_item info_props_items[2];

	bool have_format;
	struct spa_audio_info format;
	uint32_t stride;
	uint32_t blocks;
	uint32_t size;

	uint32_t offset;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_list queue;
};

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


#include "fmt-ops.c"

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct type type;
	struct spa_type_map *map;
	struct spa_log *log;

	struct props props;

	const struct spa_node_callbacks *callbacks;
	void *user_data;

	struct port ports[2][MAX_PORTS];
	uint32_t n_ports[2];

	struct format formats[2];
	uint32_t n_formats[2];

	bool started;

	convert_func_t convert;

	float empty[4096];
};

#define CHECK_FREE_PORT(this,d,id)	(id < MAX_PORTS && !GET_PORT(this,d,id)->valid)
#define CHECK_PORT(this,d,id)		(id < MAX_PORTS && GET_PORT(this,d,id)->valid)
#define GET_PORT(this,d,id)		(&this->ports[d][id])
#define GET_IN_PORT(this,id)		GET_PORT(this,SPA_DIRECTION_INPUT,id)
#define GET_OUT_PORT(this,id)		GET_PORT(this,SPA_DIRECTION_OUTPUT,id)

static int collect_format(struct impl *this, enum spa_direction direction, struct format *fmt)
{
	int i, idx, ch = 0;

	*fmt = this->formats[direction];
	for (i = 0, idx = 0; idx < this->n_ports[direction] && i < MAX_PORTS; i++) {
		struct port *p = GET_PORT(this, direction, i);
		if (!p->valid)
			continue;
		idx++;
		if (!p->have_format)
			return -1;
		fmt->format = p->format;
		ch += p->format.info.raw.channels;
	}
	fmt->format.info.raw.channels = ch;

	return 0;
}

static int setup_convert(struct impl *this)
{
	uint32_t src_fmt, dst_fmt;
	struct type *t = &this->type;
	struct format informat, outformat;
	const struct conv_info *conv;

	if (collect_format(this, SPA_DIRECTION_INPUT, &informat) < 0)
		return -1;
	if (collect_format(this, SPA_DIRECTION_OUTPUT, &outformat) < 0)
		return -1;

	src_fmt = informat.format.info.raw.format;
	dst_fmt = outformat.format.info.raw.format;

	spa_log_info(this->log, NAME " %p: %s/%d@%d.%d->%s/%d@%d.%d", this,
			spa_type_map_get_type(this->map, src_fmt),
			informat.format.info.raw.channels,
			informat.format.info.raw.rate,
			informat.format.info.raw.layout,
			spa_type_map_get_type(this->map, dst_fmt),
			outformat.format.info.raw.channels,
			outformat.format.info.raw.rate,
			outformat.format.info.raw.layout);

	if (informat.format.info.raw.channels != outformat.format.info.raw.channels)
		return -EINVAL;

	if (informat.format.info.raw.rate != outformat.format.info.raw.rate)
		return -EINVAL;

	/* find fast path */
	conv = find_conv_info(&t->audio_format, src_fmt, dst_fmt, FEATURE_SSE);
	if (conv != NULL) {
		spa_log_info(this->log, NAME " %p: got converter features %08x", this,
				conv->features);
		if (informat.format.info.raw.layout == SPA_AUDIO_LAYOUT_INTERLEAVED) {
			if (outformat.format.info.raw.layout == SPA_AUDIO_LAYOUT_INTERLEAVED)
				this->convert = conv->i2i;
			else
				this->convert = conv->i2d;
		}
		else {
			if (outformat.format.info.raw.layout == SPA_AUDIO_LAYOUT_INTERLEAVED)
				this->convert = conv->d2i;
			else
				this->convert = conv->i2i;
		}
		return 0;
	}
	return -ENOTSUP;
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
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (n_input_ports)
		*n_input_ports = this->n_ports[SPA_DIRECTION_INPUT];
	if (max_input_ports)
		*max_input_ports = MAX_PORTS;
	if (n_output_ports)
		*n_output_ports = this->n_ports[SPA_DIRECTION_OUTPUT];
	if (max_output_ports)
		*max_output_ports = MAX_PORTS;

	return 0;
}

static inline struct port *iterate_ports(struct impl *this, enum spa_direction direction, uint32_t max, uint32_t *state)
{
	for (; (*state & 0xffff) < MAX_PORTS && (*state >> 16) < max; (*state)++) {
		struct port *p = &this->ports[direction][(*state) & 0xffff];
		if (p->valid) {
			(*state) += 0x10001;
			return p;
		}
	}
	return NULL;
}

static int collect_ports(struct impl *this, enum spa_direction direction, uint32_t *ids, uint32_t n_ids)
{
	int i, idx;
	for (i = 0, idx = 0; i < MAX_PORTS && idx < n_ids; i++) {
		if (this->ports[direction][i].valid)
			ids[idx++] = i;
	}
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

	if (input_ids)
		collect_ports(this, SPA_DIRECTION_INPUT, input_ids, n_input_ids);
	if (output_ids)
		collect_ports(this, SPA_DIRECTION_OUTPUT, output_ids, n_output_ids);

	return 0;
}

static int init_port(struct impl *this, enum spa_direction direction, uint32_t port_id, uint32_t flags)
{
        struct port *port;

        port = GET_PORT(this, direction, port_id);
        port->valid = true;
        port->id = port_id;

        spa_list_init(&port->queue);
        port->info.flags = flags;

	port->info_props_items[0] = SPA_DICT_ITEM_INIT("port.dsp", "32 bit float mono audio");
	port->info_props = SPA_DICT_INIT(port->info_props_items, 1);
	port->info.props = &port->info_props;

        this->n_ports[direction]++;
        port->have_format = false;

	return 0;
}

static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
        struct impl *this;

        spa_return_val_if_fail(node != NULL, -EINVAL);

        this = SPA_CONTAINER_OF(node, struct impl, node);

        spa_return_val_if_fail(CHECK_FREE_PORT(this, direction, port_id), -EINVAL);

	init_port(this, direction, port_id,
			SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
			SPA_PORT_INFO_FLAG_REMOVABLE);

        spa_log_debug(this->log, NAME " %p: add port %d", this, port_id);

        return 0;
}

static int
impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	struct impl *this;
        struct port *port;

        spa_return_val_if_fail(node != NULL, -EINVAL);

        this = SPA_CONTAINER_OF(node, struct impl, node);

        spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

        port = GET_PORT (this, direction, port_id);

        this->n_ports[direction]--;
        if (port->have_format)
		this->n_formats[direction]--;
        spa_memzero(port, sizeof(struct port));

        spa_log_debug(this->log, NAME " %p: remove port %d", this, port_id);

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
	struct spa_audio_info *other;

	other = &this->formats[SPA_DIRECTION_REVERSE(direction)].format;

	switch (*index) {
	case 0:
		if (other->info.raw.channels > 0) {
			*param = spa_pod_builder_object(builder,
				t->param.idEnumFormat, t->format,
				"I", t->media_type.audio,
				"I", t->media_subtype.raw,
				":", t->format_audio.format,   "Ieu", other->info.raw.format,
					SPA_POD_PROP_ENUM(3, other->info.raw.format,
							     t->audio_format.F32,
							     t->audio_format.F32_OE),
				":", t->format_audio.layout,   "ieu", other->info.raw.layout,
					SPA_POD_PROP_ENUM(2, SPA_AUDIO_LAYOUT_INTERLEAVED,
							     SPA_AUDIO_LAYOUT_NON_INTERLEAVED),
				":", t->format_audio.rate,     "i", other->info.raw.rate,
				":", t->format_audio.channels, "i", other->info.raw.channels);
		} else {
			*param = spa_pod_builder_object(builder,
				t->param.idEnumFormat, t->format,
				"I", t->media_type.audio,
				"I", t->media_subtype.raw,
				":", t->format_audio.format,   "Ieu", t->audio_format.S16,
					SPA_POD_PROP_ENUM(11, t->audio_format.U8,
							      t->audio_format.S16,
							      t->audio_format.S16_OE,
							      t->audio_format.F32,
							      t->audio_format.F32_OE,
							      t->audio_format.S32,
							      t->audio_format.S32_OE,
							      t->audio_format.S24,
							      t->audio_format.S24_OE,
							      t->audio_format.S24_32,
							      t->audio_format.S24_32_OE),
				":", t->format_audio.layout,   "ieu", SPA_AUDIO_LAYOUT_INTERLEAVED,
					SPA_POD_PROP_ENUM(2, SPA_AUDIO_LAYOUT_INTERLEAVED,
							     SPA_AUDIO_LAYOUT_NON_INTERLEAVED),
				":", t->format_audio.rate,     "iru", DEFAULT_RATE,
					SPA_POD_PROP_MIN_MAX(1, INT32_MAX),
				":", t->format_audio.channels, "iru", DEFAULT_CHANNELS,
					SPA_POD_PROP_MIN_MAX(1, INT32_MAX));
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
		":", t->format_audio.format,   "I", port->format.info.raw.format,
		":", t->format_audio.layout,   "i", port->format.info.raw.layout,
		":", t->format_audio.rate,     "i", port->format.info.raw.rate,
		":", t->format_audio.channels, "i", port->format.info.raw.channels);

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
	struct port *port, *other;

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
	other = GET_PORT(this, SPA_DIRECTION_REVERSE(direction), port_id);

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (id == t->param.idList) {
		uint32_t list[] = { t->param.idEnumFormat,
				    t->param.idFormat,
				    t->param.idBuffers,
				    t->param.idMeta,
				    t->param_io.idBuffers };

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
		uint32_t buffers, size;
		const char *size_fmt;

		if (!port->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		if (other->n_buffers > 0) {
			buffers = other->n_buffers;
			size = other->size / other->stride;
			size_fmt = "ir";
		} else {
			buffers = 1;
			size = 1024;
			size_fmt = "iru";
		}

		param = spa_pod_builder_object(&b,
			id, t->param_buffers.Buffers,
			":", t->param_buffers.buffers, "iru", buffers,
				SPA_POD_PROP_MIN_MAX(2, MAX_BUFFERS),
			":", t->param_buffers.blocks,  "i", port->blocks,
			":", t->param_buffers.size,    size_fmt, size * port->stride,
				SPA_POD_PROP_MIN_MAX(16 * port->stride, INT32_MAX / port->stride),
			":", t->param_buffers.stride,  "i", port->stride,
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
	else
		return -ENOENT;

	(*index)++;

	if (spa_pod_filter(builder, result, param, filter) < 0)
		goto next;

	return 1;
}

static int calc_width(struct spa_audio_info *info, struct type *t)
{
	if (info->info.raw.format == t->audio_format.U8)
		return 1;
	else if (info->info.raw.format == t->audio_format.S16 ||
	    info->info.raw.format == t->audio_format.S16_OE)
		return 2;
	else if (info->info.raw.format == t->audio_format.S24 ||
	    info->info.raw.format == t->audio_format.S24_OE)
		return 3;
	else
		return 4;
}

static int clear_buffers(struct impl *this, struct port *port)
{
	if (port->n_buffers > 0) {
		spa_log_debug(this->log, NAME " %p: clear buffers %p", this, port);
		port->n_buffers = 0;
		spa_list_init(&port->queue);
	}
	return 0;
}
static int compatible_format(struct spa_audio_info *info, struct spa_audio_info *info2)
{
	if (info->info.raw.format != info2->info.raw.format ||
	    info->info.raw.layout != info2->info.raw.layout ||
	    info->info.raw.rate != info2->info.raw.rate)
		return -EINVAL;
	return 0;
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
	int res = 0;

	port = GET_PORT(this, direction, port_id);

	if (format == NULL) {
		if (port->have_format) {
			port->have_format = false;
			this->n_formats[direction]--;
			this->formats[direction].format.info.raw.channels -= port->format.info.raw.channels;
			clear_buffers(this, port);
			this->convert = NULL;
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

		if (this->n_formats[direction] > 0) {
			if (compatible_format(&info, &this->formats[direction].format) < 0)
				return -EINVAL;
			this->formats[direction].format.info.raw.channels += info.info.raw.channels;
		}
		else {
			this->formats[direction].format = info;
		}
		this->n_formats[direction]++;

		port->have_format = true;
		port->format = info;

		port->stride = calc_width(&info, t);

		if (info.info.raw.layout == SPA_AUDIO_LAYOUT_INTERLEAVED) {
			port->stride *= info.info.raw.channels;
			port->blocks = 1;
		}
		else {
			port->blocks = info.info.raw.channels;
		}

		if (this->n_formats[SPA_DIRECTION_INPUT] == this->n_ports[SPA_DIRECTION_INPUT] &&
		    this->n_formats[SPA_DIRECTION_OUTPUT] == this->n_ports[SPA_DIRECTION_OUTPUT])
			res = setup_convert(this);

		spa_log_debug(this->log, NAME " %p: set format on port %d %d %d",
				this, port_id, res, port->stride);
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
	uint32_t i, size = SPA_ID_INVALID;
	struct type *t;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	spa_return_val_if_fail(port->have_format, -EIO);

	spa_log_debug(this->log, NAME " %p: use buffers %d on port %d", this, n_buffers, port_id);

	clear_buffers(this, port);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

		b = &port->buffers[i];
		b->flags = 0;
		b->outbuf = buffers[i];
		b->h = spa_buffer_find_meta_data(buffers[i], t->meta.Header, sizeof(*b->h));

		if (size == SPA_ID_INVALID)
			size = d[0].maxsize;
		else
			if (size != d[0].maxsize)
				return -EINVAL;

		if (!((d[0].type == t->data.MemPtr ||
		       d[0].type == t->data.MemFd ||
		       d[0].type == t->data.DmaBuf) && d[0].data != NULL)) {
			spa_log_error(this->log, NAME " %p: invalid memory on buffer %p", this,
				      buffers[i]);
			return -EINVAL;
		}
		if (direction == SPA_DIRECTION_OUTPUT)
			spa_list_append(&port->queue, &b->link);
		else
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
	}
	port->n_buffers = n_buffers;
	port->size = size;

	spa_log_debug(this->log, NAME " %p: buffer size %d", this, size);

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

	spa_log_debug(this->log, NAME " %p: port %d:%d update io %d %p",
			this, direction, port_id, id, data);

	if (id == t->io.Buffers)
		port->io = data;
	else if (id == t->io.ControlRange)
		port->ctrl = data;
	else
		return -ENOENT;

	return 0;
}

static void recycle_buffer(struct impl *this, struct port *port, uint32_t id)
{
	struct buffer *b = &port->buffers[id];

	if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_OUT)) {
		spa_list_append(&port->queue, &b->link);
		SPA_FLAG_UNSET(b->flags, BUFFER_FLAG_OUT);
		spa_log_trace(this->log, NAME " %p: recycle buffer %d", this, id);
	}
}

static inline struct buffer *peek_buffer(struct impl *this, struct port *port)
{
	if (spa_list_is_empty(&port->queue))
		return NULL;
	return spa_list_first(&port->queue, struct buffer, link);
}

static inline void dequeue_buffer(struct impl *this, struct buffer *b)
{
	spa_list_remove(&b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	port = GET_OUT_PORT(this, port_id);

	recycle_buffer(this, port, buffer_id);

	return 0;
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    const struct spa_command *command)
{
	return -ENOTSUP;
}

static int process_merge(struct impl *this)
{
	struct port *inport, *outport;
	struct spa_io_buffers *inio, *outio;
	struct buffer *inbuf, *outbuf;
	struct spa_buffer *inb, *outb;
	const void **src_datas;
	void **dst_datas;
	uint32_t n_src_datas, n_ins, n_dst_datas;
	int i, j, res = 0, n_bytes = 0, maxsize;
	uint32_t size = 0;

	outport = GET_OUT_PORT(this, 0);
	outio = outport->io;
	spa_return_val_if_fail(outio != NULL, -EIO);

	spa_log_trace(this->log, NAME " %p: status %p %d %d", this,
			outio, outio->status, outio->buffer_id);

	if (outio->status == SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_HAVE_BUFFER;

	if (outio->buffer_id < outport->n_buffers) {
		recycle_buffer(this, outport, outio->buffer_id);
		outio->buffer_id = SPA_ID_INVALID;
	}

	if ((outbuf = peek_buffer(this, outport)) == NULL)
		return outio->status = -EPIPE;

	outb = outbuf->outbuf;

	n_dst_datas = outb->n_datas;
	dst_datas = alloca(sizeof(void*) * n_dst_datas);

	maxsize = outb->datas[0].maxsize;
	if (outport->ctrl)
		maxsize = SPA_MIN(outport->ctrl->max_size, maxsize);

	n_ins = this->n_ports[SPA_DIRECTION_INPUT];
	src_datas = alloca(sizeof(void*) * MAX_PORTS);
	n_src_datas = 0;

	for (i = 0; i < n_ins; i++) {
		inport = GET_IN_PORT(this, i);

		if ((inio = inport->io) == NULL ||
		    inio->status != SPA_STATUS_HAVE_BUFFER ||
		    inio->buffer_id >= inport->n_buffers)
			continue;

		spa_log_trace(this->log, NAME " %p: %d %p %d %d %d", this, i,
				inio, inio->status, inio->buffer_id, inport->stride);

		inbuf = &inport->buffers[inio->buffer_id];
		inb = inbuf->outbuf;

		size = inb->datas[0].chunk->size;
		size = (size / inport->stride) * outport->stride;
		n_bytes = SPA_MIN(size - outport->offset, maxsize);

		for (j = 0; j < inb->n_datas; j++)
			src_datas[n_src_datas++] = inb->datas[j].data;

		inio->status = SPA_STATUS_NEED_BUFFER;
		res |= SPA_STATUS_NEED_BUFFER;
	}

	spa_log_trace(this->log, NAME " %p: %d %d %d %d %d %d", this,
			n_src_datas, n_dst_datas, n_bytes, outport->offset, size, outport->stride);

	if (n_src_datas > 0) {
		for (i = 0; i < n_dst_datas; i++) {
			dst_datas[i] = SPA_MEMBER(outb->datas[i].data, outport->offset, void);
			outb->datas[i].chunk->offset = 0;
			outb->datas[i].chunk->size = n_bytes;
		}

		this->convert(this, n_dst_datas, dst_datas, n_src_datas, src_datas, n_bytes);

		outport->offset += n_bytes;
		outport->offset = 0;

		dequeue_buffer(this, outbuf);
		outio->status = SPA_STATUS_HAVE_BUFFER;
		outio->buffer_id = outb->id;
		res |= SPA_STATUS_HAVE_BUFFER;
	}
	return res;
}

static int process_split(struct impl *this)
{
	struct port *inport, *outport;
	struct spa_io_buffers *inio, *outio;
	struct buffer *inbuf, *outbuf;
	struct spa_buffer *inb, *outb;
	const void **src_datas;
	void **dst_datas;
	uint32_t n_src_datas, n_outs, n_dst_datas;
	int i, j, res = 0, n_bytes = 0, maxsize;
	uint32_t size;

	inport = GET_IN_PORT(this, 0);
	inio = inport->io;
	spa_return_val_if_fail(inio != NULL, -EIO);

	spa_log_trace(this->log, NAME " %p: status %p %d %d", this,
			inio, inio->status, inio->buffer_id);

	if (inio->status != SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_NEED_BUFFER;
	if (inio->buffer_id >= inport->n_buffers)
		return inio->status = -EINVAL;

	inbuf = &inport->buffers[inio->buffer_id];
	inb = inbuf->outbuf;

	n_src_datas = inb->n_datas;
	src_datas = alloca(sizeof(void*) * n_src_datas);

	size = inb->datas[0].chunk->size;
	for (i = 0; i < n_src_datas; i++)
		src_datas[i] = SPA_MEMBER(inb->datas[i].data, inport->offset, void);

	n_outs = this->n_ports[SPA_DIRECTION_OUTPUT];
	dst_datas = alloca(sizeof(void*) * MAX_PORTS);
	n_dst_datas = 0;

	for (i = 0; i < n_outs; i++) {
		outport = GET_OUT_PORT(this, i);
		outio = outport->io;
		if (outio == NULL)
			goto empty;

		spa_log_trace(this->log, NAME " %p: %d %p %d %d %d", this, i,
				outio, outio->status, outio->buffer_id, outport->stride);

		if (outio->status == SPA_STATUS_HAVE_BUFFER) {
			res |= SPA_STATUS_HAVE_BUFFER;
			goto empty;
		}

		if (outio->buffer_id < outport->n_buffers) {
			recycle_buffer(this, outport, outio->buffer_id);
			outio->buffer_id = SPA_ID_INVALID;
		}

		if ((outbuf = peek_buffer(this, outport)) == NULL) {
			outio->status = -EPIPE;
          empty:
			dst_datas[n_dst_datas++] = this->empty;
			continue;
		}

		outb = outbuf->outbuf;

		maxsize = outb->datas[0].maxsize;
		if (outport->ctrl)
			maxsize = SPA_MIN(outport->ctrl->max_size, maxsize);
		maxsize = (maxsize / outport->stride) * inport->stride;
		n_bytes = SPA_MIN(size - inport->offset, maxsize);

		for (j = 0; j < outb->n_datas; j++) {
			dst_datas[n_dst_datas++] = outb->datas[j].data;
			outb->datas[j].chunk->offset = 0;
			outb->datas[j].chunk->size = (n_bytes / inport->stride) * outport->stride;
		}

		dequeue_buffer(this, outbuf);
		outio->status = SPA_STATUS_HAVE_BUFFER;
		outio->buffer_id = outb->id;
		res |= SPA_STATUS_HAVE_BUFFER;
	}

	spa_log_trace(this->log, NAME " %p: %d %d %d %d %d %d", this,
			n_src_datas, n_dst_datas, n_bytes, inport->offset, size, inport->stride);

	if (n_dst_datas > 0) {
		this->convert(this, n_dst_datas, dst_datas, n_src_datas, src_datas, n_bytes);

		inport->offset += n_bytes;
		if (inport->offset >= size) {
			inio->status = SPA_STATUS_NEED_BUFFER;
			inport->offset = 0;
			res |= SPA_STATUS_NEED_BUFFER;
		}
	}
	return res;
}

static int impl_node_process(struct spa_node *node)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (this->n_ports[SPA_DIRECTION_INPUT] > 1)
		return process_merge(this);
	else if (this->n_ports[SPA_DIRECTION_OUTPUT] >= 1)
		return process_split(this);
	else
		return -ENOTSUP;
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

	init_port(this, SPA_DIRECTION_OUTPUT, 0, SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS);
	init_port(this, SPA_DIRECTION_INPUT, 0, SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS);

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

const struct spa_handle_factory spa_fmtconvert_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
