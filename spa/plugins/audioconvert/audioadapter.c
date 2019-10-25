/* SPA
 *
 * Copyright © 2019 Wim Taymans
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

#include <spa/support/log.h>
#include <spa/support/cpu.h>

#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/utils/names.h>
#include <spa/buffer/alloc.h>
#include <spa/pod/parser.h>
#include <spa/pod/filter.h>
#include <spa/param/param.h>
#include <spa/param/audio/format-utils.h>
#include <spa/debug/format.h>
#include <spa/debug/pod.h>

#define NAME "audioadapter"

/** \cond */

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_cpu *cpu;

	uint32_t max_align;
	enum spa_direction direction;

	struct spa_node *target;

	struct spa_node *slave;
	struct spa_hook slave_listener;
	uint32_t slave_flags;
	struct spa_audio_info slave_current_format;

	struct spa_handle *hnd_convert;
	struct spa_node *convert;
	struct spa_hook convert_listener;
	uint32_t convert_flags;

	uint32_t n_buffers;
	struct spa_buffer **buffers;

	struct spa_io_buffers io_buffers;
	struct spa_io_rate_match io_rate_match;

	uint64_t info_all;
	struct spa_node_info info;
	struct spa_param_info params[6];

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	unsigned int add_listener:1;
	unsigned int use_converter:1;
	unsigned int have_format:1;
	unsigned int started:1;
	unsigned int driver:1;
	unsigned int master:1;
};

/** \endcond */

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
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
next:
	result.index = result.next;

	spa_log_debug(this->log, NAME" %p: %d id:%u", this, seq, id);

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumPortConfig:
	case SPA_PARAM_PortConfig:
	case SPA_PARAM_PropInfo:
	case SPA_PARAM_Props:
		if ((res = spa_node_enum_params_sync(this->convert,
				id, &result.next, filter, &param, &b)) != 1)
			return res;
		break;

	case SPA_PARAM_EnumFormat:
	case SPA_PARAM_Format:
		if ((res = spa_node_port_enum_params_sync(this->slave,
				this->direction, 0,
				id, &result.next, filter, &param, &b)) != 1)
			return res;
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

static int link_io(struct impl *this)
{
	int res;

	if (!this->use_converter)
		return 0;

	spa_log_debug(this->log, NAME " %p: controls", this);

	spa_zero(this->io_rate_match);
	this->io_rate_match.rate = 1.0;

	if ((res = spa_node_port_set_io(this->slave,
			this->direction, 0,
			SPA_IO_RateMatch,
			&this->io_rate_match, sizeof(this->io_rate_match))) < 0) {
		spa_log_debug(this->log, NAME " %p: set RateMatch on slave disabled %d %s", this,
			res, spa_strerror(res));
	}
	else if ((res = spa_node_port_set_io(this->convert,
			SPA_DIRECTION_REVERSE(this->direction), 0,
			SPA_IO_RateMatch,
			&this->io_rate_match, sizeof(this->io_rate_match))) < 0) {
		spa_log_warn(this->log, NAME " %p: set RateMatch on convert failed %d %s", this,
			res, spa_strerror(res));
	}

	spa_zero(this->io_buffers);

	if ((res = spa_node_port_set_io(this->slave,
			this->direction, 0,
			SPA_IO_Buffers,
			&this->io_buffers, sizeof(this->io_buffers))) < 0) {
		spa_log_warn(this->log, NAME " %p: set Buffers on slave failed %d %s", this,
			res, spa_strerror(res));
		return res;
	}
	else if ((res = spa_node_port_set_io(this->convert,
			SPA_DIRECTION_REVERSE(this->direction), 0,
			SPA_IO_Buffers,
			&this->io_buffers, sizeof(this->io_buffers))) < 0) {
		spa_log_warn(this->log, NAME " %p: set Buffers on convert failed %d %s", this,
			res, spa_strerror(res));
		return res;
	}
	return 0;
}

