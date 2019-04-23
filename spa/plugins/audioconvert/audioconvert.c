/* Spa
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
#include <string.h>
#include <stdio.h>

#include <spa/support/log.h>
#include <spa/utils/list.h>
#include <spa/node/node.h>
#include <spa/buffer/alloc.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/pod/filter.h>
#include <spa/debug/pod.h>
#include <spa/debug/types.h>

#define NAME "audioconvert"

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
	uint32_t out_flags;
	struct spa_node *in_node;
	uint32_t in_port;
	uint32_t in_flags;
	struct spa_io_buffers io;
	uint32_t min_buffers;
	uint32_t n_buffers;
	struct spa_buffer **buffers;
	unsigned int negotiated:1;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;

	struct spa_hook_list hooks;

	int n_links;
	struct link links[8];
	int n_nodes;
	struct spa_node *nodes[8];

#define MODE_SPLIT	0
#define MODE_MERGE	1
#define MODE_CONVERT	2
	int mode;
	bool started;

	struct spa_handle *hnd_fmt[2];
	struct spa_handle *hnd_channelmix;
	struct spa_handle *hnd_resample;

	struct spa_node *fmt[2];
	struct spa_node *channelmix;
	struct spa_node *resample;

	struct spa_hook listener[4];
	unsigned int listening:1;
};

static int make_link(struct impl *this,
		struct spa_node *out_node, uint32_t out_port,
		struct spa_node *in_node, uint32_t in_port, uint32_t min_buffers)
{
	struct link *l = &this->links[this->n_links++];

	l->out_node = out_node;
	l->out_port = out_port;
	l->out_flags = 0;
	l->in_node = in_node;
	l->in_port = in_port;
	l->in_flags = 0;
	l->negotiated = false;
	l->io.status = SPA_STATUS_NEED_BUFFER;
	l->io.buffer_id = SPA_ID_INVALID;
	l->n_buffers = 0;
	l->min_buffers = min_buffers;

	spa_node_port_set_io(out_node,
			     SPA_DIRECTION_OUTPUT, out_port,
			     SPA_IO_Buffers,
			     &l->io, sizeof(l->io));
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
		res = spa_node_port_enum_params_sync(node,
				       direction, port_id,
				       id, &state,
				       NULL, &param, &b);
		if (res != 1)
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
	if ((res = spa_node_port_enum_params_sync(link->out_node,
			       SPA_DIRECTION_OUTPUT, link->out_port,
			       SPA_PARAM_EnumFormat, &state,
			       filter, &format, &b)) != 1) {
		debug_params(this, link->out_node, SPA_DIRECTION_OUTPUT, link->out_port,
				SPA_PARAM_EnumFormat, filter);
		return -ENOTSUP;
	}
	filter = format;
	state = 0;
	if ((res = spa_node_port_enum_params_sync(link->in_node,
			       SPA_DIRECTION_INPUT, link->in_port,
			       SPA_PARAM_EnumFormat, &state,
			       filter, &format, &b)) != 1) {
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

	spa_log_debug(this->log, "setup convert");

	this->n_nodes = 0;
	/* unpack */
	this->nodes[this->n_nodes++] = this->fmt[SPA_DIRECTION_INPUT];
	/* down mix */
	this->nodes[this->n_nodes++] = this->channelmix;
	/* resample */
	this->nodes[this->n_nodes++] = this->resample;
	/* pack */
	this->nodes[this->n_nodes++] = this->fmt[SPA_DIRECTION_OUTPUT];

	make_link(this, this->nodes[0], 0, this->nodes[1], 0, 2);
	make_link(this, this->nodes[1], 0, this->nodes[2], 0, 2);
	make_link(this, this->nodes[2], 0, this->nodes[3], 0, 1);

	for (i = 0, j = this->n_links - 1; j >= i; i++, j--) {
		spa_log_debug(this->log, "negotiate %d", i);
		if ((res = negotiate_link_format(this, &this->links[i])) < 0)
			return res;
		spa_log_debug(this->log, "negotiate %d", j);
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
	int res;
	bool in_alloc, out_alloc;
	uint32_t i, size, buffers, blocks, align, flags;
	uint32_t *aligns;
	struct spa_data *datas;

	if (link->n_buffers > 0)
		return 0;

	state = 0;
	if ((res = spa_node_port_enum_params_sync(link->in_node,
			       SPA_DIRECTION_INPUT, link->in_port,
			       SPA_PARAM_Buffers, &state,
			       param, &param, &b)) != 1) {
		debug_params(this, link->out_node, SPA_DIRECTION_OUTPUT, link->out_port,
				SPA_PARAM_Buffers, param);
		return -ENOTSUP;
	}
	state = 0;
	if ((res = spa_node_port_enum_params_sync(link->out_node,
			       SPA_DIRECTION_OUTPUT, link->out_port,
			       SPA_PARAM_Buffers, &state,
			       param, &param, &b)) != 1) {
		debug_params(this, link->in_node, SPA_DIRECTION_INPUT, link->in_port,
				SPA_PARAM_Buffers, param);
		return -ENOTSUP;
	}

	spa_pod_fixate(param);

	in_alloc = SPA_FLAG_CHECK(link->in_flags,
				SPA_PORT_FLAG_CAN_ALLOC_BUFFERS);
	out_alloc = SPA_FLAG_CHECK(link->out_flags,
				SPA_PORT_FLAG_CAN_ALLOC_BUFFERS);

	flags = 0;
	if (out_alloc || in_alloc) {
		flags |= SPA_BUFFER_ALLOC_FLAG_NO_DATA;
		if (out_alloc)
			in_alloc = false;
	}

	if (spa_pod_parse_object(param,
		SPA_TYPE_OBJECT_ParamBuffers, NULL,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(&buffers),
		SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(&blocks),
		SPA_PARAM_BUFFERS_size,    SPA_POD_Int(&size),
		SPA_PARAM_BUFFERS_align,   SPA_POD_Int(&align)) < 0)
		return -EINVAL;

	spa_log_debug(this->log, "%p: buffers %d, blocks %d, size %d, align %d",
			this, buffers, blocks, size, align);

	datas = alloca(sizeof(struct spa_data) * blocks);
	memset(datas, 0, sizeof(struct spa_data) * blocks);
	aligns = alloca(sizeof(uint32_t) * blocks);
	for (i = 0; i < blocks; i++) {
		datas[i].type = SPA_DATA_MemPtr;
		datas[i].flags = SPA_DATA_FLAG_DYNAMIC;
		datas[i].maxsize = size;
		aligns[i] = align;
	}

	buffers = SPA_MAX(link->min_buffers, buffers);

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

	spa_log_debug(this->log, NAME " %p: %d %d", this, direction, this->n_links);

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

static int impl_node_enum_params(struct spa_node *node, int seq,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct impl *this;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumProfile:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamProfile, id,
				SPA_PARAM_PROFILE_direction, SPA_POD_Id(SPA_DIRECTION_INPUT));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamProfile, id,
				SPA_PARAM_PROFILE_direction, SPA_POD_Id(SPA_DIRECTION_OUTPUT));
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_PropInfo:
		return spa_node_enum_params(this->channelmix, seq, id, start, num, filter);

	case SPA_PARAM_Props:
		return spa_node_enum_params(this->channelmix, seq, id, start, num, filter);

	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int impl_node_set_io(struct spa_node *node, uint32_t id, void *data, size_t size)
{
	return -ENOTSUP;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	int res = 0;
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	switch (id) {
	case SPA_PARAM_Profile:
	{
		enum spa_direction direction;
		struct spa_pod *format;
		struct spa_audio_info info = { 0, };
		struct spa_pod_builder b = { 0 };
		uint8_t buffer[1024];

		if (spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_direction, SPA_POD_Id(&direction),
				SPA_PARAM_PROFILE_format, SPA_POD_Pod(&format)) < 0)
			return -EINVAL;

		if (!SPA_POD_IS_OBJECT_TYPE(format, SPA_TYPE_OBJECT_Format))
			return -EINVAL;

		if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return res;

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
			return -EINVAL;

		switch (direction) {
		case SPA_DIRECTION_INPUT:
		case SPA_DIRECTION_OUTPUT:
			spa_log_debug(this->log, NAME " %p: profile %d", this, info.info.raw.channels);

			info.info.raw.format = SPA_AUDIO_FORMAT_F32P;

			spa_pod_builder_init(&b, buffer, sizeof(buffer));

			param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info.info.raw);
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamProfile, id,
				SPA_PARAM_PROFILE_direction,  SPA_POD_Id(direction),
				SPA_PARAM_PROFILE_format,     SPA_POD_Pod(param));
			res = spa_node_set_param(this->fmt[direction], id, flags, param);
			break;
		default:
			res = -EINVAL;
			break;
		}
		break;
	}
	case SPA_PARAM_Props:
	{
		res = spa_node_set_param(this->channelmix, id, flags, param);
		break;
	}
	default:
		res = -ENOTSUP;
		break;
	}
	return res;
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

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

