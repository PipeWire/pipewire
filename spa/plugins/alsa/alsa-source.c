/* Spa ALSA Source
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
#include <spa/list.h>
#include <spa/audio/format.h>
#include <lib/props.h>

#define NAME "alsa-source"

#include "alsa-utils.h"

#define CHECK_PORT(this,d,p)    ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)

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

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(props != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_pod_builder_init(&b, this->props_buffer, sizeof(this->props_buffer));

	*props = spa_pod_builder_props(&b,
		this->type.props,
		":", this->type.prop_device,      "S", this->props.device, sizeof(this->props.device),
		":", this->type.prop_device_name, "S", this->props.device_name, sizeof(this->props.device_name),
		":", this->type.prop_card_name,   "S", this->props.card_name, sizeof(this->props.card_name),
		":", this->type.prop_min_latency, "ir", this->props.min_latency,
								2, 1, INT32_MAX);

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
		spa_props_parse(props,
			":", this->type.prop_device,      "?S", this->props.device, sizeof(this->props.device),
			":", this->type.prop_min_latency, "?i", &this->props.min_latency, NULL);
	}

	return SPA_RESULT_OK;
}

static int do_send_done(struct spa_loop *loop, bool async, uint32_t seq, size_t size, const void *data, void *user_data)
{
	struct state *this = user_data;

	this->callbacks->done(this->callbacks_data, seq, *(int*)data);

	return SPA_RESULT_OK;
}

static int do_start(struct spa_loop *loop, bool async, uint32_t seq, size_t size, const void *data, void *user_data)
{
	struct state *this = user_data;
	int res;

	res = spa_alsa_start(this, false);

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

static int do_pause(struct spa_loop *loop, bool async, uint32_t seq, size_t size, const void *data, void *user_data)
{
	struct state *this = user_data;
	int res;

	res = spa_alsa_pause(this, false);

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

	if (SPA_COMMAND_TYPE(command) == this->type.command_node.Start) {
		if (!this->have_format)
			return SPA_RESULT_NO_FORMAT;

		if (this->n_buffers == 0)
			return SPA_RESULT_NO_BUFFERS;

		return spa_loop_invoke(this->data_loop, do_start, ++this->seq, 0, NULL, false, this);
	} else if (SPA_COMMAND_TYPE(command) == this->type.command_node.Pause) {
		if (!this->have_format)
			return SPA_RESULT_NO_FORMAT;

		if (this->n_buffers == 0)
			return SPA_RESULT_NO_BUFFERS;

		return spa_loop_invoke(this->data_loop, do_pause, ++this->seq, 0, NULL, false, this);
	} else
		return SPA_RESULT_NOT_IMPLEMENTED;

	return SPA_RESULT_OK;
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
		*n_input_ports = 0;
	if (max_input_ports)
		*max_input_ports = 0;
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

	if (n_output_ports > 0 && output_ids != NULL)
		output_ids[0] = 0;

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
			    struct spa_format **format, const struct spa_format *filter, uint32_t index)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	return spa_alsa_enum_format(this, format, filter, index);
}

static void recycle_buffer(struct state *this, uint32_t buffer_id)
{
	struct buffer *b;

	spa_log_trace(this->log, NAME " %p: recycle buffer %u", this, buffer_id);

	b = &this->buffers[buffer_id];
	spa_return_if_fail(b->outstanding);

	b->outstanding = false;
	spa_list_append(&this->free, &b->link);
}

static int clear_buffers(struct state *this)
{
	if (this->n_buffers > 0) {
		spa_list_init(&this->free);
		spa_list_init(&this->ready);
		this->n_buffers = 0;
	}
	return SPA_RESULT_OK;
}

static int
impl_node_port_set_format(struct spa_node *node,
			  enum spa_direction direction,
			  uint32_t port_id, uint32_t flags, const struct spa_format *format)
{
	struct state *this;
	int err;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	if (format == NULL) {
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

		if (spa_format_audio_raw_parse(format, &info.info.raw, &this->type.format_audio) < 0)
			return SPA_RESULT_INVALID_MEDIA_TYPE;

		if ((err = spa_alsa_set_format(this, &info, flags)) < 0)
			return err;

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
			  enum spa_direction direction, uint32_t port_id, const struct spa_format **format)
{
	struct state *this;
	struct spa_pod_builder b = { NULL, };
	struct type *t;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	if (!this->have_format)
		return SPA_RESULT_NO_FORMAT;

	spa_pod_builder_init(&b, this->format_buffer, sizeof(this->format_buffer));
	*format = spa_pod_builder_format(&b,
		t->format,
		t->media_type.audio, t->media_subtype.raw,
		":", t->format_audio.format,   "I", this->current_format.info.raw.format,
		":", t->format_audio.rate,     "i", this->current_format.info.raw.rate,
		":", t->format_audio.channels, "i", this->current_format.info.raw.channels);

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
			   enum spa_direction direction, uint32_t port_id, uint32_t index, struct spa_param **param)
{
	struct state *this;
	struct spa_pod_builder b = { NULL, };
	struct type *t;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(param != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);
	t = &this->type;

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	spa_pod_builder_init(&b, this->params_buffer, sizeof(this->params_buffer));

	switch (index) {
	case 0:
		*param = spa_pod_builder_param(&b,
			t->param_alloc_buffers.Buffers,
			":", t->param_alloc_buffers.size,    "i", this->props.min_latency * this->frame_size,
			":", t->param_alloc_buffers.stride,  "i", 0,
			":", t->param_alloc_buffers.buffers, "ir", 2,
									2, 1, 32,
			":", t->param_alloc_buffers.align,   "i", 16);
		break;

	case 1:
		*param = spa_pod_builder_param(&b,
			t->param_alloc_meta_enable.MetaEnable,
			":", t->param_alloc_meta_enable.type, "I", t->meta.Header,
			":", t->param_alloc_meta_enable.size, "i", sizeof(struct spa_meta_header));
		break;

	default:
		return SPA_RESULT_NOT_IMPLEMENTED;
	}

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
	int res;
	int i;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), SPA_RESULT_INVALID_PORT);

	if (!this->have_format)
		return SPA_RESULT_NO_FORMAT;

	if (this->n_buffers > 0) {
		spa_alsa_pause(this, false);
		if ((res = clear_buffers(this)) < 0)
			return res;
	}
	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &this->buffers[i];
		struct spa_data *d = buffers[i]->datas;

		b->outbuf = buffers[i];
		b->outstanding = false;

		b->h = spa_buffer_find_meta(b->outbuf, this->type.meta.Header);

		if (!((d[0].type == this->type.data.MemFd ||
		       d[0].type == this->type.data.DmaBuf ||
		       d[0].type == this->type.data.MemPtr) && d[0].data != NULL)) {
			spa_log_error(this->log, NAME " %p: need mapped memory", this);
			return SPA_RESULT_ERROR;
		}
		spa_list_append(&this->free, &b->link);
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

	if (this->n_buffers == 0)
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
	struct state *this;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(port_id == 0, SPA_RESULT_INVALID_PORT);

	if (this->n_buffers == 0)
		return SPA_RESULT_NO_BUFFERS;

	if (buffer_id >= this->n_buffers)
		return SPA_RESULT_INVALID_BUFFER_ID;

	recycle_buffer(this, buffer_id);

	return SPA_RESULT_OK;
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
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int impl_node_process_output(struct spa_node *node)
{
	struct state *this;
	struct spa_port_io *io;

	spa_return_val_if_fail(node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(node, struct state, node);
	io = this->io;
	spa_return_val_if_fail(io != NULL, SPA_RESULT_WRONG_STATE);

	if (io->status == SPA_RESULT_HAVE_BUFFER)
		return SPA_RESULT_HAVE_BUFFER;

	if (io->buffer_id < this->n_buffers) {
		recycle_buffer(this, io->buffer_id);
		io->buffer_id = SPA_ID_INVALID;
	}
	return SPA_RESULT_OK;
}

static const struct spa_dict_item node_info_items[] = {
	{ "media.class", "Audio/Source" },
};

static const struct spa_dict node_info = {
	SPA_N_ELEMENTS(node_info_items),
	node_info_items
};

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	&node_info,
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

static int impl_clock_get_props(struct spa_clock *clock, struct spa_props **props)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int impl_clock_set_props(struct spa_clock *clock, const struct spa_props *props)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int impl_clock_get_time(struct spa_clock *clock,
			       int32_t *rate,
			       int64_t *ticks,
			       int64_t *monotonic_time)
{
	struct state *this;

	spa_return_val_if_fail(clock != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = SPA_CONTAINER_OF(clock, struct state, clock);

	if (rate)
		*rate = SPA_USEC_PER_SEC;
	if (ticks)
		*ticks = this->last_ticks;
	if (monotonic_time)
		*monotonic_time = this->last_monotonic;

	return SPA_RESULT_OK;
}

static const struct spa_clock impl_clock = {
	SPA_VERSION_CLOCK,
	NULL,
	SPA_CLOCK_STATE_STOPPED,
	impl_clock_get_props,
	impl_clock_set_props,
	impl_clock_get_time,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t interface_id, void **interface)
{
	struct state *this;

	spa_return_val_if_fail(handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(interface != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	this = (struct state *) handle;

	if (interface_id == this->type.node)
		*interface = &this->node;
	else if (interface_id == this->type.clock)
		*interface = &this->clock;
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
		spa_log_error(this->log, "an id-map is needed");
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
	this->clock = impl_clock;
	this->stream = SND_PCM_STREAM_CAPTURE;
	reset_props(&this->props);

	spa_list_init(&this->free);
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
	{SPA_TYPE__Clock,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t index)
{
	spa_return_val_if_fail(factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
	spa_return_val_if_fail(info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

	if (index >= SPA_N_ELEMENTS(impl_interfaces))
		return SPA_RESULT_ENUM_END;

	*info = &impl_interfaces[index];

	return SPA_RESULT_OK;
}

static const struct spa_dict_item info_items[] = {
	{ "factory.author", "Wim Taymans <wim.taymans@gmail.com>" },
	{ "factory.description", "Record audio with the alsa API" },
};

static const struct spa_dict info = {
	SPA_N_ELEMENTS(info_items),
	info_items
};

const struct spa_handle_factory spa_alsa_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	&info,
	sizeof(struct state),
	impl_init,
	impl_enum_interface_info,
};