static void emit_node_info(struct impl *this, bool full)
{
	if (this->add_listener)
		return;

	if (full)
		this->info.change_mask = this->info_all;

	if (this->info.change_mask) {
		struct spa_dict_item items[1];

		items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_NODE_DRIVER, this->driver ? "1" : "0");
		this->info.props = &SPA_DICT_INIT(items, 1);

		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = 0;
	}
}

static int debug_params(struct impl *this, struct spa_node *node,
                enum spa_direction direction, uint32_t port_id, uint32_t id, struct spa_pod *filter,
		const char *debug, int err)
{
        struct spa_pod_builder b = { 0 };
        uint8_t buffer[4096];
        uint32_t state;
        struct spa_pod *param;
        int res;

        spa_log_error(this->log, "params %s: %d:%d (%s) %s",
			spa_debug_type_find_name(spa_type_param, id),
			direction, port_id, debug, spa_strerror(err));

        state = 0;
        while (true) {
                spa_pod_builder_init(&b, buffer, sizeof(buffer));
                res = spa_node_port_enum_params_sync(node,
                                       direction, port_id,
                                       id, &state,
                                       NULL, &param, &b);
                if (res != 1) {
			if (res < 0)
				spa_log_error(this->log, "  error: %s", spa_strerror(res));
                        break;
		}
                spa_debug_pod(2, NULL, param);
        }

        spa_log_error(this->log, "failed filter:");
        if (filter)
                spa_debug_pod(2, NULL, filter);

        return 0;
}

static int negotiate_buffers(struct impl *this)
{
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t state;
	struct spa_pod *param;
	int res;
	bool slave_alloc, conv_alloc;
	uint32_t i, size, buffers, blocks, align, flags;
	uint32_t *aligns;
	struct spa_data *datas;
	uint32_t slave_flags, conv_flags;

	spa_log_debug(this->log, "%p: %d", this, this->n_buffers);

	if (this->n_buffers > 0)
		return 0;

	state = 0;
	param = NULL;
	if ((res = spa_node_port_enum_params_sync(this->slave,
				this->direction, 0,
				SPA_PARAM_Buffers, &state,
				param, &param, &b)) < 0) {
		debug_params(this, this->slave, this->direction, 0,
				SPA_PARAM_Buffers, param, "slave buffers", res);
		return -ENOTSUP;
	}

	state = 0;
	if ((res = spa_node_port_enum_params_sync(this->convert,
				SPA_DIRECTION_REVERSE(this->direction), 0,
				SPA_PARAM_Buffers, &state,
				param, &param, &b)) != 1) {
		debug_params(this, this->convert,
				SPA_DIRECTION_REVERSE(this->direction), 0,
				SPA_PARAM_Buffers, param, "convert buffers", res);
		return -ENOTSUP;
	}

	spa_pod_fixate(param);

	slave_flags = this->slave_flags;
	conv_flags = this->convert_flags;

	slave_alloc = SPA_FLAG_IS_SET(slave_flags, SPA_PORT_FLAG_CAN_ALLOC_BUFFERS);
	conv_alloc = SPA_FLAG_IS_SET(conv_flags, SPA_PORT_FLAG_CAN_ALLOC_BUFFERS);

	flags = 0;
	if (conv_alloc || slave_alloc) {
		flags |= SPA_BUFFER_ALLOC_FLAG_NO_DATA;
		if (conv_alloc)
			slave_alloc = false;
	}

	if ((res = spa_pod_parse_object(param,
			SPA_TYPE_OBJECT_ParamBuffers, NULL,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(&buffers),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(&blocks),
			SPA_PARAM_BUFFERS_size,    SPA_POD_Int(&size),
			SPA_PARAM_BUFFERS_align,   SPA_POD_Int(&align))) < 0)
		return res;

	spa_log_debug(this->log, "%p: buffers %d, blocks %d, size %d, align %d %d:%d",
			this, buffers, blocks, size, align, slave_alloc, conv_alloc);

	align = SPA_MAX(align, this->max_align);

	datas = alloca(sizeof(struct spa_data) * blocks);
	memset(datas, 0, sizeof(struct spa_data) * blocks);
	aligns = alloca(sizeof(uint32_t) * blocks);
	for (i = 0; i < blocks; i++) {
		datas[i].type = SPA_DATA_MemPtr;
		datas[i].flags = SPA_DATA_FLAG_DYNAMIC;
		datas[i].maxsize = size;
		aligns[i] = align;
	}

	free(this->buffers);
	this->buffers = spa_buffer_alloc_array(buffers, flags, 0, NULL, blocks, datas, aligns);
	if (this->buffers == NULL)
		return -errno;
	this->n_buffers = buffers;

	if ((res = spa_node_port_use_buffers(this->convert,
		       SPA_DIRECTION_REVERSE(this->direction), 0,
		       conv_alloc ? SPA_NODE_BUFFERS_FLAG_ALLOC : 0,
		       this->buffers, this->n_buffers)) < 0)
		return res;

	if ((res = spa_node_port_use_buffers(this->slave,
		       this->direction, 0,
		       slave_alloc ? SPA_NODE_BUFFERS_FLAG_ALLOC : 0,
		       this->buffers, this->n_buffers)) < 0)
		return res;

	return 0;
}

