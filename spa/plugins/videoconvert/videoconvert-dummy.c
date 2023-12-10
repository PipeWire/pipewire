/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2023 columbarius */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <spa/support/plugin.h>
#include <spa/support/loop.h>
#include <spa/support/log.h>
#include <spa/utils/result.h>
#include <spa/utils/list.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/node/keys.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/param.h>
#include <spa/pod/filter.h>
#include <spa/debug/types.h>

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic
SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.videoconvert.dummy");

#define MAX_PORTS 1

struct props {
};

struct port {
	uint32_t direction;
	uint32_t id;

	struct spa_io_buffers *io;

	uint64_t info_all;
	struct spa_port_info info;
#define IDX_EnumFormat	0
#define IDX_Meta	1
#define IDX_IO		2
#define IDX_Format	3
#define IDX_Buffers	4
#define IDX_Latency	5
#define IDX_Tag		6
#define N_PORT_PARAMS	7
	struct spa_param_info params[N_PORT_PARAMS];
};

struct dir {
	struct port ports[MAX_PORTS];
	uint32_t n_ports;

	enum spa_direction direction;
	enum spa_param_port_config_mode mode;

	struct spa_video_info format;
	unsigned int have_profile:1;
	struct spa_pod *tag;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;

	struct props props;

	struct spa_io_position *io_position;

	uint64_t info_all;
	struct spa_node_info info;
#define IDX_EnumPortConfig	0
#define IDX_PortConfig		1
#define IDX_PropInfo		2
#define IDX_Props		3
#define N_NODE_PARAMS		4
	struct spa_param_info params[N_NODE_PARAMS];

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	struct dir dir[2];
};

#define CHECK_PORT(this,d,p)		((p) < this->dir[d].n_ports)

static const struct spa_dict_item node_info_items[] = {
	{ SPA_KEY_MEDIA_CLASS, "Video/Filter" },
};

static void emit_node_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;

	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		this->info.props = &SPA_DICT_INIT_ARRAY(node_info_items);
		if (this->info.change_mask & SPA_NODE_CHANGE_MASK_PARAMS) {
			SPA_FOR_EACH_ELEMENT_VAR(this->params, p) {
				if (p->user > 0) {
					p->flags ^= SPA_PARAM_INFO_SERIAL;
					p->user = 0;
				}
			}
		}
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
}

static void emit_port_info(struct impl *this, struct port *port, bool full)
{
	uint64_t old = full ? port->info.change_mask : 0;

	if (full)
		port->info.change_mask = port->info_all;
	if (port->info.change_mask) {
		struct spa_dict_item items[1];

		items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_FORMAT_DSP, "32 bit float RGBA video");
		port->info.props = &SPA_DICT_INIT(items, 1);
		spa_node_emit_port_info(&this->hooks, port->direction, port->id, &port->info);
		port->info.change_mask = old;
	}
}


static int impl_node_enum_params(void *object, int seq,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumPortConfig:
	{
		struct dir *dir;
		switch (result.index) {
		case 0:
			dir = &this->dir[SPA_DIRECTION_INPUT];;
			break;
		case 1:
			dir = &this->dir[SPA_DIRECTION_OUTPUT];;
			break;
		default:
			return 0;
		}
		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamPortConfig, id,
			SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(dir->direction),
			SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_none),
			SPA_PARAM_PORT_CONFIG_monitor,   SPA_POD_Bool(false),
			SPA_PARAM_PORT_CONFIG_control,   SPA_POD_Bool(false));
		break;
	}
	case SPA_PARAM_PortConfig:
	{
		struct dir *dir;
		struct spa_pod_frame f[1];

		switch (result.index) {
		case 0:
			dir = &this->dir[SPA_DIRECTION_INPUT];;
			break;
		case 1:
			dir = &this->dir[SPA_DIRECTION_OUTPUT];;
			break;
		default:
			return 0;
		}
		spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_ParamPortConfig, id);
		spa_pod_builder_add(&b,
			SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(dir->direction),
			SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_Id(dir->mode),
			SPA_PARAM_PORT_CONFIG_monitor,   SPA_POD_Bool(false),
			SPA_PARAM_PORT_CONFIG_control,   SPA_POD_Bool(false),
			0);

		param = spa_pod_builder_pop(&b, &f[0]);
		break;
	}
	case SPA_PARAM_PropInfo:
	{
		switch (result.index) {
		case 0:
			param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_name,   SPA_POD_String("video.convert.converter"),
				SPA_PROP_INFO_description, SPA_POD_String("Name of the used videoconverter"),
				SPA_PROP_INFO_type, SPA_POD_String("dummy"),
				SPA_PROP_INFO_params, SPA_POD_Bool(true));
			break;
		default:
			return 0;
		}
		break;
	}
	default:
		return 0;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "%p: io %d %p/%zu", this, id, data, size);

	switch (id) {
	case SPA_IO_Position:
		if (size > 0 && size < sizeof(struct spa_io_position))
			return -EINVAL;
		this->io_position = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int reconfigure_mode(struct impl *this, enum spa_param_port_config_mode mode,
		enum spa_direction direction, struct spa_video_info *info)
{
	struct dir *dir;
	uint32_t i;

	dir = &this->dir[direction];

	if (dir->have_profile && dir->mode == mode &&
	    (info == NULL || memcmp(&dir->format, info, sizeof(*info)) == 0))
		return 0;

	spa_log_info(this->log, "%p: port config direction:%d mode:%d %d %p", this,
			direction, mode, dir->n_ports, info);

	for (i = 0; i < dir->n_ports; i++) {
		spa_node_emit_port_info(&this->hooks, direction, i, NULL);
	}

	dir->have_profile = true;
	dir->mode = mode;

	switch (mode) {
	case SPA_PARAM_PORT_CONFIG_MODE_none:
		break;
	default:
		return -ENOTSUP;
	}

	this->info.change_mask |= SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PARAMS;
	this->info.flags &= ~SPA_NODE_FLAG_NEED_CONFIGURE;
	this->params[IDX_Props].user++;
	this->params[IDX_PortConfig].user++;
	return 0;
}

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if (param == NULL)
		return 0;

	switch (id) {
	case SPA_PARAM_PortConfig:
	{
		struct spa_video_info info = { 0, }, *infop = NULL;
		struct spa_pod *format = NULL;
		enum spa_direction direction;
		enum spa_param_port_config_mode mode;
		bool monitor = false, control = false;
		int res;

		if (spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamPortConfig, NULL,
				SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(&direction),
				SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(&mode),
				SPA_PARAM_PORT_CONFIG_monitor,		SPA_POD_OPT_Bool(&monitor),
				SPA_PARAM_PORT_CONFIG_control,		SPA_POD_OPT_Bool(&control),
				SPA_PARAM_PORT_CONFIG_format,		SPA_POD_OPT_Pod(&format)) < 0)
			return -EINVAL;

		if (format) {
			if (!spa_pod_is_object_type(format, SPA_TYPE_OBJECT_Format))
				return -EINVAL;

			if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
				return res;

			if (info.media_type != SPA_MEDIA_TYPE_video ||
			    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
				return -EINVAL;

			if (spa_format_video_raw_parse(format, &info.info.raw) < 0)
				return -EINVAL;

			if (info.info.raw.format == 0)
				return -EINVAL;

			infop = &info;
		}

		if ((res = reconfigure_mode(this, mode, direction, infop)) < 0)
			return res;

		emit_node_info(this, false);
		break;
	}

	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	switch (SPA_NODE_COMMAND_ID(command)) {
	default:
		return -ENOTSUP;
	}
	return 0;
}

static int
impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_trace(this->log, "%p: add listener %p", this, listener);
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);
	emit_port_info(this, &this->dir[0].ports[0], true);
	emit_port_info(this, &this->dir[1].ports[0], true);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	return -ENOTSUP;
}

