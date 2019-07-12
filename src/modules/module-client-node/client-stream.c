/* PipeWire
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

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/eventfd.h>

#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/utils/names.h>
#include <spa/buffer/alloc.h>
#include <spa/pod/parser.h>
#include <spa/pod/filter.h>
#include <spa/param/audio/format-utils.h>

#include "pipewire/pipewire.h"
#include "pipewire/interfaces.h"
#include "pipewire/control.h"
#include "pipewire/private.h"

#include "modules/spa/spa-node.h"
#include "client-node.h"
#include "client-stream.h"

#include "pipewire/core.h"

#define NAME "client-stream"

#undef spa_debug

#include <spa/debug/pod.h>
#include <spa/debug/format.h>

/** \cond */

struct node {
	struct spa_node node;

	struct impl *impl;

	struct spa_log *log;

	uint64_t info_all;
	struct spa_node_info info;
	struct spa_param_info params[5];

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;
};

struct impl {
	struct pw_client_stream this;

	struct pw_core *core;

	struct node node;
	bool started;
	bool active;
	bool driver;

	struct spa_hook node_listener;
	struct spa_hook client_node_listener;

	enum spa_direction direction;

	struct spa_node *cnode;
	struct spa_handle *handle;
	struct spa_node *adapter;
	struct spa_hook adapter_listener;
	struct spa_node *adapter_mix;
	uint32_t adapter_mix_flags;
	uint32_t adapter_mix_port;

	bool use_converter;

	struct pw_client_node *client_node;
	struct pw_port *client_port;
	struct pw_port_mix client_port_mix;

	struct spa_io_buffers *io;

	struct spa_buffer **buffers;
	uint32_t n_buffers;
	struct pw_memblock *mem;

	struct pw_control_link control;
	struct pw_control_link notify;
};

/** \endcond */

static int impl_node_enum_params(void *object, int seq,
				 uint32_t id, uint32_t start, uint32_t num,
				 const struct spa_pod *filter)
{
	struct node *this = object;
	struct impl *impl;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	impl = this->impl;

	result.id = id;
	result.next = start;
next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_PropInfo:
	case SPA_PARAM_Props:
		if (impl->adapter == impl->cnode)
			return 0;

		if ((res = spa_node_enum_params_sync(impl->adapter,
				id, &start, filter, &param, &b)) != 1)
			return res;
		break;

	case SPA_PARAM_EnumFormat:
	case SPA_PARAM_Format:
		if ((res = spa_node_port_enum_params_sync(impl->cnode,
				impl->direction, 0,
				id, &start, filter, &param, &b)) != 1)
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

static void try_link_controls(struct impl *impl)
{
	struct pw_control *cin, *cout;
	struct pw_port *target, *port;
	int res;

	if (!impl->use_converter)
		return;

	if (impl->control.valid || impl->notify.valid)
		return;

	target = pw_node_find_port(impl->this.node, impl->direction, 0);

	if (target == NULL) {
		pw_log_warn(NAME " %p: can't link controls", &impl->this);
		return;
	}

	port = impl->client_port;

	pw_log_debug(NAME " %p: trying controls", impl);
	spa_list_for_each(cout, &port->control_list[SPA_DIRECTION_OUTPUT], port_link) {
		spa_list_for_each(cin, &target->control_list[SPA_DIRECTION_INPUT], port_link) {
			if ((res = pw_control_add_link(cout, 0, cin, 0, &impl->control)) < 0)
				pw_log_error("failed to link controls: %s", spa_strerror(res));
			break;
		}
	}
	spa_list_for_each(cin, &port->control_list[SPA_DIRECTION_INPUT], port_link) {
		spa_list_for_each(cout, &target->control_list[SPA_DIRECTION_OUTPUT], port_link) {
			if ((res = pw_control_add_link(cout, 0, cin, 0, &impl->notify)) < 0)
				pw_log_error("failed to link controls: %s", spa_strerror(res));
			break;
		}
	}
}

static void emit_node_info(struct node *this, bool full)
{
	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = 0;
	}
}