static int configure_format(struct impl *this, uint32_t flags, const struct spa_pod *format)
{
	int res;

	spa_log_debug(this->log, NAME "%p: configure format:", this);
	if (format && spa_log_level_enabled(this->log, SPA_LOG_LEVEL_DEBUG))
		spa_debug_format(0, NULL, format);

	if (this->use_converter) {
		if ((res = spa_node_port_set_param(this->convert,
					   SPA_DIRECTION_REVERSE(this->direction), 0,
					   SPA_PARAM_Format, flags,
					   format)) < 0)
				return res;
	}

	if ((res = spa_node_port_set_param(this->slave,
					   this->direction, 0,
					   SPA_PARAM_Format, flags,
					   format)) < 0)
			return res;

	this->have_format = format != NULL;
	if (format == NULL) {
		this->n_buffers = 0;
	} else {
		res = negotiate_buffers(this);
	}

	return res;
}

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	int res = 0;
	struct impl *this = object;
	struct spa_audio_info info = { 0 };

	spa_log_debug(this->log, NAME" %p: set param %d", this, id);

	switch (id) {
	case SPA_PARAM_Format:
		if (this->started)
			return -EIO;
		if (param == NULL)
			return -EINVAL;

		if ((res = spa_format_parse(param, &info.media_type, &info.media_subtype)) < 0)
			return res;
		if (info.media_type != SPA_MEDIA_TYPE_audio ||
			info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
				return -EINVAL;
		if (spa_format_audio_raw_parse(param, &info.info.raw) < 0)
			return -EINVAL;

		this->slave_current_format = info;
		break;

	case SPA_PARAM_PortConfig:
		if (this->started)
			return -EIO;
		if (this->target != this->slave) {
			if ((res = spa_node_set_param(this->target, id, flags, param)) < 0)
				return res;
		}
		break;

	case SPA_PARAM_Props:
		if (this->target != this->slave) {
			if ((res = spa_node_set_param(this->target, id, flags, param)) < 0)
				return res;
		}
		break;
	default:
		res = -ENOTSUP;
		break;
	}
	return res;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;
	int res = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if (this->target)
		res = spa_node_set_io(this->target, id, data, size);

	if (this->target != this->slave)
		res = spa_node_set_io(this->slave, id, data, size);

	return res;
}