static void on_node_result(void *data, int seq, int res, const void *result)
{
	struct impl *this = data;
	spa_log_debug(this->log, "%p: result %d %d", this, seq, res);
	spa_node_emit_result(&this->hooks, seq, res, result);
}

static void fmt_input_port_info(void *data,
		enum spa_direction direction, uint32_t port,
		const struct spa_port_info *info)
{
	struct impl *this = data;
	if (direction == SPA_DIRECTION_INPUT ||
	    (this->mode == MODE_MERGE && port > 0))
		spa_node_emit_port_info(&this->hooks, direction, port, info);
}

static struct spa_node_events fmt_input_events = {
	SPA_VERSION_NODE_EVENTS,
	.port_info = fmt_input_port_info,
	.result = on_node_result,
};

static void fmt_output_port_info(void *data,
		enum spa_direction direction, uint32_t port,
		const struct spa_port_info *info)
{
	struct impl *this = data;
	if (direction == SPA_DIRECTION_OUTPUT)
		spa_node_emit_port_info(&this->hooks, direction, port, info);
}

static struct spa_node_events fmt_output_events = {
	SPA_VERSION_NODE_EVENTS,
	.port_info = fmt_output_port_info,
	.result = on_node_result,
};

static struct spa_node_events node_events = {
	SPA_VERSION_NODE_EVENTS,
	.result = on_node_result,
};

