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

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include <spa/node/utils.h>
#include <spa/pod/parser.h>
#include <spa/pod/compare.h>
#include <spa/param/param.h>
#include <spa/buffer/alloc.h>

#include "pipewire/private.h"
#include "pipewire/interfaces.h"
#include "pipewire/control.h"
#include "pipewire/link.h"
#include "pipewire/type.h"
#include "pipewire/work-queue.h"

#undef spa_debug
#include <spa/debug/node.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>

#define MAX_BUFFERS     64

/** \cond */
struct impl {
	struct pw_link this;

	bool prepare;
	bool activated;
	bool passive;

	struct pw_work_queue *work;

	struct spa_pod *format_filter;
	struct pw_properties *properties;

	struct spa_hook input_port_listener;
	struct spa_hook input_node_listener;
	struct spa_hook output_port_listener;
	struct spa_hook output_node_listener;

	struct spa_io_buffers io;

	struct pw_node *inode, *onode;
};

struct resource_data {
	struct spa_hook resource_listener;
};

/** \endcond */

static void debug_link(struct pw_link *link)
{
	struct pw_node *in = link->input->node, *out = link->output->node;

	pw_log_debug("link %p: %d %d %d out %d %d %d , %d %d %d in %d %d %d", link,
			out->n_used_input_links,
			out->n_ready_input_links,
			out->idle_used_input_links,
			out->n_used_output_links,
			out->n_ready_output_links,
			out->idle_used_output_links,
			in->n_used_input_links,
			in->n_ready_input_links,
			in->idle_used_input_links,
			in->n_used_output_links,
			in->n_ready_output_links,
			in->idle_used_output_links);
}

static void pw_link_update_state(struct pw_link *link, enum pw_link_state state, char *error)
{
	enum pw_link_state old = link->info.state;
	struct pw_node *in = link->input->node, *out = link->output->node;
	struct pw_resource *resource;

	if (state == old)
		return;


	if (state == PW_LINK_STATE_ERROR) {
		pw_log_error("link %p: update state %s -> error (%s)", link,
		     pw_link_state_as_string(old), error);
	} else {
		pw_log_debug("link %p: update state %s -> %s", link,
		     pw_link_state_as_string(old), pw_link_state_as_string(state));
	}

	link->info.state = state;
	free((char*)link->info.error);
	link->info.error = error;

	pw_link_emit_state_changed(link, old, state, error);

	link->info.change_mask |= PW_LINK_CHANGE_MASK_STATE;
	pw_link_emit_info_changed(link, &link->info);

	if (link->global)
		spa_list_for_each(resource, &link->global->resource_list, link)
			pw_link_resource_info(resource, &link->info);

	link->info.change_mask = 0;

	debug_link(link);

	if (old != PW_LINK_STATE_PAUSED && state == PW_LINK_STATE_PAUSED) {
		if (++out->n_ready_output_links == out->n_used_output_links &&
		    out->n_ready_input_links == out->n_used_input_links)
			pw_node_set_state(out, PW_NODE_STATE_RUNNING);
		if (++in->n_ready_input_links == in->n_used_input_links &&
		    in->n_ready_output_links == in->n_used_output_links)
			pw_node_set_state(in, PW_NODE_STATE_RUNNING);
		pw_link_activate(link);
	}
	else if (old == PW_LINK_STATE_PAUSED && state < PW_LINK_STATE_PAUSED) {
		if (--out->n_ready_output_links == 0 &&
		    out->n_ready_input_links == 0)
			pw_node_set_state(out, PW_NODE_STATE_IDLE);
		if (--in->n_ready_input_links == 0 &&
		    in->n_ready_output_links == 0)
			pw_node_set_state(in, PW_NODE_STATE_IDLE);
	}
}

static void complete_ready(void *obj, void *data, int res, uint32_t id)
{
	struct pw_link *this = data;
	struct pw_port_mix *mix = obj == this->input->node ? &this->rt.in_mix : &this->rt.out_mix;
	struct pw_port *port = mix->p;

	if (SPA_RESULT_IS_OK(res)) {
		pw_port_update_state(port, PW_PORT_STATE_READY);
		pw_log_debug("port %p: state READY", port);
	} else {
		pw_port_update_state(port, PW_PORT_STATE_ERROR);
		pw_log_warn("port %p: failed to go to READY", port);
	}
	if (this->input->state >= PW_PORT_STATE_READY &&
	    this->output->state >= PW_PORT_STATE_READY)
		pw_link_update_state(this, PW_LINK_STATE_ALLOCATING, NULL);
}

static void complete_paused(void *obj, void *data, int res, uint32_t id)
{
	struct pw_link *this = data;
	struct pw_port_mix *mix = obj == this->input->node ? &this->rt.in_mix : &this->rt.out_mix;
	struct pw_port *port = mix->p;

	if (SPA_RESULT_IS_OK(res)) {
		pw_port_update_state(port, PW_PORT_STATE_PAUSED);
		mix->have_buffers = true;
		pw_log_debug("port %p: state PAUSED", port);
	} else {
		pw_port_update_state(port, PW_PORT_STATE_ERROR);
		mix->have_buffers = false;
		pw_log_warn("port %p: failed to go to PAUSED", port);
	}
	if (this->rt.in_mix.have_buffers && this->rt.out_mix.have_buffers)
		pw_link_update_state(this, PW_LINK_STATE_PAUSED, NULL);

}