static int negotiate_format(struct impl *this)
{
	uint32_t state;
	struct spa_pod *format;
	uint8_t buffer[4096];
	struct spa_pod_builder b = { 0 };
	int res;

	if (this->have_format)
		return 0;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	spa_log_debug(this->log, NAME "%p: negiotiate", this);

	state = 0;
	format = NULL;

	if (this->have_format)
		format = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &this->slave_current_format.info.raw);

	if ((res = spa_node_port_enum_params_sync(this->slave,
				this->direction, 0,
				SPA_PARAM_EnumFormat, &state,
				format, &format, &b)) < 0) {
		debug_params(this, this->slave, this->direction, 0,
				SPA_PARAM_EnumFormat, format, "slave format", res);
		return -ENOTSUP;
	}

	if (this->use_converter) {
		state = 0;
		if ((res = spa_node_port_enum_params_sync(this->convert,
					SPA_DIRECTION_REVERSE(this->direction), 0,
					SPA_PARAM_EnumFormat, &state,
					format, &format, &b)) != 1) {
			debug_params(this, this->convert,
					SPA_DIRECTION_REVERSE(this->direction), 0,
					SPA_PARAM_EnumFormat, format, "convert format", res);
			return -ENOTSUP;
		}
	}

	spa_pod_fixate(format);

	res = configure_format(this, 0, format);

	return res;
}


static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, NAME " %p: command %d", this, SPA_NODE_COMMAND_ID(command));

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if ((res = negotiate_format(this)) < 0)
			return res;
		if ((res = negotiate_buffers(this)) < 0)
			return res;
		this->started = true;
		break;
	case SPA_NODE_COMMAND_Suspend:
		configure_format(this, 0, NULL);
		/* fallthrough */
	case SPA_NODE_COMMAND_Pause:
		this->started = false;
		break;
	default:
		break;
	}

	if ((res = spa_node_send_command(this->target, command)) < 0) {
		spa_log_error(this->log, NAME " %p: can't send command: %s",
				this, spa_strerror(res));
		return res;
	}

	if (this->target != this->slave) {
		if ((res = spa_node_send_command(this->slave, command)) < 0) {
			spa_log_error(this->log, NAME " %p: can't send command: %s",
					this, spa_strerror(res));
			return res;
		}
	}
	return res;
}

static void convert_node_info(void *data, const struct spa_node_info *info)
{
	struct impl *this = data;
	uint32_t i;

	for (i = 0; i < info->n_params; i++) {
		uint32_t idx = SPA_ID_INVALID;

		switch (info->params[i].id) {
		case SPA_PARAM_PropInfo:
			idx = 1;
			break;
		case SPA_PARAM_Props:
			idx = 2;
			break;
		}
		if (idx != SPA_ID_INVALID) {
			this->params[idx] = info->params[i];
			this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
		}
	}
	emit_node_info(this, false);
}

static void convert_port_info(void *data,
		enum spa_direction direction, uint32_t port_id,
		const struct spa_port_info *info)
{
	struct impl *this = data;

	if (direction != this->direction) {
		if (port_id == 0)
			return;
		else
			port_id--;
	}

	spa_log_trace(this->log, NAME" %p: port info %d:%d", this,
			direction, port_id);

	spa_node_emit_port_info(&this->hooks, direction, port_id, info);
}

static void convert_result(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct impl *this = data;
	spa_log_trace(this->log, NAME" %p: result %d %d", this, seq, res);
	spa_node_emit_result(&this->hooks, seq, res, type, result);
}

static const struct spa_node_events convert_node_events = {
	SPA_VERSION_NODE_EVENTS,
	.info = convert_node_info,
	.port_info = convert_port_info,
	.result = convert_result,
};

static void slave_info(void *data, const struct spa_node_info *info)
{
	struct impl *this = data;
	const char *str;

	if (info->max_input_ports > 0)
		this->direction = SPA_DIRECTION_INPUT;
        else
		this->direction = SPA_DIRECTION_OUTPUT;

	this->info.max_input_ports = this->direction == SPA_DIRECTION_INPUT ? 128 : 0;
	this->info.max_output_ports = this->direction == SPA_DIRECTION_OUTPUT ? 128 : 0;

	spa_log_debug(this->log, NAME" %p: slave info %s", this,
			this->direction == SPA_DIRECTION_INPUT ?
				"Input" : "Output");

	if (info->props) {
		if ((str = spa_dict_lookup(info->props, SPA_KEY_NODE_DRIVER)) != NULL) {
			this->driver = strcmp(str, "true") == 0 || atoi(str) == 1;
			this->info.change_mask |= SPA_NODE_CHANGE_MASK_PROPS;
		}
	}
}

