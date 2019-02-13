/* Spa ALSA Source
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

#include <stddef.h>

#include <asoundlib.h>

#include <spa/node/node.h>
#include <spa/utils/list.h>
#include <spa/param/audio/format.h>
#include <spa/pod/filter.h>

#define NAME "alsa-source"

#include "alsa-utils.h"

#define CHECK_PORT(this,d,p)    ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)

static const char default_device[] = "hw:0";
static const uint32_t default_min_latency = 64;
static const uint32_t default_max_latency = 1024;

static void reset_props(struct props *props)
{
	strncpy(props->device, default_device, 64);
	props->min_latency = default_min_latency;
	props->max_latency = default_max_latency;
}

static int impl_node_enum_params(struct spa_node *node,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod *filter,
				 struct spa_pod **result,
				 struct spa_pod_builder *builder)
{
	struct state *this;
	struct spa_pod *param;
	uint8_t buffer[1024];
	struct spa_pod_builder b = { 0 };
	struct props *p;


	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);
	p = &this->props;

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_PropInfo,
				    SPA_PARAM_Props, };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_add_object(&b,
					SPA_TYPE_OBJECT_ParamList, id,
					SPA_PARAM_LIST_id, SPA_POD_Id(list[*index]));
		else
			return 0;
		break;
	}
	case SPA_PARAM_PropInfo:
		switch (*index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_device),
				SPA_PROP_INFO_name, SPA_POD_String("The ALSA device"),
				SPA_PROP_INFO_type, SPA_POD_Stringn(p->device, sizeof(p->device)));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_deviceName),
				SPA_PROP_INFO_name, SPA_POD_String("The ALSA device name"),
				SPA_PROP_INFO_type, SPA_POD_Stringn(p->device_name, sizeof(p->device_name)));
			break;
		case 2:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_cardName),
				SPA_PROP_INFO_name, SPA_POD_String("The ALSA card name"),
				SPA_PROP_INFO_type, SPA_POD_Stringn(p->card_name, sizeof(p->card_name)));
			break;
		case 3:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_minLatency),
				SPA_PROP_INFO_name, SPA_POD_String("The minimum latency"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(p->min_latency, 1, INT32_MAX));
			break;
		case 4:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   SPA_POD_Id(SPA_PROP_maxLatency),
				SPA_PROP_INFO_name, SPA_POD_String("The maximum latency"),
				SPA_PROP_INFO_type, SPA_POD_CHOICE_RANGE_Int(p->max_latency, 1, INT32_MAX));
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_Props:
		switch (*index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Props, id,
				SPA_PROP_device,      SPA_POD_Stringn(p->device, sizeof(p->device)),
				SPA_PROP_deviceName,  SPA_POD_Stringn(p->device_name, sizeof(p->device_name)),
				SPA_PROP_cardName,    SPA_POD_Stringn(p->card_name, sizeof(p->card_name)),
				SPA_PROP_minLatency,  SPA_POD_Int(p->min_latency),
				SPA_PROP_maxLatency,  SPA_POD_Int(p->max_latency));
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

static int impl_node_set_io(struct spa_node *node, uint32_t id, void *data, size_t size)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	switch (id) {
	case SPA_IO_Clock:
		this->clock = data;
		break;
	case SPA_IO_Position:
		this->position = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	switch (id) {
	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;

		if (param == NULL) {
			reset_props(p);
			return 0;
		}
		spa_pod_parse_object(param,
			SPA_TYPE_OBJECT_Props, NULL,
			SPA_PROP_device,     SPA_POD_OPT_Stringn(p->device, sizeof(p->device)),
			SPA_PROP_minLatency, SPA_POD_OPT_Int(&p->min_latency),
			SPA_PROP_maxLatency, SPA_POD_OPT_Int(&p->max_latency));
		break;
	}
	default:
		return -ENOENT;
	}

	return 0;
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct state *this;
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (!this->have_format)
			return -EIO;
		if (this->n_buffers == 0)
			return -EIO;

		if ((res = spa_alsa_start(this)) < 0)
			return res;
		break;
	case SPA_NODE_COMMAND_Pause:
		if ((res = spa_alsa_pause(this)) < 0)
			return res;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static const struct spa_dict_item node_info_items[] = {
	{ "media.class", "Audio/Source" },
	{ "node.driver", "true" },
};

static int
impl_node_set_callbacks(struct spa_node *node,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	this->callbacks = callbacks;
	this->callbacks_data = data;

	if (callbacks) {
		if (callbacks->info) {
			struct spa_node_info info;

			info = SPA_NODE_INFO_INIT();
			info.change_mask = SPA_NODE_CHANGE_MASK_PROPS;
			info.props = &SPA_DICT_INIT_ARRAY(node_info_items);

			callbacks->info(data, &info);
		}
	}
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

static int
impl_node_get_port_ids(struct spa_node *node,
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


static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_port_get_info(struct spa_node *node,
			enum spa_direction direction, uint32_t port_id, const struct spa_port_info **info)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	*info = &this->info;

	return 0;
}

static void recycle_buffer(struct state *this, uint32_t buffer_id)
{
	struct buffer *b = &this->buffers[buffer_id];

	if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_OUT)) {
		spa_log_trace(this->log, NAME " %p: recycle buffer %u", this, buffer_id);
		spa_list_append(&this->free, &b->link);
		SPA_FLAG_UNSET(b->flags, BUFFER_FLAG_OUT);
	}
}

static int
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t *index,
			   const struct spa_pod *filter,
			   struct spa_pod **result,
			   struct spa_pod_builder *builder)
{
	struct state *this;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_EnumFormat,
				    SPA_PARAM_Format,
				    SPA_PARAM_Buffers,
				    SPA_PARAM_Meta };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_add_object(&b,
					SPA_TYPE_OBJECT_ParamList, id,
					SPA_PARAM_LIST_id, SPA_POD_Id(list[*index]));
		else
			return 0;
		break;
	}
	case SPA_PARAM_EnumFormat:
		return spa_alsa_enum_format(this, index, filter, result, builder);

	case SPA_PARAM_Format:
		if (!this->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		param = spa_format_audio_raw_build(&b, id, &this->current_format.info.raw);
		break;

	case SPA_PARAM_Buffers:
		if (!this->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
							this->props.max_latency * this->frame_size,
							this->props.min_latency * this->frame_size,
							INT32_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(this->frame_size),
			SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));
		break;

	case SPA_PARAM_Meta:
		if (!this->have_format)
			return -EIO;

		switch (*index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_IO:
		switch (*index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Clock),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_clock)));
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

static int clear_buffers(struct state *this)
{
	if (this->n_buffers > 0) {
		spa_list_init(&this->free);
		spa_list_init(&this->ready);
		this->n_buffers = 0;
	}
	return 0;
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags, const struct spa_pod *format)
{
	struct state *this = SPA_CONTAINER_OF(node, struct state, node);
	int err;

	if (format == NULL) {
		spa_alsa_pause(this);
		clear_buffers(this);
		spa_alsa_close(this);
		this->have_format = false;
	} else {
		struct spa_audio_info info = { 0 };

		if ((err = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return err;

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
			return -EINVAL;

		if ((err = spa_alsa_set_format(this, &info, flags)) < 0)
			return err;

		this->current_format = info;
		this->have_format = true;
	}

	if (this->have_format) {
		this->info.rate = this->rate;
	}

	return 0;
}

static int
impl_node_port_set_param(struct spa_node *node,
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

static int
impl_node_port_use_buffers(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id, struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct state *this;
	int res;
	uint32_t i;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (!this->have_format)
		return -EIO;

	if (this->n_buffers > 0) {
		spa_alsa_pause(this);
		if ((res = clear_buffers(this)) < 0)
			return res;
	}
	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &this->buffers[i];
		struct spa_data *d = buffers[i]->datas;

		b->buf = buffers[i];
		b->id = i;
		b->flags = 0;

		b->h = spa_buffer_find_meta_data(b->buf, SPA_META_Header, sizeof(*b->h));

		if (!((d[0].type == SPA_DATA_MemFd ||
		       d[0].type == SPA_DATA_DmaBuf ||
		       d[0].type == SPA_DATA_MemPtr) && d[0].data != NULL)) {
			spa_log_error(this->log, NAME " %p: need mapped memory", this);
			return -EINVAL;
		}
		spa_list_append(&this->free, &b->link);
	}
	this->n_buffers = n_buffers;

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
	struct state *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(buffers != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	if (this->n_buffers == 0)
		return -EIO;

	return -ENOTSUP;
}

static int
impl_node_port_set_io(struct spa_node *node,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	switch (id) {
	case SPA_IO_Buffers:
		this->io = data;
		break;
	case SPA_IO_Clock:
		this->clock = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct state *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);

	spa_return_val_if_fail(port_id == 0, -EINVAL);

	if (this->n_buffers == 0)
		return -EIO;

	if (buffer_id >= this->n_buffers)
		return -EINVAL;

	recycle_buffer(this, buffer_id);

	return 0;
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction, uint32_t port_id, const struct spa_command *command)
{
	spa_return_val_if_fail(node != NULL, -EINVAL);
	return -ENOTSUP;
}

static int impl_node_process(struct spa_node *node)
{
	struct state *this;
	struct spa_io_buffers *io;
	struct buffer *b;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct state, node);
	io = this->io;
	spa_return_val_if_fail(io != NULL, -EIO);

	if (io->status == SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_HAVE_BUFFER;

	if (io->buffer_id < this->n_buffers) {
		recycle_buffer(this, io->buffer_id);
		io->buffer_id = SPA_ID_INVALID;
	}

	if (spa_list_is_empty(&this->ready))
		return SPA_STATUS_OK;

	b = spa_list_first(&this->ready, struct buffer, link);
	spa_list_remove(&b->link);

	spa_log_trace(this->log, NAME " %p: dequeue buffer %d", node, b->id);

	io->buffer_id = b->id;
	io->status = SPA_STATUS_HAVE_BUFFER;

	return SPA_STATUS_HAVE_BUFFER;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	impl_node_enum_params,
	impl_node_set_param,
	impl_node_set_io,
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
	struct state *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct state *) handle;

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
	return sizeof(struct state);
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

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct state *) handle;

	for (i = 0; i < n_support; i++) {
		if (support[i].type == SPA_TYPE_INTERFACE_Log)
			this->log = support[i].data;
		else if (support[i].type == SPA_TYPE_INTERFACE_DataLoop)
			this->data_loop = support[i].data;
		else if (support[i].type == SPA_TYPE_INTERFACE_MainLoop)
			this->main_loop = support[i].data;
	}
	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data loop is needed");
		return -EINVAL;
	}
	if (this->main_loop == NULL) {
		spa_log_error(this->log, "a main loop is needed");
		return -EINVAL;
	}

	this->node = impl_node;
	this->stream = SND_PCM_STREAM_CAPTURE;
	reset_props(&this->props);

	this->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
			   SPA_PORT_INFO_FLAG_LIVE |
			   SPA_PORT_INFO_FLAG_PHYSICAL |
			   SPA_PORT_INFO_FLAG_TERMINAL;

	spa_list_init(&this->free);
	spa_list_init(&this->ready);

	for (i = 0; info && i < info->n_items; i++) {
		if (!strcmp(info->items[i].key, "alsa.device")) {
			snprintf(this->props.device, 63, "%s", info->items[i].value);
		}
	}
	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
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

static const struct spa_dict_item info_items[] = {
	{ "factory.author", "Wim Taymans <wim.taymans@gmail.com>" },
	{ "factory.description", "Record audio with the alsa API" },
};

static const struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

const struct spa_handle_factory spa_alsa_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