static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	int res = 0;
	struct node *this = object;
	struct impl *impl;

	impl = this->impl;

	switch (id) {
	case SPA_PARAM_Profile:
		if (impl->started)
			return -EIO;
		pw_log_debug("set profile %d", id);
		if (impl->adapter && impl->adapter != impl->cnode) {
			if ((res = spa_node_set_param(impl->adapter, id, flags, param)) < 0)
				return res;

			try_link_controls(impl);
		}
		break;
	case SPA_PARAM_Props:
		if (impl->adapter && impl->adapter != impl->cnode) {
			if ((res = spa_node_set_param(impl->adapter, id, flags, param)) < 0)
				return res;

			this->info.change_mask = SPA_NODE_CHANGE_MASK_PARAMS;
			this->params[2].flags ^= SPA_PARAM_INFO_SERIAL;
			emit_node_info(this, false);
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
	struct node *this = object;
	struct impl *impl;
	int res = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	impl = this->impl;

	if (impl->adapter)
		res = spa_node_set_io(impl->adapter, id, data, size);

	if (impl->cnode && impl->adapter != impl->cnode) {
		res = spa_node_set_io(impl->cnode, id, data, size);
	}
	return res;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct node *this = object;
	struct impl *impl;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	impl = this->impl;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		impl->started = true;
		break;
	case SPA_NODE_COMMAND_Pause:
		impl->started = false;
		break;
	default:
		break;
	}

	if ((res = spa_node_send_command(impl->adapter, command)) < 0)
		return res;

	if (impl->adapter != impl->cnode) {
		if ((res = spa_node_send_command(impl->cnode, command)) < 0)
			return res;
	}
	return res;
}

static void adapter_port_info(void *data,
		enum spa_direction direction, uint32_t port_id,
		const struct spa_port_info *info)
{
	struct impl *impl = data;
	struct node *this = &impl->node;

	if (direction == impl->direction) {
		struct spa_port_info i = *info;
		SPA_FLAG_UNSET(i.flags, SPA_PORT_FLAG_DYNAMIC_DATA);
		spa_node_emit_port_info(&this->hooks, direction, port_id, &i);
	}
}

static void adapter_result(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct impl *impl = data;
	struct node *this = &impl->node;
	pw_log_trace("%p: result %d %d", this, seq, res);
	spa_node_emit_result(&this->hooks, seq, res, type, result);
}

static const struct spa_node_events adapter_node_events = {
	SPA_VERSION_NODE_EVENTS,
	.port_info = adapter_port_info,
	.result = adapter_result,
};

static int impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct node *this = object;
	struct impl *impl;
	struct spa_hook l;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	impl = this->impl;

	pw_log_debug("%p: add listener %p", this, listener);
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);

	if (impl->adapter && impl->adapter != impl->cnode) {
		spa_zero(l);
		spa_node_add_listener(impl->adapter, &l, &adapter_node_events, impl);
		spa_hook_remove(&l);
	}

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct node *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);


	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int
impl_node_sync(void *object, int seq)
{
	struct node *this = object;
	struct impl *impl;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	impl = this->impl;

	return spa_node_sync(impl->cnode, seq);
}

static int
impl_node_add_port(void *object, enum spa_direction direction, uint32_t port_id,
		const struct spa_dict *props)
{
	struct node *this = object;
	struct impl *impl;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	impl = this->impl;

	if (direction != impl->direction)
		return -EINVAL;

	if ((res = spa_node_add_port(impl->adapter_mix, direction, port_id, props)) < 0)
		return res;

	return res;
}

static int
impl_node_remove_port(void *object, enum spa_direction direction, uint32_t port_id)
{
	struct node *this = object;
	struct impl *impl;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	impl = this->impl;

	if (direction != this->impl->direction)
		return -EINVAL;

	return spa_node_remove_port(impl->adapter_mix, direction, port_id);
}

static int
impl_node_port_enum_params(void *object, int seq,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t start, uint32_t num,
			   const struct spa_pod *filter)
{
	struct node *this = object;
	struct impl *impl;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	impl = this->impl;

	if (direction != impl->direction)
		return -EINVAL;

	pw_log_debug("%p: %d %u", this, seq, id);

	return spa_node_port_enum_params(impl->adapter, seq, direction, port_id, id,
			start, num, filter);
}