static void slave_port_info(void *data,
		enum spa_direction direction, uint32_t port_id,
		const struct spa_port_info *info)
{
	struct impl *this = data;
	uint32_t i;

	for (i = 0; i < info->n_params; i++) {
		uint32_t idx = SPA_ID_INVALID;

		switch (info->params[i].id) {
		case SPA_PARAM_Format:
			idx = 3;
			break;
		}
		if (idx != SPA_ID_INVALID) {
			this->params[idx] = info->params[i];
			this->info.change_mask |= SPA_NODE_CHANGE_MASK_PARAMS;
		}
	}
	emit_node_info(this, false);
}

static const struct spa_node_events slave_node_events = {
	SPA_VERSION_NODE_EVENTS,
	.info = slave_info,
	.port_info = slave_port_info,
};

static int slave_ready(void *data, int status)
{
	struct impl *this = data;

	spa_log_trace(this->log, NAME " %p: ready %d", this, status);

	this->master = true;

	if (this->direction == SPA_DIRECTION_OUTPUT)
		status = spa_node_process(this->convert);

	return spa_node_call_ready(&this->callbacks, status);
}

static int slave_reuse_buffer(void *data, uint32_t port_id, uint32_t buffer_id)
{
	int res;
	struct impl *this = data;

	if (this->use_converter)
		res = spa_node_port_reuse_buffer(this->convert, port_id, buffer_id);
	else
		res = spa_node_call_reuse_buffer(&this->callbacks, port_id, buffer_id);

	return res;
}

static int slave_xrun(void *data, uint64_t trigger, uint64_t delay, struct spa_pod *info)
{
	struct impl *this = data;
	return spa_node_call_xrun(&this->callbacks, trigger, delay, info);
}

static const struct spa_node_callbacks slave_node_callbacks = {
	SPA_VERSION_NODE_CALLBACKS,
	.ready = slave_ready,
	.reuse_buffer = slave_reuse_buffer,
	.xrun = slave_xrun,
};

static int impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *this = object;
	struct spa_hook l;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_trace(this->log, NAME" %p: add listener %p", this, listener);
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	this->add_listener = true;

	if (this->use_converter) {
		spa_zero(l);
		spa_node_add_listener(this->convert, &l, &convert_node_events, this);
		spa_hook_remove(&l);
	}

	this->add_listener = false;

	emit_node_info(this, true);

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

static int
impl_node_sync(void *object, int seq)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	return spa_node_sync(this->slave, seq);
}

static int
impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if (direction != this->direction)
		return -EINVAL;

	return spa_node_add_port(this->target, direction, port_id, props);
}

static int
impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if (direction != this->direction)
		return -EINVAL;

	return spa_node_remove_port(this->target, direction, port_id);
}

static int
impl_node_port_enum_params(void *object, int seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t start, uint32_t num,
			   const struct spa_pod *filter)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	if (direction != this->direction)
		port_id++;

	spa_log_debug(this->log, NAME" %p: %d %u", this, seq, id);

	return spa_node_port_enum_params(this->target, seq, direction, port_id, id,
			start, num, filter);
}