static int do_negotiate(struct pw_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int res = -EIO, res2;
	struct spa_pod *format = NULL, *current;
	char *error = NULL;
	struct pw_resource *resource;
	bool changed = true;
	struct pw_port *input, *output;
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t index;
	uint32_t in_state, out_state;

	if (this->info.state >= PW_LINK_STATE_NEGOTIATING)
		return 0;

	input = this->input;
	output = this->output;

	in_state = input->state;
	out_state = output->state;

	pw_log_debug("link %p: in_state:%d out_state:%d", this, in_state, out_state);

	if (in_state != PW_PORT_STATE_CONFIGURE && out_state != PW_PORT_STATE_CONFIGURE)
		return 0;

	pw_link_update_state(this, PW_LINK_STATE_NEGOTIATING, NULL);

	input = this->input;
	output = this->output;

	if ((res = pw_core_find_format(this->core, output, input, NULL, 0, NULL, &format, &b, &error)) < 0)
		goto error;

	format = spa_pod_copy(format);
	spa_pod_fixate(format);

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (out_state > PW_PORT_STATE_CONFIGURE && output->node->info.state == PW_NODE_STATE_IDLE) {
		index = 0;
		res = spa_node_port_enum_params_sync(output->node->node,
				output->direction, output->port_id,
				SPA_PARAM_Format, &index,
				NULL, &current, &b);
		switch (res) {
		case -EIO:
			current = NULL;
			res = 0;
			/* fallthrough */
		case 1:
			break;
		case 0:
			res = -EBADF;
			/* fallthrough */
		default:
			asprintf(&error, "error get output format: %s", spa_strerror(res));
			goto error;
		}
		if (current == NULL || spa_pod_compare(current, format) != 0) {
			pw_log_debug("link %p: output format change, renegotiate", this);
			if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG)) {
				if (current)
					spa_debug_pod(2, NULL, current);
				spa_debug_pod(2, NULL, format);
			}
			pw_node_set_state(output->node, PW_NODE_STATE_SUSPENDED);
			out_state = PW_PORT_STATE_CONFIGURE;
		}
		else {
			pw_log_debug("link %p: format was already set", this);
			changed = false;
		}
	}
	if (in_state > PW_PORT_STATE_CONFIGURE && input->node->info.state == PW_NODE_STATE_IDLE) {
		index = 0;
		res = spa_node_port_enum_params_sync(input->node->node,
				input->direction, input->port_id,
				SPA_PARAM_Format, &index,
				NULL, &current, &b);
		switch (res) {
		case -EIO:
			current = NULL;
			res = 0;
			/* fallthrough */
		case 1:
			break;
		case 0:
			res = -EBADF;
			/* fallthrough */
		default:
			asprintf(&error, "error get input format: %s", spa_strerror(res));
			goto error;
		}
		if (current == NULL || spa_pod_compare(current, format) != 0) {
			pw_log_debug("link %p: input format change, renegotiate", this);
			if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG)) {
				if (current)
					spa_debug_pod(2, NULL, current);
				spa_debug_pod(2, NULL, format);
			}
			pw_node_set_state(input->node, PW_NODE_STATE_SUSPENDED);
			in_state = PW_PORT_STATE_CONFIGURE;
		}
		else {
			pw_log_debug("link %p: format was already set", this);
			changed = false;
		}
	}

	pw_log_debug("link %p: doing set format %p", this, format);
	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_format(2, NULL, format);

	if (out_state == PW_PORT_STATE_CONFIGURE) {
		pw_log_debug("link %p: doing set format on output", this);
		if ((res = pw_port_set_param(output,
					     SPA_PARAM_Format, SPA_NODE_PARAM_FLAG_NEAREST,
					     format)) < 0) {
			asprintf(&error, "error set output format: %d (%s)", res, spa_strerror(res));
			goto error;
		}
		if (SPA_RESULT_IS_ASYNC(res)) {
			res = spa_node_sync(output->node->node, res),
			pw_work_queue_add(impl->work, output->node, res,
					complete_ready, this);
		} else {
			complete_ready(output->node, this, res, 0);
		}
	}
	if (in_state == PW_PORT_STATE_CONFIGURE) {
		pw_log_debug("link %p: doing set format on input", this);
		if ((res2 = pw_port_set_param(input,
					      SPA_PARAM_Format, SPA_NODE_PARAM_FLAG_NEAREST,
					      format)) < 0) {
			asprintf(&error, "error set input format: %d (%s)", res2, spa_strerror(res2));
			goto error;
		}
		if (SPA_RESULT_IS_ASYNC(res2)) {
			res2 = spa_node_sync(input->node->node, res2),
			pw_work_queue_add(impl->work, input->node, res2,
					complete_ready, this);
			if (res == 0)
				res = res2;
		} else {
			complete_ready(input->node, this, res2, 0);
		}
	}

	free(this->info.format);
	this->info.format = format;

	if (changed) {
		this->info.change_mask |= PW_LINK_CHANGE_MASK_FORMAT;

		pw_link_emit_info_changed(this, &this->info);

		if (this->global)
			spa_list_for_each(resource, &this->global->resource_list, link)
				pw_link_resource_info(resource, &this->info);

		this->info.change_mask = 0;
	}
	pw_log_debug("link %p: result %d", this, res);
	return res;

      error:
	pw_link_update_state(this, PW_LINK_STATE_ERROR, error);
	free(format);
	return res;
}

static struct spa_pod *find_param(struct spa_pod **params, uint32_t n_params, uint32_t type)
{
	uint32_t i;

	for (i = 0; i < n_params; i++) {
		if (spa_pod_is_object_type(params[i], type))
			return params[i];
	}
	return NULL;
}