static int debug_params(struct impl *impl, struct spa_node *node,
                enum spa_direction direction, uint32_t port_id, uint32_t id, struct spa_pod *filter)
{
	struct node *this = &impl->node;
        struct spa_pod_builder b = { 0 };
        uint8_t buffer[4096];
        uint32_t state;
        struct spa_pod *param;
        int res;

        spa_log_error(this->log, "params %s:", spa_debug_type_find_name(spa_type_param, id));

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


static int negotiate_format(struct impl *impl)
{
	struct node *this = &impl->node;
	uint32_t state;
	struct spa_pod *format;
	uint8_t buffer[4096];
	struct spa_pod_builder b = { 0 };
	int res;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	spa_log_debug(this->log, NAME "%p: negiotiate", impl);

	state = 0;
	if ((res = spa_node_port_enum_params_sync(impl->adapter_mix,
				SPA_DIRECTION_REVERSE(impl->direction),
				impl->adapter_mix_port,
				SPA_PARAM_EnumFormat, &state,
				NULL, &format, &b)) != 1) {
		debug_params(impl, impl->adapter_mix,
				SPA_DIRECTION_REVERSE(impl->direction),
				impl->adapter_mix_port,
				SPA_PARAM_EnumFormat, NULL);
		return -ENOTSUP;
	}

	state = 0;
	if ((res = spa_node_port_enum_params_sync(impl->cnode,
				impl->direction, 0,
				SPA_PARAM_EnumFormat, &state,
				format, &format, &b)) != 1) {
		debug_params(impl, impl->cnode, impl->direction, 0,
				SPA_PARAM_EnumFormat, format);
		return -ENOTSUP;
	}

	spa_pod_fixate(format);
	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_format(0, NULL, format);

	if ((res = spa_node_port_set_param(impl->adapter_mix,
				   SPA_DIRECTION_REVERSE(impl->direction),
				   impl->adapter_mix_port,
				   SPA_PARAM_Format, 0,
				   format)) < 0)
			return res;

	if ((res = spa_node_port_set_param(impl->cnode,
					   impl->direction, 0,
					   SPA_PARAM_Format, 0,
					   format)) < 0)
			return res;

	return res;
}

static int negotiate_buffers(struct impl *impl)
{
	struct node *this = &impl->node;
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t state;
	struct spa_pod *param = NULL;
	int res, i;
	bool in_alloc, out_alloc;
	int32_t size, buffers, blocks, align, flags;
	uint32_t *aligns;
	struct spa_data *datas;
	uint32_t in_flags, out_flags;
        struct spa_buffer_alloc_info info = { 0, };
        void *skel;

	spa_log_debug(this->log, "%p: %d", impl, impl->n_buffers);

	if (impl->n_buffers > 0)
		return 0;

	state = 0;
	if ((res = spa_node_port_enum_params_sync(impl->adapter_mix,
				SPA_DIRECTION_REVERSE(impl->direction),
				impl->adapter_mix_port,
				SPA_PARAM_Buffers, &state,
				param, &param, &b)) != 1) {
		debug_params(impl, impl->adapter_mix,
				SPA_DIRECTION_REVERSE(impl->direction),
				impl->adapter_mix_port,
				SPA_PARAM_Buffers, param);
		return -ENOTSUP;
	}
	if (res != 1)
		param = NULL;

	state = 0;
	if ((res = spa_node_port_enum_params_sync(impl->cnode,
				impl->direction, 0,
				SPA_PARAM_Buffers, &state,
				param, &param, &b)) < 0) {
		debug_params(impl, impl->cnode, impl->direction, 0,
				SPA_PARAM_Buffers, param);
		return res;
	}

	spa_pod_fixate(param);

	in_flags = impl->client_port->spa_flags;
	out_flags = impl->adapter_mix_flags;

	in_alloc = SPA_FLAG_CHECK(in_flags, SPA_PORT_FLAG_CAN_ALLOC_BUFFERS);
	out_alloc = SPA_FLAG_CHECK(out_flags, SPA_PORT_FLAG_CAN_ALLOC_BUFFERS);

	flags = 0;
	if (out_alloc || in_alloc) {
		flags |= SPA_BUFFER_ALLOC_FLAG_NO_DATA;
		if (out_alloc)
			in_alloc = false;
	}

	if ((res = spa_pod_parse_object(param,
			SPA_TYPE_OBJECT_ParamBuffers, NULL,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(&buffers),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(&blocks),
			SPA_PARAM_BUFFERS_size,    SPA_POD_Int(&size),
			SPA_PARAM_BUFFERS_align,   SPA_POD_Int(&align))) < 0)
		return res;

	spa_log_debug(this->log, "%p: buffers %d, blocks %d, size %d, align %d",
			impl, buffers, blocks, size, align);

	datas = alloca(sizeof(struct spa_data) * blocks);
	memset(datas, 0, sizeof(struct spa_data) * blocks);
	aligns = alloca(sizeof(uint32_t) * blocks);
	for (i = 0; i < blocks; i++) {
		datas[i].type = SPA_DATA_MemPtr;
		datas[i].flags = SPA_DATA_FLAG_DYNAMIC;
		datas[i].maxsize = size;
		aligns[i] = align;
	}

	spa_buffer_alloc_fill_info(&info, 0, NULL, blocks, datas, aligns);

	free(impl->buffers);
	impl->buffers = calloc(buffers, sizeof(struct spa_buffer *) + info.skel_size);
	if (impl->buffers == NULL)
		return -errno;

        skel = SPA_MEMBER(impl->buffers, sizeof(struct spa_buffer *) * buffers, void);

	if (impl->mem) {
		pw_memblock_free(impl->mem);
		impl->mem = NULL;
	}

	if ((res = pw_memblock_alloc(PW_MEMBLOCK_FLAG_WITH_FD |
				     PW_MEMBLOCK_FLAG_MAP_READWRITE |
				     PW_MEMBLOCK_FLAG_SEAL, buffers * info.mem_size,
				     &impl->mem)) < 0)
		return res;

	impl->n_buffers = buffers;

        spa_buffer_alloc_layout_array(&info, impl->n_buffers, impl->buffers,
			skel, impl->mem->ptr);

	if (in_alloc) {
		if ((res = spa_node_port_alloc_buffers(impl->adapter_mix,
			       SPA_DIRECTION_REVERSE(impl->direction),
			       impl->adapter_mix_port,
			       NULL, 0,
			       impl->buffers, &impl->n_buffers)) < 0)
			return res;
	}
	else {
		if ((res = spa_node_port_use_buffers(impl->adapter_mix,
			       SPA_DIRECTION_REVERSE(impl->direction),
			       impl->adapter_mix_port,
			       impl->buffers, impl->n_buffers)) < 0)
			return res;
	}
	if (out_alloc) {
		if ((res = spa_node_port_alloc_buffers(impl->client_port->mix,
			       impl->direction, 0,
			       NULL, 0,
			       impl->buffers, &impl->n_buffers)) < 0)
			return res;
	}
	else {
		if ((res = spa_node_port_use_buffers(impl->client_port->mix,
			       impl->direction, 0,
			       impl->buffers, impl->n_buffers)) < 0)
			return res;
	}

	return 0;
}

static int
impl_node_port_set_param(void *object,
			 enum spa_direction direction, uint32_t port_id,
			 uint32_t id, uint32_t flags,
			 const struct spa_pod *param)
{
	struct node *this = object;
	struct impl *impl;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	impl = this->impl;

	if (direction != impl->direction)
		return -EINVAL;

	if ((res = spa_node_port_set_param(impl->adapter_mix, direction, port_id, id,
			flags, param)) < 0)
		return res;

	if (id == SPA_PARAM_Format && impl->use_converter) {
		if (param == NULL) {
			if ((res = spa_node_port_set_param(impl->adapter_mix,
					SPA_DIRECTION_REVERSE(direction),
					impl->adapter_mix_port,
					id, 0, NULL)) < 0)
				return res;
			impl->n_buffers = 0;
		}
		else {
			if (port_id == 0)
				res = negotiate_format(impl);
		}
	}
	return res;
}

static int
impl_node_port_set_io(void *object,
		      enum spa_direction direction,
		      uint32_t port_id,
		      uint32_t id,
		      void *data, size_t size)
{
	struct node *this = object;
	struct impl *impl;
	int res = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	impl = this->impl;

	spa_log_debug(this->log, "set io %d %d %d %d", port_id, id, direction, impl->direction);

	if (impl->use_converter) {
		if (direction != impl->direction && id != SPA_IO_Control)
			return -EINVAL;
		res = spa_node_port_set_io(impl->adapter_mix, direction, port_id, id, data, size);
	}
	else {
		if (direction != impl->direction)
			return -EINVAL;
		if (id == SPA_IO_Buffers && size >= sizeof(struct spa_io_buffers))
			impl->io = data;
	}
	return res;
}

static int
impl_node_port_use_buffers(void *object,
			   enum spa_direction direction,
			   uint32_t port_id,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct node *this = object;
	struct impl *impl;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	impl = this->impl;

	if (direction != impl->direction)
		return -EINVAL;

	if ((res = spa_node_port_use_buffers(impl->adapter_mix,
					direction, port_id, buffers, n_buffers)) < 0)
		return res;


	spa_log_debug(this->log, "%p: %d %d", impl, n_buffers, port_id);

	if (n_buffers > 0 && impl->use_converter) {
		if (port_id == 0)
			res = negotiate_buffers(impl);
	}
	return res;
}

static int
impl_node_port_alloc_buffers(void *object,
			     enum spa_direction direction,
			     uint32_t port_id,
			     struct spa_pod **params,
			     uint32_t n_params,
			     struct spa_buffer **buffers,
			     uint32_t *n_buffers)
{
	struct node *this = object;
	struct impl *impl;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	impl = this->impl;

	if (direction != impl->direction)
		return -EINVAL;

	return spa_node_port_alloc_buffers(impl->adapter_mix, direction, port_id,
			params, n_params, buffers, n_buffers);
}

static int
impl_node_port_reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	struct node *this = object;
	struct impl *impl;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	impl = this->impl;