static int
impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int port_enum_formats(void *object,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t index,
			     const struct spa_pod *filter,
			     struct spa_pod **param,
			     struct spa_pod_builder *builder)
{
	return -ENOTSUP;
}


static int
impl_node_port_enum_params(void *object, int seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t start, uint32_t num,
			   const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[4096];
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	spa_log_debug(this->log, "%p: enum params port %d.%d %d %u",
			this, direction, port_id, seq, id);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(this, direction, port_id,
						result.index, filter, &param, &b)) <= 0)
			return res;
		break;
	case SPA_PARAM_Meta:
		switch (result.index) {
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
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int port_set_format(struct impl *this,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	return -ENOTSUP;
}


static int
impl_node_port_set_param(void *object,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "%p: set param port %d.%d %u",
			this, direction, port_id, id);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	switch (id) {
	case SPA_PARAM_Format:
		return port_set_format(this, direction, port_id, flags, param);
	default:
		return -ENOENT;
	}
}

static int
impl_node_port_use_buffers(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	return -ENOTSUP;
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction, uint32_t port_id,
		      uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	return -ENOTSUP;

}

static int impl_node_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	return -ENOTSUP;
}

static int impl_node_process(void *object)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	return -ENOTSUP;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
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
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.process = impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Node))
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{

	spa_return_val_if_fail(handle != NULL, -EINVAL);

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
	struct dir *dir;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_log_topic_init(this->log, &log_topic);

	// props_reset(&this->props);

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);
	spa_hook_list_init(&this->hooks);

	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PROPS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_output_ports = 1;
	this->info.max_input_ports = 1;
	this->info.flags = SPA_NODE_FLAG_RT |
		SPA_NODE_FLAG_IN_PORT_CONFIG |
		SPA_NODE_FLAG_OUT_PORT_CONFIG |
		SPA_NODE_FLAG_NEED_CONFIGURE;
	this->params[IDX_EnumPortConfig] = SPA_PARAM_INFO(SPA_PARAM_EnumPortConfig, SPA_PARAM_INFO_READ);
	this->params[IDX_PortConfig] = SPA_PARAM_INFO(SPA_PARAM_PortConfig, SPA_PARAM_INFO_READWRITE);
	this->params[IDX_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[IDX_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = N_NODE_PARAMS;

	dir = &this->dir[SPA_DIRECTION_INPUT];
	dir->direction = SPA_DIRECTION_INPUT;

	dir = &this->dir[SPA_DIRECTION_OUTPUT];
	dir->direction = SPA_DIRECTION_OUTPUT;

	reconfigure_mode(this, SPA_PARAM_PORT_CONFIG_MODE_none, SPA_DIRECTION_INPUT, NULL);
	reconfigure_mode(this, SPA_PARAM_PORT_CONFIG_MODE_none, SPA_DIRECTION_OUTPUT, NULL);
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

static const struct spa_dict_item info_items[] = {
	{ SPA_KEY_FACTORY_AUTHOR, "Columbarius <co1umbarius@protonmail.com>" },
	{ SPA_KEY_FACTORY_DESCRIPTION, "Dummy video convert plugin" },
};

static const struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

const struct spa_handle_factory spa_videoconvert_dummy_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_VIDEO_CONVERT_DUMMY,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