/* Allocate an array of buffers that can be shared */
static int alloc_buffers(struct pw_link *this,
			 uint32_t n_buffers,
			 uint32_t n_params,
			 struct spa_pod **params,
			 uint32_t n_datas,
			 uint32_t *data_sizes,
			 int32_t *data_strides,
			 uint32_t *data_aligns,
			 struct allocation *allocation)
{
	int res;
	struct spa_buffer **buffers, *bp;
	uint32_t i;
	uint32_t n_metas;
	struct spa_meta *metas;
	struct spa_data *datas;
	struct pw_memblock *m;
	struct spa_buffer_alloc_info info = { 0, };

	n_metas = 0;

	metas = alloca(sizeof(struct spa_meta) * n_params);
	datas = alloca(sizeof(struct spa_data) * n_datas);

	/* collect metadata */
	for (i = 0; i < n_params; i++) {
		if (spa_pod_is_object_type (params[i], SPA_TYPE_OBJECT_ParamMeta)) {
			uint32_t type, size;

			if (spa_pod_parse_object(params[i],
				SPA_TYPE_OBJECT_ParamMeta, NULL,
				SPA_PARAM_META_type, SPA_POD_Id(&type),
				SPA_PARAM_META_size, SPA_POD_Int(&size)) < 0)
				continue;

			pw_log_debug("link %p: enable meta %d %d", this, type, size);

			metas[n_metas].type = type;
			metas[n_metas].size = size;
			n_metas++;
		}
	}

	for (i = 0; i < n_datas; i++) {
		struct spa_data *d = &datas[i];

		if (data_sizes[i] > 0) {
			d->type = SPA_DATA_MemPtr;
			d->maxsize = data_sizes[i];
		} else {
			d->type = SPA_ID_INVALID;
			d->maxsize = 0;
		}
	}

        spa_buffer_alloc_fill_info(&info, n_metas, metas, n_datas, datas, data_aligns);

	buffers = calloc(n_buffers, info.skel_size + sizeof(struct spa_buffer *));
	if (buffers == NULL)
		return -ENOMEM;

	/* pointer to buffer structures */
	bp = SPA_MEMBER(buffers, n_buffers * sizeof(struct spa_buffer *), struct spa_buffer);

	if ((res = pw_memblock_alloc(PW_MEMBLOCK_FLAG_WITH_FD |
				     PW_MEMBLOCK_FLAG_MAP_READWRITE |
				     PW_MEMBLOCK_FLAG_SEAL, n_buffers * info.mem_size,
				     &m)) < 0)
		return res;

	pw_log_debug("layout buffers %p data %p", bp, m->ptr);
	spa_buffer_alloc_layout_array(&info, n_buffers, buffers, bp, m->ptr);

	allocation->mem = m;
	allocation->n_buffers = n_buffers;
	allocation->buffers = buffers;

	return 0;
}

static int
param_filter(struct pw_link *this,
	     struct pw_port *in_port,
	     struct pw_port *out_port,
	     uint32_t id,
	     struct spa_pod_builder *result)
{
	uint8_t ibuf[4096];
        struct spa_pod_builder ib = { 0 };
	struct spa_pod *oparam, *iparam;
	uint32_t iidx, oidx, num = 0;
	int res;

	for (iidx = 0;;) {
	        spa_pod_builder_init(&ib, ibuf, sizeof(ibuf));
		pw_log_debug("iparam %d", iidx);
		if ((res = spa_node_port_enum_params_sync(in_port->node->node,
						in_port->direction, in_port->port_id,
						id, &iidx, NULL, &iparam, &ib)) < 0)
			break;

		if (res != 1) {
			if (num > 0)
				break;
			iparam = NULL;
		}

		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG) && iparam != NULL)
			spa_debug_pod(2, NULL, iparam);

		for (oidx = 0;;) {
			pw_log_debug("oparam %d", oidx);
			if (spa_node_port_enum_params_sync(out_port->node->node,
						out_port->direction, out_port->port_id,
						id, &oidx, iparam, &oparam, result) != 1) {
				break;
			}

			if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
				spa_debug_pod(2, NULL, oparam);

			num++;
		}
		if (iparam == NULL && num == 0)
			break;
	}
	return num;
}

static int port_set_io(struct pw_link *this, struct pw_port *port, uint32_t id,
		void *data, size_t size, struct pw_port_mix *mix)
{
	int res = 0;

	mix->io = data;
	pw_log_debug("link %p: %s port %p %d.%d set io: %d %p %zd", this,
			pw_direction_as_string(port->direction),
			port, port->port_id, mix->port.port_id, id, data, size);

	if (port->mix->port_set_io) {
		if ((res = spa_node_port_set_io(port->mix,
				     mix->port.direction,
				     mix->port.port_id,
				     id, data, size)) < 0)
			pw_log_warn("port %p: can't set io: %s", port, spa_strerror(res));
	}
	return res;
}

static int select_io(struct pw_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct spa_io_buffers *io;
	int res;

	io = this->rt.in_mix.io;
	if (io == NULL)
		io = this->rt.out_mix.io;
	if (io == NULL)
		io = &impl->io;
	if (io == NULL)
		return -EIO;

	if ((res = port_set_io(this, this->input, SPA_IO_Buffers, io,
			sizeof(struct spa_io_buffers), &this->rt.in_mix)) < 0)
		return res;

	if ((res = port_set_io(this, this->output, SPA_IO_Buffers, io,
			sizeof(struct spa_io_buffers), &this->rt.out_mix)) < 0)
		return res;

	this->io = io;
	return 0;
}