	return spa_node_port_reuse_buffer(impl->adapter, port_id, buffer_id);
}

static int impl_node_process(void *object)
{
	struct node *this = object;
	struct impl *impl = this->impl;
	struct spa_io_position *q = impl->this.node->driver_node->rt.position;
	int status, trigger;

	if (impl->driver) {
		impl->client_node->node->rt.target.signal(impl->client_node->node->rt.target.data);
		return SPA_STATUS_OK;
	}

	if (!impl->active)
		return SPA_STATUS_HAVE_BUFFER;

	spa_log_trace_fp(this->log, "%p: process %zd", this, q->size * sizeof(float));

	if (impl->use_converter) {
		status = spa_node_process(impl->adapter);
	}
	else {
		struct spa_io_buffers tmp;

		spa_log_trace_fp(this->log, "%p: process %d/%d %d/%d", this,
				impl->io->status, impl->io->buffer_id,
				impl->client_port_mix.io->status,
				impl->client_port_mix.io->buffer_id);

		tmp = *impl->io;
		*impl->io = *impl->client_port_mix.io;
		*impl->client_port_mix.io = tmp;

		status = impl->client_port_mix.io->status | impl->io->status;
	}
	spa_log_trace_fp(this->log, "%p: process %d", this, status);

	if (impl->direction == SPA_DIRECTION_OUTPUT) {
		if (!(status & SPA_STATUS_HAVE_BUFFER))
			spa_log_warn(this->log, "%p: '%s' id:%d underrun",
					this, impl->this.node->info.name, impl->this.node->info.id);
		trigger = status & SPA_STATUS_NEED_BUFFER;
	}
	else
		trigger = status & SPA_STATUS_HAVE_BUFFER;

	if (trigger && !impl->this.node->driver)
		impl->client_node->node->rt.target.signal(impl->client_node->node->rt.target.data);

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
	.port_alloc_buffers = impl_node_port_alloc_buffers,
	.port_set_io = impl_node_port_set_io,
	.port_reuse_buffer = impl_node_port_reuse_buffer,
	.process = impl_node_process,
};