static int
impl_node_port_set_param(void *object,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, " %d %d %d %d", port_id, id, direction, this->direction);

	if (direction != this->direction)
		port_id++;

	if ((res = spa_node_port_set_param(this->target, direction, port_id, id,
			flags, param)) < 0)
		return res;

	return res;
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_log_debug(this->log, "set io %d %d %d %d", port_id, id, direction, this->direction);

	if (direction != this->direction)
		port_id++;

	return spa_node_port_set_io(this->target, direction, port_id, id, data, size);
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
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if (direction != this->direction)
		port_id++;

	spa_log_debug(this->log, NAME" %p: %d %d:%d", this,
			n_buffers, direction, port_id);

	if ((res = spa_node_port_use_buffers(this->target,
					direction, port_id, flags, buffers, n_buffers)) < 0)
		return res;

	return res;
}

static int
impl_node_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	return spa_node_port_reuse_buffer(this->target, port_id, buffer_id);
}

static int impl_node_process(void *object)
{
	struct impl *this = object;
	int status;

	spa_log_trace_fp(this->log, "%p: process convert:%u master:%d",
			this, this->use_converter, this->master);

	if (this->direction == SPA_DIRECTION_INPUT) {
		if (this->use_converter)
			status = spa_node_process(this->convert);
	}

	status = spa_node_process(this->slave);

	if (this->direction == SPA_DIRECTION_OUTPUT && !this->master) {
		if (this->use_converter)
			status = spa_node_process(this->convert);
	}
	this->master = false;

	return status;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.sync = impl_node_sync,
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

	spa_hook_remove(&this->slave_listener);
	spa_node_set_callbacks(this->slave, NULL, NULL);

	if (this->buffers)
		free(this->buffers);
	this->buffers = NULL;

	return 0;
}

static int configure_adapt(struct impl *this)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_pod *param;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	spa_log_debug(this->log, "%p: configure convert %p", this, this->target);

	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
		SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(this->direction),
		SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp));

	return spa_node_set_param(this->target, SPA_PARAM_PortConfig, 0, param);
}

extern const struct spa_handle_factory spa_audioconvert_factory;

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	size_t size;

	size = spa_handle_factory_get_size(&spa_audioconvert_factory, params);
	size += sizeof(struct impl);

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
	void *iface;
	const char *str;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	for (i = 0; i < n_support; i++) {
		switch (support[i].type) {
		case SPA_TYPE_INTERFACE_Log:
			this->log = support[i].data;
			break;
		case SPA_TYPE_INTERFACE_CPU:
			this->cpu = support[i].data;
			break;
		}
	}
	if (info == NULL || (str = spa_dict_lookup(info, "audio.adapt.slave")) == NULL)
		return -EINVAL;

	sscanf(str, "pointer:%p", &this->slave);
	if (this->slave == NULL)
		return -EINVAL;

	if (this->cpu)
		this->max_align = spa_cpu_get_max_align(this->cpu);

	spa_hook_list_init(&this->hooks);

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);

	this->hnd_convert = SPA_MEMBER(this, sizeof(struct impl), struct spa_handle);
	spa_handle_factory_init(&spa_audioconvert_factory,
				this->hnd_convert,
				info, support, n_support);

	spa_handle_get_interface(this->hnd_convert, SPA_TYPE_INTERFACE_Node, &iface);
	this->convert = iface;
	this->target = this->convert;

	this->info_all = SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	this->params[1] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[2] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	this->params[4] = SPA_PARAM_INFO(SPA_PARAM_EnumPortConfig, SPA_PARAM_INFO_READ);
	this->params[5] = SPA_PARAM_INFO(SPA_PARAM_PortConfig, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = 6;

	spa_node_add_listener(this->slave,
			&this->slave_listener, &slave_node_events, this);
	spa_node_set_callbacks(this->slave, &slave_node_callbacks, this);

	spa_node_add_listener(this->convert,
			&this->convert_listener, &convert_node_events, this);
	this->use_converter = true;

	configure_adapt(this);

	link_io(this);

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

const struct spa_handle_factory spa_audioadapter_factory = {
	.version = SPA_VERSION_HANDLE_FACTORY,
	.name = SPA_NAME_AUDIO_ADAPT,
	.get_size = impl_get_size,
	.init = impl_init,
	.enum_interface_info = impl_enum_interface_info,
};