static int do_allocation(struct pw_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int res, out_res = 0, in_res = 0;
	uint32_t in_flags, out_flags;
	char *error = NULL;
	struct pw_port *input, *output;
	struct allocation allocation = { NULL, };

	if (this->info.state > PW_LINK_STATE_ALLOCATING)
		return 0;

	input = this->input;
	output = this->output;

	pw_log_debug("link %p: in_state:%d out_state:%d", this, input->state, output->state);

	pw_link_update_state(this, PW_LINK_STATE_ALLOCATING, NULL);

	pw_log_debug("link %p: doing alloc buffers %p %p", this, output->node, input->node);

	in_flags = input->spa_flags;
	out_flags = output->spa_flags;

	if (out_flags & SPA_PORT_FLAG_LIVE) {
		pw_log_debug("setting link as live");
		output->node->live = true;
		input->node->live = true;
	}

	if (output->allocation.n_buffers) {
		out_flags = SPA_PORT_FLAG_CAN_USE_BUFFERS;
		in_flags = SPA_PORT_FLAG_CAN_USE_BUFFERS;

		move_allocation(&output->allocation, &allocation);

		pw_log_debug("link %p: reusing %d output buffers %p", this,
				allocation.n_buffers, allocation.buffers);
	} else {
		struct spa_pod **params, *param;
		uint8_t buffer[4096];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		uint32_t i, offset, n_params;
		uint32_t max_buffers;
		size_t minsize = 8192, stride = 0, align;
		uint32_t data_sizes[1];
		int32_t data_strides[1];
		uint32_t data_aligns[1];

		n_params = param_filter(this, input, output, SPA_PARAM_Buffers, &b);
		n_params += param_filter(this, input, output, SPA_PARAM_Meta, &b);

		params = alloca(n_params * sizeof(struct spa_pod *));
		for (i = 0, offset = 0; i < n_params; i++) {
			params[i] = SPA_MEMBER(buffer, offset, struct spa_pod);
			spa_pod_fixate(params[i]);
			pw_log_debug("fixated param %d:", i);
			if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
				spa_debug_pod(2, NULL, params[i]);
			offset += SPA_ROUND_UP_N(SPA_POD_SIZE(params[i]), 8);
		}

		max_buffers = MAX_BUFFERS;
		minsize = stride = 0;
		align = 8;
		param = find_param(params, n_params, SPA_TYPE_OBJECT_ParamBuffers);
		if (param) {
			uint32_t qmax_buffers = max_buffers,
			    qminsize = minsize, qstride = stride, qalign = align;

			spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamBuffers, NULL,
				SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(&qmax_buffers),
				SPA_PARAM_BUFFERS_size,    SPA_POD_Int(&qminsize),
				SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(&qstride),
				SPA_PARAM_BUFFERS_align,   SPA_POD_Int(&qalign));

			max_buffers =
			    qmax_buffers == 0 ? max_buffers : SPA_MIN(qmax_buffers,
							      max_buffers);
			minsize = SPA_MAX(minsize, qminsize);
			stride = SPA_MAX(stride, qstride);
			align = SPA_MAX(align, qalign);

			pw_log_debug("%d %d %d %d -> %zd %zd %d %zd",
					qminsize, qstride, qmax_buffers, qalign,
					minsize, stride, max_buffers, align);
		} else {
			pw_log_warn("no buffers param");
			minsize = 8192;
			max_buffers = 4;
		}

		/* when one of the ports can allocate buffer memory, set the minsize to
		 * 0 to make sure we don't allocate memory in the shared memory */
		if ((in_flags & SPA_PORT_FLAG_CAN_ALLOC_BUFFERS) ||
		    (out_flags & SPA_PORT_FLAG_CAN_ALLOC_BUFFERS)) {
			minsize = 0;
		}

		data_sizes[0] = minsize;
		data_strides[0] = stride;
		data_aligns[0] = align;

		if ((res = alloc_buffers(this,
					 max_buffers,
					 n_params,
					 params,
					 1,
					 data_sizes, data_strides,
					 data_aligns,
					 &allocation)) < 0) {
			asprintf(&error, "error alloc buffers: %d", res);
			goto error;
		}

		pw_log_debug("link %p: allocating %d buffers %p %zd %zd", this,
			     allocation.n_buffers, allocation.buffers, minsize, stride);

		if (out_flags & SPA_PORT_FLAG_CAN_ALLOC_BUFFERS) {
			if ((res = pw_port_alloc_buffers(output,
							params, n_params,
							allocation.buffers,
							&allocation.n_buffers)) < 0) {
				asprintf(&error, "error alloc output buffers: %d", res);
				goto error;
			}
			out_res = res;
			out_flags &= ~SPA_PORT_FLAG_CAN_USE_BUFFERS;
			move_allocation(&allocation, &output->allocation);

			pw_log_debug("link %p: allocated %d buffers %p from output port", this,
				     allocation.n_buffers, allocation.buffers);
		}
	}
	if (out_flags & SPA_PORT_FLAG_CAN_USE_BUFFERS) {
		pw_log_debug("link %p: using %d buffers %p on output port", this,
			     allocation.n_buffers, allocation.buffers);
		if ((res = pw_port_use_buffers(output,
					       this->rt.out_mix.port.port_id,
					       allocation.buffers,
					       allocation.n_buffers)) < 0) {
			asprintf(&error, "link %p: error use output buffers: %s", this,
					spa_strerror(res));
			goto error;
		}
		out_res = res;
		move_allocation(&allocation, &output->allocation);
	}
	if (in_flags & SPA_PORT_FLAG_CAN_USE_BUFFERS) {
		pw_log_debug("link %p: using %d buffers %p on input port", this,
			     allocation.n_buffers, allocation.buffers);
		if ((res = pw_port_use_buffers(input,
						this->rt.in_mix.port.port_id,
						allocation.buffers,
						allocation.n_buffers)) < 0) {
			asprintf(&error, "link %p: error use input buffers: %s", this,
					spa_strerror(res));
			goto error;
		}
		in_res = res;
	} else {
		asprintf(&error, "no common buffer alloc found");
		res = -EIO;
		goto error;
	}

	if (SPA_RESULT_IS_ASYNC(out_res)) {
		pw_work_queue_add(impl->work, output->node,
				spa_node_sync(output->node->node, out_res),
				complete_paused, this);
	} else {
		complete_paused(output->node, this, out_res, 0);
	}
	if (SPA_RESULT_IS_ASYNC(in_res)) {
		pw_work_queue_add(impl->work, input->node,
				spa_node_sync(input->node->node, in_res),
				complete_paused, this);
	} else {
		complete_paused(input->node, this, in_res, 0);
	}
	return 0;

      error:
	free_allocation(&output->allocation);
	free_allocation(&input->allocation);
	pw_link_update_state(this, PW_LINK_STATE_ERROR, error);
	return res;
}