static int
node_init(struct node *this,
	  struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	uint32_t i;

	for (i = 0; i < n_support; i++) {
		if (support[i].type == SPA_TYPE_INTERFACE_Log)
			this->log = support[i].data;
	}
	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);
	spa_hook_list_init(&this->hooks);

	this->info_all = SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = 0;
	this->info.max_output_ports = 0;
	this->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	this->params[1] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[2] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READ);
	this->params[4] = SPA_PARAM_INFO(SPA_PARAM_Profile, SPA_PARAM_INFO_WRITE);
	this->info.params = this->params;
	this->info.n_params = 5;

	return 0;
}

static int do_port_info(void *data, struct pw_port *port)
{
	struct impl *impl = data;
	struct node *node = &impl->node;
	struct spa_port_info info;

	info = SPA_PORT_INFO_INIT();
	info.change_mask = SPA_PORT_CHANGE_MASK_FLAGS | SPA_PORT_CHANGE_MASK_PROPS;
	info.flags = port->spa_flags;
	info.props = &port->properties->dict;

	spa_node_emit_port_info(&node->hooks,
			impl->direction, port->port_id, &info);
	return 0;
}

static void emit_port_info(struct impl *impl)
{
	pw_node_for_each_port(impl->client_node->node,
			impl->direction,
			do_port_info, impl);
}

