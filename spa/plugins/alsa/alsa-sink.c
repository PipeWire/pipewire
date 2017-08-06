/* Spa ALSA Sink
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

#include <asoundlib.h>

#include <spa/node.h>
#include <spa/audio/format.h>
#include <lib/props.h>

#define NAME "alsa-sink"

#include "alsa-utils.h"

#define CHECK_PORT(this,d,p)    ((d) == SPA_DIRECTION_INPUT && (p) == 0)

static const char default_device[] = "hw:0";
static const uint32_t default_min_latency = 1024;

static void reset_props(struct props *props)
{
	strncpy(props->device, default_device, 64);
	props->min_latency = default_min_latency;
}

static int impl_node_get_props(struct spa_node *node, struct spa_props **props)
{
	struct state *this;
	struct spa_pod_builder b = { NULL, };
	struct spa_pod_frame f[2];

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(props != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_pod_builder_init(&b, this->props_buffer, sizeof(this->props_buffer));

	spa_pod_builder_props(&b, &f[0], this->type.props,
		PROP(&f[1], this->type.prop_device, -SPA_POD_TYPE_STRING,
			this->props.device, sizeof(this->props.device)),
		PROP(&f[1], this->type.prop_device_name, -SPA_POD_TYPE_STRING,
			this->props.device_name, sizeof(this->props.device_name)),
		PROP(&f[1], this->type.prop_card_name, -SPA_POD_TYPE_STRING,
			this->props.card_name, sizeof(this->props.card_name)),
		PROP_MM(&f[1], this->type.prop_min_latency, SPA_POD_TYPE_INT,
			this->props.min_latency,
			1, INT32_MAX));
	*props = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_props);

	return SPA_RESULT_OK;
}

static int impl_node_set_props(struct spa_node *node, const struct spa_props *props)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	if (props == NULL) {
		reset_props(&this->props);
		return SPA_RESULT_OK;
	} else {
		spa_props_query(props,
				this->type.prop_device, -SPA_POD_TYPE_STRING,
					this->props.device, sizeof(this->props.device),
				this->type.prop_min_latency, SPA_POD_TYPE_INT, &this->props.min_latency, 0);
	}
	return SPA_RESULT_OK;
}

static int do_send_done(struct spa_loop *loop, bool async, uint32_t seq, size_t size, const void *data, void *user_data)
{
	struct state *this = user_data;

	this->callbacks->done(this->callbacks_data, seq, *(int*)data);

	return SPA_RESULT_OK;
}

static int do_command(struct spa_loop *loop, bool async, uint32_t seq, size_t size, const void *data, void *user_data)
{
	struct state *this = user_data;
	int res;
	const struct spa_command *cmd = data;

	if (SPA_COMMAND_TYPE(cmd) == this->type.command_node.Start ||
	    SPA_COMMAND_TYPE(cmd) == this->type.command_node.Pause) {
		res = spa_node_port_send_command(&this->node, SPA_DIRECTION_INPUT, 0, cmd);
	} else
		res = SPA_RESULT_NOT_IMPLEMENTED;

	if (async) {
		spa_loop_invoke(this->main_loop,
				do_send_done,
				seq,
				sizeof(res),
				&res,
				false,
				this);
	}
	return res;
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(command != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	if (SPA_COMMAND_TYPE(command) == this->type.command_node.Start ||
	    SPA_COMMAND_TYPE(command) == this->type.command_node.Pause) {
		if (!this->have_format)
			return SPA_RESULT_NO_FORMAT;

		if (this->n_buffers == 0)
			return SPA_RESULT_NO_BUFFERS;

		return spa_loop_invoke(this->data_loop,
				       do_command,
				       ++this->seq,
				       SPA_POD_SIZE(command),
				       command,
				       false,
				       this);

	} else
		return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_set_callbacks(struct spa_node *node,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

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
		*n_output_ports = 0;
	if (max_output_ports)
		*max_output_ports = 0;

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

	if (n_input_ports > 0 && input_ids != NULL)
		input_ids[0] = 0;

	return SPA_RESULT_OK;
}


static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_port_enum_formats(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    struct spa_format **format,
			    const struct spa_format *filter,
			    uint32_t index)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	return spa_alsa_enum_format(this, format, filter, index);
}

static int clear_buffers(struct state *this)
{
	if (this->n_buffers > 0) {
		spa_list_init(&this->ready);
		this->n_buffers = 0;
	}
	return SPA_RESULT_OK;
}

static int
impl_node_port_set_format(struct spa_node *node,
			  enum spa_direction direction,
			  uint32_t port_id,
			  uint32_t flags,
			  const struct spa_format *format)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	if (format == NULL) {
		spa_log_info(this->log, "clear format");
		spa_alsa_pause(this, false);
		clear_buffers(this);
		spa_alsa_close(this);
		this->have_format = false;
	} else {
		struct spa_audio_info info = { SPA_FORMAT_MEDIA_TYPE(format),
			SPA_FORMAT_MEDIA_SUBTYPE(format),
		};

		if (info.media_type != this->type.media_type.audio ||
		    info.media_subtype != this->type.media_subtype.raw)
			return SPA_RESULT_INVALID_MEDIA_TYPE;

		if (!spa_format_audio_raw_parse(format, &info.info.raw, &this->type.format_audio))
			return SPA_RESULT_INVALID_MEDIA_TYPE;

		if (spa_alsa_set_format(this, &info, flags) < 0)
			return SPA_RESULT_ERROR;

		this->current_format = info;
		this->have_format = true;
	}

	if (this->have_format) {
		this->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS | SPA_PORT_INFO_FLAG_LIVE;
		this->info.rate = this->rate;
	}

	return SPA_RESULT_OK;
}

static int
impl_node_port_get_format(struct spa_node *node,
			  enum spa_direction direction,
			  uint32_t port_id,
			  const struct spa_format **format)
{
	struct state *this;
	struct spa_pod_builder b = { NULL, };
	struct spa_pod_frame f[2];

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	if (!this->have_format)
		return SPA_RESULT_NO_FORMAT;

	spa_pod_builder_init(&b, this->format_buffer, sizeof(this->format_buffer));
	spa_pod_builder_format(&b, &f[0], this->type.format,
		this->type.media_type.audio,
		this->type.media_subtype.raw,
		PROP(&f[1], this->type.format_audio.format, SPA_POD_TYPE_ID,
			this->current_format.info.raw.format),
		PROP(&f[1], this->type.format_audio.rate, SPA_POD_TYPE_INT,
			this->current_format.info.raw.rate),
		PROP(&f[1], this->type.format_audio.channels, SPA_POD_TYPE_INT,
			this->current_format.info.raw.channels));
	*format = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_format);

	return SPA_RESULT_OK;
}

static int
impl_node_port_get_info(struct spa_node *node,
			enum spa_direction direction, uint32_t port_id, const struct spa_port_info **info)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	*info = &this->info;

	return SPA_RESULT_OK;
}

static int
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t index,
			   struct spa_param **param)
{

	struct state *this;
	struct spa_pod_builder b = { NULL };
	struct spa_pod_frame f[2];

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(param != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	spa_pod_builder_init(&b, this->params_buffer, sizeof(this->params_buffer));

	switch (index) {
	case 0:
		spa_pod_builder_object(&b, &f[0], 0, this->type.param_alloc_buffers.Buffers,
			PROP(&f[1], this->type.param_alloc_buffers.size, SPA_POD_TYPE_INT,
				this->props.min_latency * this->frame_size),
			PROP(&f[1], this->type.param_alloc_buffers.stride, SPA_POD_TYPE_INT,
				0),
			PROP_MM(&f[1], this->type.param_alloc_buffers.buffers, SPA_POD_TYPE_INT,
				32,
				1, 32),
			PROP(&f[1], this->type.param_alloc_buffers.align, SPA_POD_TYPE_INT,
				16));
		break;

	case 1:
		spa_pod_builder_object(&b, &f[0], 0, this->type.param_alloc_meta_enable.MetaEnable,
			PROP(&f[1], this->type.param_alloc_meta_enable.type, SPA_POD_TYPE_ID,
				this->type.meta.Header),
			PROP(&f[1], this->type.param_alloc_meta_enable.size, SPA_POD_TYPE_INT,
				sizeof(struct spa_meta_header)));
		break;

	case 2:
		spa_pod_builder_object(&b, &f[0], 0, this->type.param_alloc_meta_enable.MetaEnable,
			PROP(&f[1], this->type.param_alloc_meta_enable.type, SPA_POD_TYPE_ID,
				this->type.meta.Ringbuffer),
			PROP(&f[1], this->type.param_alloc_meta_enable.size, SPA_POD_TYPE_INT,
				sizeof(struct spa_meta_ringbuffer)),
			PROP(&f[1], this->type.param_alloc_meta_enable.ringbufferSize, SPA_POD_TYPE_INT,
				this->period_frames * this->frame_size * 32),
			PROP(&f[1], this->type.param_alloc_meta_enable.ringbufferStride, SPA_POD_TYPE_INT,
				0),
			PROP(&f[1], this->type.param_alloc_meta_enable.ringbufferBlocks, SPA_POD_TYPE_INT,
				1),
			PROP(&f[1], this->type.param_alloc_meta_enable.ringbufferAlign, SPA_POD_TYPE_INT,
				16));
		break;

	default:
		return SPA_RESULT_NOT_IMPLEMENTED;
	}

	*param = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_param);

	return SPA_RESULT_OK;
}

static int
impl_node_port_set_param(struct spa_node *node,
			 enum spa_direction direction, uint32_t port_id, const struct spa_param *param)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id, struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct state *this;
	int i;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	spa_log_info(this->log, "use buffers %d", n_buffers);

	if (!this->have_format)
		return SPA_RESULT_NO_FORMAT;

	if (n_buffers == 0) {
		spa_alsa_pause(this, false);
		clear_buffers(this);
		return SPA_RESULT_OK;
	}

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &this->buffers[i];
		uint32_t type = buffers[i]->datas[0].type;

		b->outbuf = buffers[i];
		b->outstanding = true;

		b->h = spa_buffer_find_meta(b->outbuf, this->type.meta.Header);
		b->rb = spa_buffer_find_meta(b->outbuf, this->type.meta.Ringbuffer);

		if ((type == this->type.data.MemFd ||
		     type == this->type.data.DmaBuf ||
		     type == this->type.data.MemPtr) && buffers[i]->datas[0].data == NULL) {
			spa_log_error(this->log, NAME " %p: need mapped memory", this);
			return SPA_RESULT_ERROR;
		}
	}
	this->n_buffers = n_buffers;

	return SPA_RESULT_OK;
}

static int
impl_node_port_alloc_buffers(struct spa_node *node,
			     enum spa_direction direction,
			     uint32_t port_id,
			     struct spa_param **params,
			     uint32_t n_params,
			     struct spa_buffer **buffers,
			     uint32_t *n_buffers)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(buffers != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	if (!this->have_format)
		return SPA_RESULT_NO_FORMAT;

	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_port_set_io(struct spa_node *node, enum spa_direction direction, uint32_t port_id, struct spa_port_io *io)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	this->io = io;

	return SPA_RESULT_OK;
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction, uint32_t port_id, const struct spa_command *command)
{
	struct state *this;
	int res;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	if (SPA_COMMAND_TYPE(command) == this->type.command_node.Pause) {
		res = spa_alsa_pause(this, false);
	} else if (SPA_COMMAND_TYPE(command) == this->type.command_node.Start) {
		res = spa_alsa_start(this, false);
	} else
		res = SPA_RESULT_NOT_IMPLEMENTED;

	return res;
}

static int impl_node_process_input(struct spa_node *node)
{
	struct state *this;
	struct spa_port_io *input;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);
	input = this->io;
	spa_return_val_if_fail(input != NULL, SPA_RESULT_WRONG_STATE);

	if (input->status == SPA_RESULT_HAVE_BUFFER && input->buffer_id != SPA_ID_INVALID) {
		struct buffer *b = &this->buffers[input->buffer_id];

		if (!b->outstanding) {
			spa_log_warn(this->log, NAME " %p: buffer %u in use", this, input->buffer_id);
			input->status = SPA_RESULT_INVALID_BUFFER_ID;
			return SPA_RESULT_ERROR;
		}

		spa_log_trace(this->log, NAME " %p: queue buffer %u", this, input->buffer_id);

		spa_list_insert(this->ready.prev, &b->link);
		b->outstanding = false;
		input->buffer_id = SPA_ID_INVALID;
		input->status = SPA_RESULT_OK;
	}
	return SPA_RESULT_OK;
}

static int impl_node_process_output(struct spa_node *node)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}


static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	NULL,
	impl_node_get_props,
	impl_node_set_props,
	impl_node_send_command,
	impl_node_set_callbacks,
	impl_node_get_n_ports,
	impl_node_get_port_ids,
	impl_node_add_port,
	impl_node_remove_port,
	impl_node_port_enum_formats,
	impl_node_port_set_format,
	impl_node_port_get_format,
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
	struct state *this;

	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(interface != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = (struct state *) handle;

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
	  struct spa_handle *handle, const struct spa_dict *info, const struct spa_support *support, uint32_t n_support)
{
	struct state *this;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct state *) handle;

	for (i = 0; i < n_support; i++) {
		if (strcmp(support[i].type, SPA_TYPE__TypeMap) == 0)
			this->map = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE__Log) == 0)
			this->log = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE_LOOP__DataLoop) == 0)
			this->data_loop = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE_LOOP__MainLoop) == 0)
			this->main_loop = support[i].data;
	}
	if (this->map == NULL) {
		spa_log_error(this->log, "a type-map is needed");
		return SPA_RESULT_ERROR;
	}
	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data loop is needed");
		return SPA_RESULT_ERROR;
	}
	if (this->main_loop == NULL) {
		spa_log_error(this->log, "a main loop is needed");
		return SPA_RESULT_ERROR;
	}
	init_type(&this->type, this->map);

	this->node = impl_node;
	this->stream = SND_PCM_STREAM_PLAYBACK;
	reset_props(&this->props);

	spa_list_init(&this->ready);

	for (i = 0; info && i < info->n_items; i++) {
		if (!strcmp(info->items[i].key, "alsa.card")) {
			snprintf(this->props.device, 63, "%s", info->items[i].value);
		}
	}

	return SPA_RESULT_OK;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE__Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info, uint32_t index)
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

const struct spa_handle_factory spa_alsa_sink_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	sizeof(struct state),
	impl_init,
	impl_enum_interface_info,
};