static int
do_activate_link(struct spa_loop *loop,
		 bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
        struct pw_link *this = user_data;
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);

	pw_log_trace("link %p: activate", this);

	spa_list_append(&this->output->rt.mix_list, &this->rt.out_mix.rt_link);
	spa_list_append(&this->input->rt.mix_list, &this->rt.in_mix.rt_link);

	if (impl->inode != impl->onode) {
		this->rt.target.activation = impl->inode->rt.activation;
		spa_list_append(&impl->onode->rt.target_list, &this->rt.target.link);
		this->rt.target.activation->state[0].required++;
	}
	return 0;
}

int pw_link_activate(struct pw_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);

	pw_log_debug("link %p: activate %d %d", this, impl->activated, this->info.state);

	if (impl->activated)
		return 0;

	pw_link_prepare(this);

	if (this->info.state == PW_LINK_STATE_PAUSED) {
		pw_loop_invoke(this->output->node->data_loop,
		       do_activate_link, SPA_ID_INVALID, NULL, 0, false, this);
		impl->activated = true;
	}
	return 0;
}
static void check_states(void *obj, void *user_data, int res, uint32_t id)
{
	struct pw_link *this = obj;
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int in_state, out_state;
	struct pw_port *input, *output;

	if (this->info.state == PW_LINK_STATE_ERROR)
		return;

	if (this->info.state == PW_LINK_STATE_PAUSED)
		return;

	input = this->input;
	output = this->output;

	if (input == NULL || output == NULL) {
		pw_link_update_state(this, PW_LINK_STATE_ERROR,
				strdup("link without input or output port"));
		return;
	}

	if (input->node->info.state == PW_NODE_STATE_ERROR ||
	    output->node->info.state == PW_NODE_STATE_ERROR) {
		pw_log_warn("link %p: one of the nodes is in error in:%d out:%d", this,
				input->node->info.state,
				output->node->info.state);
		return;
	}

	in_state = input->state;
	out_state = output->state;

	pw_log_debug("link %p: input state %d, output state %d", this, in_state, out_state);

	if (in_state == PW_PORT_STATE_ERROR || out_state == PW_PORT_STATE_ERROR) {
		pw_link_update_state(this, PW_LINK_STATE_ERROR, strdup("ports are in error"));
		return;
	}

	if (PW_PORT_IS_CONTROL(output) && PW_PORT_IS_CONTROL(input)) {
		pw_port_update_state(input, PW_PORT_STATE_PAUSED);
		pw_port_update_state(output, PW_PORT_STATE_PAUSED);
		pw_link_update_state(this, PW_LINK_STATE_PAUSED, NULL);
	}

	if ((res = do_negotiate(this)) != 0)
		goto exit;

	if ((res = do_allocation(this)) != 0)
		goto exit;

      exit:
	if (SPA_RESULT_IS_ERROR(res)) {
		pw_log_debug("link %p: got error result %d", this, res);
		return;
	}

	pw_work_queue_add(impl->work,
			  this, -EBUSY, (pw_work_func_t) check_states, this);
}

static void clear_port_buffers(struct pw_link *link, struct pw_port *port)
{
	int res;
	struct pw_port_mix *mix;

	pw_log_debug("%d %p", spa_list_is_empty(&port->links), port->allocation.mem);

	/* we don't clear output buffers when the link goes away. They will get
	 * cleared when the node goes to suspend */
	if (port->direction == PW_DIRECTION_OUTPUT)
		return;

	if (port->direction == PW_DIRECTION_OUTPUT)
		mix = &link->rt.out_mix;
	else
		mix = &link->rt.in_mix;

	if ((res = pw_port_use_buffers(port, mix->port.port_id, NULL, 0)) < 0)
		pw_log_warn("link %p: port %p clear error %s", link, port, spa_strerror(res));
}