static void client_node_initialized(void *data)
{
	struct impl *impl = data;
        uint32_t state;
	uint32_t media_type, media_subtype;
	struct spa_pod *format;
	uint8_t buffer[4096];
	struct spa_pod_builder b;
	int res;
	const struct pw_properties *props;
	const char *str, *dir, *type;
	char media_class[64];
	bool exclusive, monitor;
	struct spa_dict_item items[1];
	const struct pw_node_info *info;

	pw_log_debug(NAME " %p: initialized", &impl->this);

	info = pw_node_get_info(impl->client_node->node);
	if (info == NULL)
		return;

	if (info->n_output_ports == 0) {
		impl->direction = SPA_DIRECTION_INPUT;
		dir = "Input";
	}
	else {
		impl->direction = SPA_DIRECTION_OUTPUT;
		dir = "Output";
	}

	pw_log_debug(NAME " %p: in %d/%d out %d/%d -> %s", &impl->this,
			info->n_input_ports, info->max_input_ports,
			info->n_output_ports, info->max_output_ports,
			dir);

	props = pw_node_get_properties(impl->client_node->node);
	if (props != NULL && (str = pw_properties_get(props, PW_KEY_NODE_EXCLUSIVE)) != NULL)
		exclusive = pw_properties_parse_bool(str);
	else
		exclusive = false;

	if (props != NULL && (str = pw_properties_get(props, PW_KEY_STREAM_MONITOR)) != NULL)
		monitor = pw_properties_parse_bool(str);
	else
		monitor = false;

	impl->client_node->node->driver_node = impl->this.node;

	impl->client_port = pw_node_find_port(impl->client_node->node, impl->direction, 0);
	if (impl->client_port == NULL)
		return;

	if ((res = pw_port_init_mix(impl->client_port, &impl->client_port_mix)) < 0)
		return;

	if ((res = spa_node_port_set_io(impl->client_port->mix,
				impl->direction, 0,
				SPA_IO_Buffers,
				impl->client_port_mix.io,
				sizeof(impl->client_port_mix.io))) < 0)
		return;

	state = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	if ((res = spa_node_port_enum_params_sync(impl->cnode,
				impl->direction, 0,
				SPA_PARAM_EnumFormat, &state,
				NULL, &format, &b)) != 1) {
		pw_log_warn(NAME " %p: no format given", &impl->this);
		impl->adapter = impl->cnode;
		impl->adapter_mix = impl->client_port->mix;
		impl->adapter_mix_port = 0;
		impl->adapter_mix_flags = impl->client_port->spa_flags;
		impl->use_converter = false;
		emit_port_info(impl);
		return;
	}

	if (spa_format_parse(format, &media_type, &media_subtype) < 0)
		return;

	pw_log_debug(NAME " %p: %s/%s", &impl->this,
			spa_debug_type_find_name(spa_type_media_type, media_type),
			spa_debug_type_find_name(spa_type_media_subtype, media_subtype));

	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_format(2, NULL, format);

	if (!exclusive &&
	    media_type == SPA_MEDIA_TYPE_audio &&
	    media_subtype == SPA_MEDIA_SUBTYPE_raw) {
		struct spa_dict_item items[3];
		uint32_t n_items;
		const char *mode;
		void *iface;

		if (impl->direction == SPA_DIRECTION_OUTPUT)
			mode = "split";
		else
			mode = "merge";

		n_items = 0;
		items[n_items++] = SPA_DICT_ITEM_INIT("factory.mode", mode);
		items[n_items++] = SPA_DICT_ITEM_INIT("resample.peaks", monitor ? "1" : "0");
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_LIBRARY_NAME, "audioconvert/libspa-audioconvert");

		if ((impl->handle = pw_core_load_spa_handle(impl->core,
						SPA_NAME_AUDIO_CONVERT,
						&SPA_DICT_INIT(items, n_items))) == NULL)
			return;

		if ((res = spa_handle_get_interface(impl->handle,
				SPA_TYPE_INTERFACE_Node, &iface)) < 0)
			return;

		impl->adapter = iface;
		impl->adapter_mix = impl->adapter;
		impl->adapter_mix_port = 0;
		impl->use_converter = true;
		spa_node_add_listener(impl->adapter, &impl->adapter_listener,
				&adapter_node_events, impl);
	}
	else {
		impl->adapter = impl->cnode;
		impl->adapter_mix = impl->client_port->mix;
		impl->adapter_mix_port = 0;
		impl->adapter_mix_flags = impl->client_port->spa_flags;
		impl->use_converter = false;
		emit_port_info(impl);
	}

	if (impl->use_converter) {
		if ((res = spa_node_port_set_io(impl->adapter_mix,
					SPA_DIRECTION_REVERSE(impl->direction),
					impl->adapter_mix_port,
					SPA_IO_Buffers,
					impl->client_port_mix.io,
					sizeof(impl->client_port_mix.io))) < 0)
			return;
	}

	switch (media_type) {
	case SPA_MEDIA_TYPE_audio:
		type = "Audio";
		break;
	case SPA_MEDIA_TYPE_video:
		type = "Video";
		break;
	default:
		type = "Generic";
		break;
	}

	snprintf(media_class, sizeof(media_class), "Stream/%s/%s", dir, type);

	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_CLASS, media_class);
	pw_node_update_properties(impl->this.node, &SPA_DICT_INIT(items, 1));
}