static int
impl_node_add_listener(struct spa_node *node,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *this;
	struct spa_hook_list save;
	struct spa_hook l[4];

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	spa_log_debug(this->log, "%p: add listener %p", this, listener);

	spa_zero(l);
	spa_node_add_listener(this->fmt[SPA_DIRECTION_INPUT],
			&l[0], &fmt_input_events, this);
	spa_node_add_listener(this->channelmix,
			&l[1], &node_events, this);
	spa_node_add_listener(this->resample,
			&l[2], &node_events, this);
	spa_node_add_listener(this->fmt[SPA_DIRECTION_OUTPUT],
			&l[3], &fmt_output_events, this);

	spa_hook_remove(&l[0]);
	spa_hook_remove(&l[1]);
	spa_hook_remove(&l[2]);
	spa_hook_remove(&l[3]);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(struct spa_node *node,
			const struct spa_node_callbacks *callbacks,
			void *user_data)
{
	return 0;
}

static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	return spa_node_add_port(this->fmt[direction], direction, port_id, props);
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
impl_node_port_enum_params(struct spa_node *node, int seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t start, uint32_t num,
			   const struct spa_pod *filter)
{
	struct impl *this;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_PropInfo:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_volume),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Float(1.0, 0.0, 10.0));
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_IO:
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,	SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size,	SPA_POD_Int(sizeof(struct spa_io_buffers)));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Range),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_range)));
			break;
		case 2:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Control),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_sequence)));
			break;
		default:
			return 0;
		}
		break;
	default:
	{
		struct spa_node *target;

		if (this->mode == MODE_MERGE && port_id > 0 && direction == SPA_DIRECTION_OUTPUT)
			target = this->fmt[SPA_DIRECTION_INPUT];
		else
			target = this->fmt[direction];

		return spa_node_port_enum_params(target, seq, direction, port_id,
			id, start, num, filter);
	}
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this;
	int res;
	struct spa_node *target;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (this->mode == MODE_MERGE && port_id > 0 && direction == SPA_DIRECTION_OUTPUT)
		target = this->fmt[SPA_DIRECTION_INPUT];
	else
		target = this->fmt[direction];

	if ((res = spa_node_port_set_param(target,
					direction, port_id, id, flags, param)) < 0)
		return res;

	if (id == SPA_PARAM_Format) {
		if (param == NULL)
			clean_convert(this);
		else if ((direction == SPA_DIRECTION_OUTPUT && this->mode == MODE_MERGE) ||
		    (direction == SPA_DIRECTION_INPUT && this->mode == MODE_SPLIT)) {
			if ((res = setup_convert(this)) < 0)
				return res;
		}
	}
	return res;
}