static void input_remove(struct pw_link *this, struct pw_port *port)
{
	struct impl *impl = (struct impl *) this;
	struct pw_port_mix *mix = &this->rt.in_mix;

	pw_log_debug("link %p: remove input port %p", this, port);
	spa_hook_remove(&impl->input_port_listener);
	spa_hook_remove(&impl->input_node_listener);

	spa_list_remove(&this->input_link);
	pw_port_emit_link_removed(this->input, this);

	clear_port_buffers(this, port);

	port_set_io(this, this->input, SPA_IO_Buffers, NULL, 0, mix);
	pw_port_release_mix(port, mix);
	this->input = NULL;
}

static void output_remove(struct pw_link *this, struct pw_port *port)
{
	struct impl *impl = (struct impl *) this;
	struct pw_port_mix *mix = &this->rt.out_mix;

	pw_log_debug("link %p: remove output port %p", this, port);
	spa_hook_remove(&impl->output_port_listener);
	spa_hook_remove(&impl->output_node_listener);

	spa_list_remove(&this->output_link);
	pw_port_emit_link_removed(this->output, this);

	clear_port_buffers(this, port);

	port_set_io(this, this->output, SPA_IO_Buffers, NULL, 0, mix);
	pw_port_release_mix(port, mix);
	this->output = NULL;
}

static void on_port_destroy(struct pw_link *this, struct pw_port *port)
{
	pw_link_emit_port_unlinked(this, port);

	pw_link_update_state(this, PW_LINK_STATE_UNLINKED, NULL);
	pw_link_destroy(this);
}

static void input_port_destroy(void *data)
{
	struct impl *impl = data;
	on_port_destroy(&impl->this, impl->this.input);
}

static void output_port_destroy(void *data)
{
	struct impl *impl = data;
	on_port_destroy(&impl->this, impl->this.output);
}

int pw_link_prepare(struct pw_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);

	pw_log_debug("link %p: prepare %d", this, impl->prepare);

	if (impl->prepare)
		return 0;

	impl->prepare = true;

	this->output->node->n_used_output_links++;
	this->input->node->n_used_input_links++;

	if (impl->passive) {
		this->output->node->idle_used_output_links++;
		this->input->node->idle_used_input_links++;
	}

	debug_link(this);

	pw_work_queue_add(impl->work,
			  this, -EBUSY, (pw_work_func_t) check_states, this);

	return 0;
}


static int
do_deactivate_link(struct spa_loop *loop,
		   bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
        struct pw_link *this = user_data;

	pw_log_trace("link %p: disable %p and %p", this, &this->rt.in_mix, &this->rt.out_mix);

	spa_list_remove(&this->rt.out_mix.rt_link);
	spa_list_remove(&this->rt.in_mix.rt_link);

	if (this->input->node != this->output->node) {
		spa_list_remove(&this->rt.target.link);
		this->rt.target.activation->state[0].required--;
	}

	return 0;
}

int pw_link_deactivate(struct pw_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct pw_node *input_node, *output_node;

	pw_log_debug("link %p: deactivate %d %d", this, impl->prepare, impl->activated);

	if (!impl->prepare)
		return 0;

	impl->prepare = false;
	if (impl->activated) {
		pw_loop_invoke(this->output->node->data_loop,
			       do_deactivate_link, SPA_ID_INVALID, NULL, 0, true, this);
		impl->activated = false;
	}

	input_node = this->input->node;
	output_node = this->output->node;

	input_node->n_used_input_links--;
	output_node->n_used_output_links--;

	if (impl->passive) {
		input_node->idle_used_input_links--;
		output_node->idle_used_output_links--;
	}

	debug_link(this);

	if (input_node->n_used_input_links <= input_node->idle_used_input_links &&
	    input_node->n_used_output_links <= input_node->idle_used_output_links &&
	    input_node->info.state > PW_NODE_STATE_IDLE) {
		pw_node_set_state(input_node, PW_NODE_STATE_IDLE);
		pw_log_debug("port %p: input state %d -> %d", this->input,
				this->input->state, PW_PORT_STATE_PAUSED);
	}

	if (output_node->n_used_input_links <= output_node->idle_used_input_links &&
	    output_node->n_used_output_links <= output_node->idle_used_output_links &&
	    output_node->info.state > PW_NODE_STATE_IDLE) {
		pw_node_set_state(output_node, PW_NODE_STATE_IDLE);
		pw_log_debug("port %p: output state %d -> %d", this->output,
				this->output->state, PW_PORT_STATE_PAUSED);
	}

	pw_link_update_state(this, PW_LINK_STATE_INIT, NULL);

	return 0;
}

static void link_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	spa_list_remove(&resource->link);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = link_unbind_func,
};

static int
global_bind(void *_data, struct pw_client *client, uint32_t permissions,
	       uint32_t version, uint32_t id)
{
	struct pw_link *this = _data;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	pw_resource_add_listener(resource, &data->resource_listener, &resource_events, resource);

	pw_log_debug("link %p: bound to %d", this, resource->id);

	spa_list_append(&global->resource_list, &resource->link);

	this->info.change_mask = ~0;
	pw_link_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

      no_mem:
	pw_log_error("can't create link resource");
	return -ENOMEM;
}

static const struct pw_port_events input_port_events = {
	PW_VERSION_PORT_EVENTS,
	.destroy = input_port_destroy,
};

static const struct pw_port_events output_port_events = {
	PW_VERSION_PORT_EVENTS,
	.destroy = output_port_destroy,
};

static void input_node_result(void *data, int seq, int res, const void *result)
{
	struct impl *impl = data;
	struct pw_node *node = impl->this.input->node;
	if (SPA_RESULT_IS_ASYNC(seq)) {
		pw_log_debug("link %p: input node %p result %d %d", impl, node, seq, res);
		pw_work_queue_complete(impl->work, node, SPA_RESULT_ASYNC_SEQ(seq), res);
	}
}