static void cleanup(struct impl *impl)
{
	pw_log_debug(NAME " %p: cleanup", &impl->this);
	if (impl->use_converter) {
		if (impl->handle)
			pw_unload_spa_handle(impl->handle);
	}

	free(impl->buffers);
	if (impl->mem)
		pw_memblock_free(impl->mem);
	free(impl);
}

static void client_node_destroy(void *data)
{
	struct impl *impl = data;
	pw_log_debug(NAME " %p: destroy", &impl->this);

	pw_node_set_driver(impl->client_node->node, NULL);

	spa_hook_remove(&impl->node_listener);
	pw_node_destroy(impl->this.node);
	impl->this.node = NULL;
}

static void client_node_free(void *data)
{
	struct impl *impl = data;
	pw_log_debug(NAME " %p: free", &impl->this);
	spa_hook_remove(&impl->client_node_listener);
	cleanup(impl);
}

static void client_node_result(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct impl *impl = data;
	struct node *node = &impl->node;
	pw_log_trace(NAME " %p: result %d %d", &impl->this, seq, res);
	spa_node_emit_result(&node->hooks, seq, res, type, result);
}

static void client_node_active_changed(void *data, bool active)
{
	struct impl *impl = data;

	pw_log_debug(NAME " %p: active %d", &impl->this, active);
	impl->active = active;
}

static void client_node_info_changed(void *data, const struct pw_node_info *info)
{
	struct impl *impl = data;
	struct pw_client_stream *this = &impl->this;

	pw_log_debug(NAME " %p: info changed", this);

	if (this->node)
		pw_node_update_properties(this->node, info->props);
}

static const struct pw_node_events client_node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = client_node_destroy,
	.free = client_node_free,
	.initialized = client_node_initialized,
	.result = client_node_result,
	.active_changed = client_node_active_changed,
	.info_changed = client_node_info_changed,
};