static int
impl_node_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this;
	int res;
	struct spa_node *target;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (this->mode == MODE_MERGE && port_id > 0 && direction == SPA_DIRECTION_OUTPUT)
		target = this->fmt[SPA_DIRECTION_INPUT];
	else
		target = this->fmt[direction];

	if ((res = spa_node_port_use_buffers(target,
					direction, port_id, buffers, n_buffers)) < 0)
		return res;

	if ((direction == SPA_DIRECTION_OUTPUT && this->mode == MODE_MERGE) ||
	    (direction == SPA_DIRECTION_INPUT && this->mode == MODE_SPLIT)) {
		if ((res = setup_buffers(this, SPA_DIRECTION_INPUT)) < 0)
			return res;
	}
	return res;
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
	struct spa_node *target;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (this->mode == MODE_MERGE && port_id > 0 && direction == SPA_DIRECTION_OUTPUT)
		target = this->fmt[SPA_DIRECTION_INPUT];
	else
		target = this->fmt[direction];

	return spa_node_port_alloc_buffers(target, direction, port_id,
			params, n_params, buffers, n_buffers);
}

static int
impl_node_port_set_io(struct spa_node *node,
		      enum spa_direction direction, uint32_t port_id,
		      uint32_t id, void *data, size_t size)
{
	struct impl *this;
	struct spa_node *target;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_log_debug(this->log, "set io %d %d %d", id, direction, port_id);

	switch (id) {
	case SPA_IO_Range:
		res = spa_node_port_set_io(this->resample, direction, 0, id, data, size);
		break;
	case SPA_IO_Control:
		res = spa_node_port_set_io(this->resample, direction, 0, id, data, size);
		res = spa_node_port_set_io(this->channelmix, direction, 0, id, data, size);
		break;
	default:
		if (this->mode == MODE_MERGE && port_id > 0 && direction == SPA_DIRECTION_OUTPUT)
			target = this->fmt[SPA_DIRECTION_INPUT];
		else
			target = this->fmt[direction];

		res = spa_node_port_set_io(target, direction, port_id, id, data, size);
		break;
	}
	return res;
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this;
	struct spa_node *target;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	if (this->mode == MODE_MERGE && port_id > 0)
		target = this->fmt[SPA_DIRECTION_INPUT];
	else
		target = this->fmt[SPA_DIRECTION_OUTPUT];

	return spa_node_port_reuse_buffer(target, port_id, buffer_id);
}

static int impl_node_process(struct spa_node *node)
{
	struct impl *this;
	int r, i, res = SPA_STATUS_OK;
	int ready;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_log_trace_fp(this->log, NAME " %p: process %d", this, this->n_links);

	while (1) {
		res = SPA_STATUS_OK;
		ready = 0;
		for (i = 0; i < this->n_nodes; i++) {
			r = spa_node_process(this->nodes[i]);
			spa_log_trace_fp(this->log, NAME " %p: process %d %d", this, i, r);

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

	spa_log_trace_fp(this->log, NAME " %p: process result: %d", this, res);

	return res;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.enum_params = impl_node_enum_params,
	.set_param = impl_node_set_param,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
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

	spa_handle_clear(this->hnd_fmt[SPA_DIRECTION_INPUT]);
	spa_handle_clear(this->hnd_channelmix);
	spa_handle_clear(this->hnd_resample);
	spa_handle_clear(this->hnd_fmt[SPA_DIRECTION_OUTPUT]);

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
	spa_hook_list_init(&this->hooks);

	if (info == NULL || (str = spa_dict_lookup(info, "factory.mode")) == NULL)
		str = "convert";

	if (strcmp(str, "split") == 0) {
		this->mode = MODE_SPLIT;
		in_factory = &spa_fmtconvert_factory;
		out_factory = &spa_splitter_factory;
	}
	else if (strcmp(str, "merge") == 0) {
		this->mode = MODE_MERGE;
		in_factory = &spa_merger_factory;
		out_factory = &spa_fmtconvert_factory;
	}
	else {
		this->mode = MODE_CONVERT;
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

	spa_node_add_listener(this->fmt[SPA_DIRECTION_INPUT],
			&this->listener[0], &fmt_input_events, this);
	spa_node_add_listener(this->channelmix,
			&this->listener[1], &node_events, this);
	spa_node_add_listener(this->resample,
			&this->listener[2], &node_events, this);
	spa_node_add_listener(this->fmt[SPA_DIRECTION_OUTPUT],
			&this->listener[3], &fmt_output_events, this);

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