static void output_node_result(void *data, int seq, int res, const void *result)
{
	struct impl *impl = data;
	struct pw_node *node = impl->this.output->node;
	if (SPA_RESULT_IS_ASYNC(seq)) {
		pw_log_debug("link %p: output node %p result %d %d", impl, node, seq, res);
		pw_work_queue_complete(impl->work, node, SPA_RESULT_ASYNC_SEQ(seq), res);
	}
}

static const struct pw_node_events input_node_events = {
	PW_VERSION_NODE_EVENTS,
	.result = input_node_result,
};

static const struct pw_node_events output_node_events = {
	PW_VERSION_NODE_EVENTS,
	.result = output_node_result,
};

static int find_driver(struct pw_link *this)
{
	struct pw_node *out_driver, *in_driver;

	out_driver = this->output->node->driver_node;
	in_driver = this->input->node->driver_node;

	pw_log_debug("link %p: drivers %p/%p", this, out_driver, in_driver);

	if (out_driver == in_driver)
		return 0;

	if (out_driver->driver)
		pw_node_set_driver(in_driver, out_driver);
	else
		pw_node_set_driver(out_driver, in_driver);

	return 0;
}

static bool pw_node_can_reach(struct pw_node *output, struct pw_node *input)
{
	struct pw_port *p;

	if (output == input)
		return true;

	spa_list_for_each(p, &output->output_ports, link) {
		struct pw_link *l;

		spa_list_for_each(l, &p->links, output_link) {
			if (l->feedback)
				continue;
			if (l->input->node == input)
				return true;
		}
		spa_list_for_each(l, &p->links, output_link) {
			if (l->feedback)
				continue;
			if (pw_node_can_reach(l->input->node, input))
				return true;
		}
	}
	return false;
}

static void try_link_controls(struct impl *impl, struct pw_port *output, struct pw_port *input)
{
	struct pw_control *cin, *cout;
	struct pw_link *this = &impl->this;
	uint32_t omix, imix;
	int res;

	imix = this->rt.in_mix.port.port_id;
	omix = this->rt.out_mix.port.port_id;

	pw_log_debug("link %p: trying controls", impl);
	spa_list_for_each(cout, &output->control_list[SPA_DIRECTION_OUTPUT], port_link) {
		spa_list_for_each(cin, &input->control_list[SPA_DIRECTION_INPUT], port_link) {
			if ((res = pw_control_add_link(cout, omix, cin, imix, &this->control)) < 0)
				pw_log_error("failed to link controls: %s", spa_strerror(res));
			break;
		}
	}
	spa_list_for_each(cin, &output->control_list[SPA_DIRECTION_INPUT], port_link) {
		spa_list_for_each(cout, &input->control_list[SPA_DIRECTION_OUTPUT], port_link) {
			if ((res = pw_control_add_link(cout, imix, cin, omix, &this->notify)) < 0)
				pw_log_error("failed to link controls: %s", spa_strerror(res));
			break;
		}
	}
}

static void try_unlink_controls(struct impl *impl, struct pw_port *output, struct pw_port *input)
{
	struct pw_link *this = &impl->this;
	int res;

	pw_log_debug("link %p: unlinking controls", impl);
	if (this->control.valid) {
		if ((res = pw_control_remove_link(&this->control)) < 0)
			pw_log_error("failed to unlink controls: %s", spa_strerror(res));
	}
	if (this->notify.valid) {
		if ((res = pw_control_remove_link(&this->notify)) < 0)
			pw_log_error("failed to unlink controls: %s", spa_strerror(res));
	}
}

SPA_EXPORT
struct pw_link *pw_link_new(struct pw_core *core,
			    struct pw_port *output,
			    struct pw_port *input,
			    struct spa_pod *format_filter,
			    struct pw_properties *properties,
			    char **error,
			    size_t user_data_size)
{
	struct impl *impl;
	struct pw_link *this;
	struct pw_node *input_node, *output_node;
	int res;

	if (output == input)
		goto same_ports;

	if (output->direction != PW_DIRECTION_OUTPUT ||
	    input->direction != PW_DIRECTION_INPUT)
		goto wrong_direction;

	if (pw_link_find(output, input))
		goto link_exists;

	input_node = input->node;
	output_node = output->node;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		goto no_mem;

	this = &impl->this;
	this->feedback = pw_node_can_reach(input_node, output_node);
	pw_log_debug("link %p: new %p -> %p", this, input, output);

	if (user_data_size > 0)
                this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	impl->work = pw_work_queue_new(core->main_loop);

	this->core = core;
	this->properties = properties;
	this->info.state = PW_LINK_STATE_INIT;

	this->input = input;
	this->output = output;

	if (properties) {
		const char *str = pw_properties_get(properties, PW_LINK_PROP_PASSIVE);
		if (str && pw_properties_parse_bool(str))
			impl->passive = true;
	}
	spa_hook_list_init(&this->listener_list);

	impl->format_filter = format_filter;

	pw_port_add_listener(input, &impl->input_port_listener, &input_port_events, impl);
	pw_node_add_listener(input_node, &impl->input_node_listener, &input_node_events, impl);
	pw_port_add_listener(output, &impl->output_port_listener, &output_port_events, impl);
	pw_node_add_listener(output_node, &impl->output_node_listener, &output_node_events, impl);

	input_node->live = output_node->live;

	pw_log_debug("link %p: output node %p live %d, passive %d, feedback %d",
			this, output_node, output_node->live, impl->passive, this->feedback);

	spa_list_append(&output->links, &this->output_link);
	spa_list_append(&input->links, &this->input_link);

	this->info.format = NULL;
	this->info.props = this->properties ? &this->properties->dict : NULL;

	impl->io.buffer_id = SPA_ID_INVALID;
	impl->io.status = SPA_STATUS_NEED_BUFFER;

	pw_port_init_mix(output, &this->rt.out_mix);
	pw_port_init_mix(input, &this->rt.in_mix);

	if ((res = select_io(this)) < 0)
		goto no_io;

	if (this->feedback) {
		impl->inode = output_node;
		impl->onode = input_node;
	}
	else {
		impl->onode = output_node;
		impl->inode = input_node;
	}

	this->rt.target.signal = impl->inode->rt.target.signal;
	this->rt.target.data = impl->inode->rt.target.data;

	pw_log_debug("link %p: constructed %p:%d.%d -> %p:%d.%d", impl,
		     output_node, output->port_id, this->rt.out_mix.port.port_id,
		     input_node, input->port_id, this->rt.in_mix.port.port_id);

	find_driver(this);

	pw_port_emit_link_added(output, this);
	pw_port_emit_link_added(input, this);

	try_link_controls(impl, output, input);

	pw_node_emit_peer_added(output_node, input_node);

	return this;

      no_io:
	asprintf(error, "can't set io %d (%s)", res, spa_strerror(res));
	return NULL;
      same_ports:
	asprintf(error, "can't link the same ports");
	return NULL;
      wrong_direction:
	asprintf(error, "ports have wrong direction");
	return NULL;
      link_exists:
	asprintf(error, "link already exists");
	return NULL;
      no_mem:
	asprintf(error, "no memory");
	return NULL;
}