static void node_destroy(void *data)
{
	struct impl *impl = data;
	pw_log_debug(NAME " %p: destroy", &impl->this);
	spa_hook_remove(&impl->client_node_listener);
}

static void node_free(void *data)
{
	struct impl *impl = data;
	pw_log_debug(NAME " %p: free", &impl->this);
	pw_client_node_destroy(impl->client_node);
	spa_hook_remove(&impl->node_listener);
	cleanup(impl);
}

static void node_initialized(void *data)
{
	struct impl *impl = data;
	pw_client_node_registered(impl->client_node, impl->this.node->global);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = node_destroy,
	.free = node_free,
	.initialized = node_initialized,
};


static int node_ready(void *data, int status)
{
	struct impl *impl = data;
	pw_log_trace_fp(NAME " %p: ready %d", &impl->this, status);

	impl->driver = false;
	impl_node_process(&impl->node.node);
	impl->driver = true;

	return spa_node_call_ready(&impl->node.callbacks, status);
}

static const struct spa_node_callbacks node_callbacks = {
	SPA_VERSION_NODE_CALLBACKS,
	.ready = node_ready,
};

/** Create a new client stream
 * \param client an owner \ref pw_client
 * \param id an id
 * \param name a name
 * \param properties extra properties
 * \return a newly allocated client stream
 *
 * Create a new \ref pw_stream.
 *
 * \memberof pw_client_stream
 */
struct pw_client_stream *pw_client_stream_new(struct pw_resource *resource,
					  struct pw_global *parent,
					  struct pw_properties *properties)
{
	struct impl *impl;
	struct pw_client_stream *this;
	struct pw_client *client = pw_resource_get_client(resource);
	struct pw_core *core = pw_client_get_core(client);
	const struct spa_support *support;
	struct pw_properties *props;
	uint32_t n_support;
	const char *name;
	int res;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL) {
		res = -errno;
		goto error_exit_cleanup;
	}

	this = &impl->this;

	impl->core = core;

	pw_log_debug(NAME " %p: new", impl);

	props = pw_properties_copy(properties);
	pw_properties_set(props, PW_KEY_NODE_DRIVER, NULL);

	impl->client_node = pw_client_node_new(
			resource,
			parent,
			props,
			false);
	resource = NULL;
	if (impl->client_node == NULL) {
		res = -errno;
		goto error_exit_free;
	}

	impl->cnode = pw_node_get_implementation(impl->client_node->node);
	spa_node_set_callbacks(impl->cnode, &node_callbacks, impl);

	support = pw_core_get_support(impl->core, &n_support);
	node_init(&impl->node, NULL, support, n_support);
	impl->node.impl = impl;

	if ((name = pw_properties_get(properties, PW_KEY_NODE_NAME)) == NULL)
		name = NAME;

	this->node = pw_spa_node_new(core,
				     client,
				     parent,
				     name,
				     PW_SPA_NODE_FLAG_ASYNC |
				     PW_SPA_NODE_FLAG_ACTIVATE,
				     (struct spa_node *)&impl->node.node,
				     NULL,
				     properties, 0);
	properties = NULL;
	if (this->node == NULL) {
		res = -errno;
		goto error_exit_free_client_node;
	}

	this->node->remote = true;

	pw_node_add_listener(impl->client_node->node,
			     &impl->client_node_listener,
			     &client_node_events, impl);
	pw_node_add_listener(this->node, &impl->node_listener, &node_events, impl);

	return this;

error_exit_free_client_node:
	pw_client_node_destroy(impl->client_node);
error_exit_free:
	free(impl);
error_exit_cleanup:
	if (resource)
		pw_resource_destroy(resource);
	if (properties)
		pw_properties_free(properties);
	errno = -res;
	return NULL;
}

/** Destroy a client stream
 * \param stream the client stream to destroy
 * \memberof pw_client_stream
 */
void pw_client_stream_destroy(struct pw_client_stream *stream)
{
	struct impl *impl = SPA_CONTAINER_OF(stream, struct impl, this);
	pw_client_node_destroy(impl->client_node);
	impl->client_node = NULL;
}