static void global_destroy(void *object)
{
	struct pw_link *link = object;
	spa_hook_remove(&link->global_listener);
	link->global = NULL;
	pw_link_destroy(link);
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.destroy = global_destroy,
};

SPA_EXPORT
int pw_link_register(struct pw_link *link,
		     struct pw_client *owner,
		     struct pw_global *parent,
		     struct pw_properties *properties)
{
	struct pw_core *core = link->core;
	struct pw_node *input_node, *output_node;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		return -ENOMEM;

	input_node = link->input->node;
	output_node = link->output->node;

	link->info.output_node_id = output_node->global->id;
	link->info.output_port_id = link->output->global->id;
	link->info.input_node_id = input_node->global->id;
	link->info.input_port_id = link->input->global->id;

	pw_properties_setf(properties, "link.output", "%d", link->info.output_port_id);
	pw_properties_setf(properties, "link.input", "%d", link->info.input_port_id);

	spa_list_append(&core->link_list, &link->link);
	link->registered = true;

	link->global = pw_global_new(core,
				     PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
				     properties,
				     global_bind,
				     link);
	if (link->global == NULL)
		return -ENOMEM;

	pw_global_add_listener(link->global, &link->global_listener, &global_events, link);

	link->info.id = link->global->id;
	pw_global_register(link->global, owner, parent);

	debug_link(link);

	if ((input_node->n_used_input_links >= input_node->idle_used_input_links ||
	    output_node->n_used_output_links >= output_node->idle_used_output_links) &&
	    input_node->active && output_node->active)
		pw_link_prepare(link);

	return 0;
}

SPA_EXPORT
void pw_link_destroy(struct pw_link *link)
{
	struct impl *impl = SPA_CONTAINER_OF(link, struct impl, this);

	pw_log_debug("link %p: destroy", impl);
	pw_link_emit_destroy(link);

	pw_link_deactivate(link);

	if (link->registered)
		spa_list_remove(&link->link);

	pw_node_emit_peer_removed(link->output->node, link->input->node);

	try_unlink_controls(impl, link->output, link->input);

	input_remove(link, link->input);
	output_remove(link, link->output);

	if (link->global) {
		spa_hook_remove(&link->global_listener);
		pw_global_destroy(link->global);
	}

	pw_log_debug("link %p: free", impl);
	pw_link_emit_free(link);

	pw_work_queue_destroy(impl->work);

	if (link->properties)
		pw_properties_free(link->properties);

	free(link->info.format);
	free(impl);
}

SPA_EXPORT
void pw_link_add_listener(struct pw_link *link,
			  struct spa_hook *listener,
			  const struct pw_link_events *events,
			  void *data)
{
	pw_log_debug("link %p: add listener %p", link, listener);
	spa_hook_list_append(&link->listener_list, listener, events, data);
}

struct pw_link *pw_link_find(struct pw_port *output_port, struct pw_port *input_port)
{
	struct pw_link *pl;

	spa_list_for_each(pl, &output_port->links, output_link) {
		if (pl->input == input_port)
			return pl;
	}
	return NULL;
}

SPA_EXPORT
struct pw_core *pw_link_get_core(struct pw_link *link)
{
	return link->core;
}

SPA_EXPORT
void *pw_link_get_user_data(struct pw_link *link)
{
	return link->user_data;
}

SPA_EXPORT
const struct pw_link_info *pw_link_get_info(struct pw_link *link)
{
	return &link->info;
}

SPA_EXPORT
struct pw_global *pw_link_get_global(struct pw_link *link)
{
	return link->global;
}

SPA_EXPORT
struct pw_port *pw_link_get_output(struct pw_link *link)
{
	return link->output;
}

SPA_EXPORT
struct pw_port *pw_link_get_input(struct pw_link *link)
{
	return link->input;
}
