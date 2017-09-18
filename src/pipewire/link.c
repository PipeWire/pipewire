/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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
#include <stdio.h>

#include <spa/lib/debug.h>
#include <spa/video/format.h>
#include <spa/pod-utils.h>

#include <spa/lib/format.h>
#include <spa/lib/props.h>

#include "pipewire.h"
#include "private.h"
#include "interfaces.h"
#include "link.h"
#include "work-queue.h"

#define MAX_BUFFERS     16

/** \cond */
struct impl {
	struct pw_link this;

	bool active;

	struct pw_work_queue *work;

	struct spa_format *format_filter;
	struct pw_properties *properties;

	struct spa_hook input_port_listener;
	struct spa_hook input_node_listener;
	struct spa_hook output_port_listener;
	struct spa_hook output_node_listener;
};

struct resource_data {
	struct spa_hook resource_listener;
};

/** \endcond */

static void pw_link_update_state(struct pw_link *link, enum pw_link_state state, char *error)
{
	enum pw_link_state old = link->state;

	if (state != old) {
		pw_log_debug("link %p: update state %s -> %s (%s)", link,
			     pw_link_state_as_string(old), pw_link_state_as_string(state), error);

		link->state = state;
		if (link->error)
			free(link->error);
		link->error = error;

		spa_hook_list_call(&link->listener_list, struct pw_link_events, state_changed, old, state, error);
	}
}

static void complete_ready(void *obj, void *data, int res, uint32_t id)
{
	struct pw_port *port = data;
	if (SPA_RESULT_IS_OK(res)) {
		port->state = PW_PORT_STATE_READY;
		pw_log_debug("port %p: state READY", port);
	} else {
		port->state = PW_PORT_STATE_ERROR;
		pw_log_warn("port %p: failed to go to READY", port);
	}
}

static void complete_paused(void *obj, void *data, int res, uint32_t id)
{
	struct pw_port *port = data;
	if (SPA_RESULT_IS_OK(res)) {
		port->state = PW_PORT_STATE_PAUSED;
		pw_log_debug("port %p: state PAUSED", port);
	} else {
		port->state = PW_PORT_STATE_ERROR;
		pw_log_warn("port %p: failed to go to PAUSED", port);
	}
}

static void complete_streaming(void *obj, void *data, int res, uint32_t id)
{
	struct pw_port *port = data;
	if (SPA_RESULT_IS_OK(res)) {
		port->state = PW_PORT_STATE_STREAMING;
		pw_log_debug("port %p: state STREAMING", port);
	} else {
		port->state = PW_PORT_STATE_ERROR;
		pw_log_warn("port %p: failed to go to STREAMING", port);
	}
}

static int do_negotiate(struct pw_link *this, uint32_t in_state, uint32_t out_state)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int res = SPA_RESULT_ERROR, res2;
	struct spa_format *format, *current;
	char *error = NULL;
	struct pw_resource *resource;
	bool changed = true;
	struct pw_port *input, *output;

	if (in_state != PW_PORT_STATE_CONFIGURE && out_state != PW_PORT_STATE_CONFIGURE)
		return SPA_RESULT_OK;

	pw_link_update_state(this, PW_LINK_STATE_NEGOTIATING, NULL);

	input = this->input;
	output = this->output;

	format = pw_core_find_format(this->core, output, input, NULL, 0, NULL, &error);
	if (format == NULL)
		goto error;

	format = spa_format_copy(format);

	if (out_state > PW_PORT_STATE_CONFIGURE && output->node->info.state == PW_NODE_STATE_IDLE) {
		if ((res = spa_node_port_get_format(output->node->node, output->direction, output->port_id,
						    (const struct spa_format **) &current)) < 0) {
			asprintf(&error, "error get output format: %d", res);
			goto error;
		}
		if (spa_format_compare(current, format) < 0) {
			pw_log_debug("link %p: output format change, renegotiate", this);
			pw_node_set_state(output->node, PW_NODE_STATE_SUSPENDED);
			out_state = PW_PORT_STATE_CONFIGURE;
		}
		else {
			pw_node_update_state(output->node, PW_NODE_STATE_RUNNING, NULL);
			changed = false;
		}
	}
	if (in_state > PW_PORT_STATE_CONFIGURE && input->node->info.state == PW_NODE_STATE_IDLE) {
		if ((res = spa_node_port_get_format(input->node->node, input->direction, input->port_id,
						    (const struct spa_format **) &current)) < 0) {
			asprintf(&error, "error get input format: %d", res);
			goto error;
		}
		if (spa_format_compare(current, format) < 0) {
			pw_log_debug("link %p: input format change, renegotiate", this);
			pw_node_set_state(input->node, PW_NODE_STATE_SUSPENDED);
			in_state = PW_PORT_STATE_CONFIGURE;
		}
		else {
			pw_node_update_state(input->node, PW_NODE_STATE_RUNNING, NULL);
			changed = false;
		}
	}

	pw_log_debug("link %p: doing set format %p", this, format);
	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_format(format);

	if (out_state == PW_PORT_STATE_CONFIGURE) {
		pw_log_debug("link %p: doing set format on output", this);
		if ((res = pw_port_set_format(output, SPA_PORT_FORMAT_FLAG_NEAREST, format)) < 0) {
			asprintf(&error, "error set output format: %d", res);
			goto error;
		}
		if (SPA_RESULT_IS_ASYNC(res))
			pw_work_queue_add(impl->work, output->node, res, complete_ready,
					  output);
	}
	if (in_state == PW_PORT_STATE_CONFIGURE) {
		pw_log_debug("link %p: doing set format on input", this);
		if ((res2 = pw_port_set_format(input, SPA_PORT_FORMAT_FLAG_NEAREST, format)) < 0) {
			asprintf(&error, "error set input format: %d", res2);
			goto error;
		}
		if (SPA_RESULT_IS_ASYNC(res2))
			pw_work_queue_add(impl->work, input->node, res2, complete_ready, input);
	}


	if (this->info.format)
		free(this->info.format);
	this->info.format = format;

	if (changed) {
		this->info.change_mask |= PW_LINK_CHANGE_MASK_FORMAT;

		spa_list_for_each(resource, &this->resource_list, link)
			pw_link_resource_info(resource, &this->info);

		this->info.change_mask = 0;
	}

	return SPA_RESULT_OK;

      error:
	pw_link_update_state(this, PW_LINK_STATE_ERROR, error);
	if (format)
		free(format);
	return res;
}

static struct spa_param *find_param(struct spa_param **params, int n_params, uint32_t type)
{
	uint32_t i;

	for (i = 0; i < n_params; i++) {
		if (spa_pod_is_object_type(&params[i]->object.pod, type))
			return params[i];
	}
	return NULL;
}

static struct spa_param *find_meta_enable(struct pw_core *core, struct spa_param **params,
					  int n_params, uint32_t type)
{
	uint32_t i;

	for (i = 0; i < n_params; i++) {
		if (spa_pod_is_object_type
		    (&params[i]->object.pod, core->type.param_alloc_meta_enable.MetaEnable)) {
			uint32_t qtype;

			if (spa_param_query(params[i],
					    core->type.param_alloc_meta_enable.type,
					    SPA_POD_TYPE_ID, &qtype, 0) != 1)
				continue;

			if (qtype == type)
				return params[i];
		}
	}
	return NULL;
}

static struct spa_buffer **alloc_buffers(struct pw_link *this,
					 uint32_t n_buffers,
					 uint32_t n_params,
					 struct spa_param **params,
					 uint32_t n_datas,
					 size_t *data_sizes,
					 ssize_t *data_strides,
					 struct pw_memblock *mem)
{
	struct spa_buffer **buffers, *bp;
	uint32_t i;
	size_t skel_size, data_size, meta_size;
	struct spa_chunk *cdp;
	void *ddp;
	uint32_t n_metas;
	struct spa_meta *metas;

	n_metas = data_size = meta_size = 0;

	/* each buffer */
	skel_size = sizeof(struct spa_buffer);

	metas = alloca(sizeof(struct spa_meta) * (n_params + 1));

	/* add shared metadata */
	metas[n_metas].type = this->core->type.meta.Shared;
	metas[n_metas].size = sizeof(struct spa_meta_shared);
	meta_size += metas[n_metas].size;
	n_metas++;
	skel_size += sizeof(struct spa_meta);

	/* collect metadata */
	for (i = 0; i < n_params; i++) {
		if (spa_pod_is_object_type
		    (&params[i]->object.pod, this->core->type.param_alloc_meta_enable.MetaEnable)) {
			uint32_t type, size;

			if (spa_param_query(params[i],
					    this->core->type.param_alloc_meta_enable.type,
					    SPA_POD_TYPE_ID, &type,
					    this->core->type.param_alloc_meta_enable.size,
					    SPA_POD_TYPE_INT, &size, 0) != 2)
				continue;

			pw_log_debug("link %p: enable meta %d %d", this, type, size);

			metas[n_metas].type = type;
			metas[n_metas].size = size;
			meta_size += metas[n_metas].size;
			n_metas++;
			skel_size += sizeof(struct spa_meta);
		}
	}
	data_size += meta_size;

	/* data */
	for (i = 0; i < n_datas; i++) {
		data_size += sizeof(struct spa_chunk);
		data_size += data_sizes[i];
		skel_size += sizeof(struct spa_data);
	}

	buffers = calloc(n_buffers, skel_size + sizeof(struct spa_buffer *));
	/* pointer to buffer structures */
	bp = SPA_MEMBER(buffers, n_buffers * sizeof(struct spa_buffer *), struct spa_buffer);

	pw_memblock_alloc(PW_MEMBLOCK_FLAG_WITH_FD |
			  PW_MEMBLOCK_FLAG_MAP_READWRITE |
			  PW_MEMBLOCK_FLAG_SEAL, n_buffers * data_size, mem);

	for (i = 0; i < n_buffers; i++) {
		int j;
		struct spa_buffer *b;
		void *p;

		buffers[i] = b = SPA_MEMBER(bp, skel_size * i, struct spa_buffer);

		p = SPA_MEMBER(mem->ptr, data_size * i, void);

		b->id = i;
		b->n_metas = n_metas;
		b->metas = SPA_MEMBER(b, sizeof(struct spa_buffer), struct spa_meta);
		for (j = 0; j < n_metas; j++) {
			struct spa_meta *m = &b->metas[j];

			m->type = metas[j].type;
			m->data = p;
			m->size = metas[j].size;

			if (m->type == this->core->type.meta.Shared) {
				struct spa_meta_shared *msh = p;

				msh->flags = 0;
				msh->fd = mem->fd;
				msh->offset = data_size * i;
				msh->size = data_size;
			} else if (m->type == this->core->type.meta.Ringbuffer) {
				struct spa_meta_ringbuffer *rb = p;
				spa_ringbuffer_init(&rb->ringbuffer, data_sizes[0]);
			}
			p += m->size;
		}
		/* pointer to data structure */
		b->n_datas = n_datas;
		b->datas = SPA_MEMBER(b->metas, n_metas * sizeof(struct spa_meta), struct spa_data);

		cdp = p;
		ddp = SPA_MEMBER(cdp, sizeof(struct spa_chunk) * n_datas, void);

		for (j = 0; j < n_datas; j++) {
			struct spa_data *d = &b->datas[j];

			d->chunk = &cdp[j];
			if (data_sizes[j] > 0) {
				d->type = this->core->type.data.MemFd;
				d->flags = 0;
				d->fd = mem->fd;
				d->mapoffset = SPA_PTRDIFF(ddp, mem->ptr);
				d->maxsize = data_sizes[j];
				d->data = SPA_MEMBER(mem->ptr, d->mapoffset, void);
				d->chunk->offset = 0;
				d->chunk->size = data_sizes[j];
				d->chunk->stride = data_strides[j];
				ddp += data_sizes[j];
			} else {
				d->type = SPA_ID_INVALID;
				d->data = NULL;
			}
		}
	}
	return buffers;
}

static int
param_filter(struct pw_link *this,
	     struct pw_port *in_port,
	     struct pw_port *out_port,
	     struct spa_pod_builder *result)
{
	int res;
	struct spa_param *oparam, *iparam;
	int iidx, oidx, num = 0;

	for (iidx = 0;; iidx++) {
		if (spa_node_port_enum_params(in_port->node->node, in_port->direction, in_port->port_id,
					      iidx, &iparam) < 0)
			break;

		for (oidx = 0;; oidx++) {
			struct spa_pod_frame f;
			uint32_t offset;

			if (spa_node_port_enum_params(out_port->node->node, out_port->direction,
						      out_port->port_id, oidx, &oparam) < 0)
				break;

			if (iidx == 0 && pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
				spa_debug_param(oparam);

			if (iparam->object.body.type != oparam->object.body.type)
				continue;

			offset = result->offset;
			spa_pod_builder_push_object(result, &f, 0, iparam->object.body.type);
			if ((res = spa_props_filter(result,
						    SPA_POD_CONTENTS(struct spa_param, iparam),
						    SPA_POD_CONTENTS_SIZE(struct spa_param, iparam),
						    SPA_POD_CONTENTS(struct spa_param, oparam),
						    SPA_POD_CONTENTS_SIZE(struct spa_param, oparam))) < 0) {
				result->offset = offset;
				result->stack = NULL;
				continue;
			}
			spa_pod_builder_pop(result, &f);
			num++;
		}
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_param(iparam);

	}
	return num;
}

static int do_allocation(struct pw_link *this, uint32_t in_state, uint32_t out_state)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int res;
	const struct spa_port_info *iinfo, *oinfo;
	uint32_t in_flags, out_flags;
	char *error = NULL;
	struct pw_port *input, *output;

	if (in_state != PW_PORT_STATE_READY && out_state != PW_PORT_STATE_READY)
		return SPA_RESULT_OK;

	pw_link_update_state(this, PW_LINK_STATE_ALLOCATING, NULL);

	input = this->input;
	output = this->output;

	pw_log_debug("link %p: doing alloc buffers %p %p", this, output->node, input->node);
	/* find out what's possible */
	if ((res = spa_node_port_get_info(output->node->node, output->direction, output->port_id,
					  &oinfo)) < 0) {
		asprintf(&error, "error get output port info: %d", res);
		goto error;
	}
	if ((res = spa_node_port_get_info(input->node->node, input->direction, input->port_id,
					  &iinfo)) < 0) {
		asprintf(&error, "error get input port info: %d", res);
		goto error;
	}

	in_flags = iinfo->flags;
	out_flags = oinfo->flags;

	if (out_flags & SPA_PORT_INFO_FLAG_LIVE) {
		pw_log_debug("setting link as live");
		output->node->live = true;
		input->node->live = true;
	}

	if (in_state == PW_PORT_STATE_READY && out_state == PW_PORT_STATE_READY) {
		if ((out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) &&
		    (in_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS)) {
			out_flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
			in_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
		} else if ((out_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) &&
			   (in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS)) {
			out_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
			in_flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
		} else if ((out_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) &&
			   (in_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS)) {
			out_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
			in_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
		} else if ((out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) &&
			   (in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS)) {
			out_flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
			in_flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
		} else {
			asprintf(&error, "no common buffer alloc found");
			res = SPA_RESULT_ERROR;
			goto error;
		}
	} else if (in_state == PW_PORT_STATE_READY && out_state > PW_PORT_STATE_READY) {
		out_flags &= ~SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
		in_flags &= ~SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
	} else if (out_state == PW_PORT_STATE_READY && in_state > PW_PORT_STATE_READY) {
		in_flags &= ~SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
		out_flags &= ~SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
	} else {
		pw_log_debug("link %p: delay allocation, state %d %d", this, in_state, out_state);
		return SPA_RESULT_OK;
	}

	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG)) {
		spa_debug_port_info(oinfo);
		spa_debug_port_info(iinfo);
	}

	if (this->buffers == NULL) {
		struct spa_param **params, *param;
		uint8_t buffer[4096];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		int i, offset, n_params;
		uint32_t max_buffers;
		size_t minsize = 1024, stride = 0;

		n_params = param_filter(this, input, output, &b);

		params = alloca(n_params * sizeof(struct spa_param *));
		for (i = 0, offset = 0; i < n_params; i++) {
			params[i] = SPA_MEMBER(buffer, offset, struct spa_param);
			spa_param_fixate(params[i]);
			pw_log_debug("fixated param %d:", i);
			if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
				spa_debug_param(params[i]);
			offset += SPA_ROUND_UP_N(SPA_POD_SIZE(params[i]), 8);
		}

		param = find_meta_enable(this->core, params, n_params,
					 this->core->type.meta.Ringbuffer);
		if (param) {
			uint32_t ms, s;
			max_buffers = 1;

			if (spa_param_query(param,
					    this->core->type.param_alloc_meta_enable.ringbufferSize,
					    SPA_POD_TYPE_INT, &ms,
					    this->core->type.param_alloc_meta_enable.
					    ringbufferStride, SPA_POD_TYPE_INT, &s, 0) == 2) {
				minsize = ms;
				stride = s;
			}
		} else {
			max_buffers = MAX_BUFFERS;
			minsize = stride = 0;
			param = find_param(params, n_params,
					   this->core->type.param_alloc_buffers.Buffers);
			if (param) {
				uint32_t qmax_buffers = max_buffers,
				    qminsize = minsize, qstride = stride;

				spa_param_query(param,
						this->core->type.param_alloc_buffers.size,
						SPA_POD_TYPE_INT, &qminsize,
						this->core->type.param_alloc_buffers.stride,
						SPA_POD_TYPE_INT, &qstride,
						this->core->type.param_alloc_buffers.buffers,
						SPA_POD_TYPE_INT, &qmax_buffers, 0);

				max_buffers =
				    qmax_buffers == 0 ? max_buffers : SPA_MIN(qmax_buffers,
									      max_buffers);
				minsize = SPA_MAX(minsize, qminsize);
				stride = SPA_MAX(stride, qstride);

				pw_log_debug("%d %d %d -> %zd %zd %d", qminsize, qstride, qmax_buffers,
					     minsize, stride, max_buffers);
			} else {
				pw_log_warn("no buffers param");
				minsize = 1024;
			}
		}

		if ((in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) ||
		    (out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS))
			minsize = 0;

		if (output->n_buffers) {
			out_flags = 0;
			in_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
			this->n_buffers = output->n_buffers;
			this->buffers = output->buffers;
			this->buffer_owner = output;
			pw_log_debug("link %p: reusing %d output buffers %p", this, this->n_buffers,
				     this->buffers);
		} else if (input->n_buffers && input->mix == NULL) {
			out_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
			in_flags = 0;
			this->n_buffers = input->n_buffers;
			this->buffers = input->buffers;
			this->buffer_owner = input;
			pw_log_debug("link %p: reusing %d input buffers %p", this, this->n_buffers,
				     this->buffers);
		} else {
			size_t data_sizes[1];
			ssize_t data_strides[1];

			data_sizes[0] = minsize;
			data_strides[0] = stride;

			this->buffer_owner = this;
			this->n_buffers = max_buffers;
			this->buffers = alloc_buffers(this,
						      this->n_buffers,
						      n_params,
						      params,
						      1,
						      data_sizes, data_strides,
						      &this->buffer_mem);

			pw_log_debug("link %p: allocating %d buffers %p %zd %zd", this,
				     this->n_buffers, this->buffers, minsize, stride);
		}

		if (out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) {
			if ((res = pw_port_alloc_buffers(output,
							 params, n_params,
							 this->buffers, &this->n_buffers)) < 0) {
				asprintf(&error, "error alloc output buffers: %d", res);
				goto error;
			}
			if (SPA_RESULT_IS_ASYNC(res))
				pw_work_queue_add(impl->work, output->node, res, complete_paused, output);
			output->buffer_mem = this->buffer_mem;
			this->buffer_owner = output;
			pw_log_debug("link %p: allocated %d buffers %p from output port", this,
				     this->n_buffers, this->buffers);
		} else if (in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) {
			if ((res = pw_port_alloc_buffers(input,
							 params, n_params,
							 this->buffers, &this->n_buffers)) < 0) {
				asprintf(&error, "error alloc input buffers: %d", res);
				goto error;
			}
			if (SPA_RESULT_IS_ASYNC(res))
				pw_work_queue_add(impl->work, input->node, res, complete_paused, input);
			input->buffer_mem = this->buffer_mem;
			this->buffer_owner = input;
			pw_log_debug("link %p: allocated %d buffers %p from input port", this,
				     this->n_buffers, this->buffers);
		}
	}

	if (in_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) {
		pw_log_debug("link %p: using %d buffers %p on input port", this,
			     this->n_buffers, this->buffers);
		if ((res = pw_port_use_buffers(input, this->buffers, this->n_buffers)) < 0) {
			asprintf(&error, "error use input buffers: %d", res);
			goto error;
		}
		if (SPA_RESULT_IS_ASYNC(res))
			pw_work_queue_add(impl->work, input->node, res, complete_paused, input);
	} else if (out_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) {
		pw_log_debug("link %p: using %d buffers %p on output port", this,
			     this->n_buffers, this->buffers);
		if ((res = pw_port_use_buffers(output, this->buffers, this->n_buffers)) < 0) {
			asprintf(&error, "error use output buffers: %d", res);
			goto error;
		}
		if (SPA_RESULT_IS_ASYNC(res))
			pw_work_queue_add(impl->work, output->node, res, complete_paused, output);
	} else {
		asprintf(&error, "no common buffer alloc found");
		goto error;
	}

	return SPA_RESULT_OK;

      error:
	output->buffers = NULL;
	output->n_buffers = 0;
	output->allocated = false;
	input->buffers = NULL;
	input->n_buffers = 0;
	input->allocated = false;
	pw_link_update_state(this, PW_LINK_STATE_ERROR, error);
	return res;
}

static int do_start(struct pw_link *this, uint32_t in_state, uint32_t out_state)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	char *error = NULL;
	int res;
	struct pw_port *input, *output;

	if (in_state < PW_PORT_STATE_PAUSED || out_state < PW_PORT_STATE_PAUSED)
		return SPA_RESULT_OK;

	pw_link_update_state(this, PW_LINK_STATE_PAUSED, NULL);

	input = this->input;
	output = this->output;

	if (in_state == PW_PORT_STATE_PAUSED) {
		if  ((res = pw_node_set_state(input->node, PW_NODE_STATE_RUNNING)) < 0) {
			asprintf(&error, "error starting input node: %d", res);
			goto error;
		}

		if (SPA_RESULT_IS_ASYNC(res))
			pw_work_queue_add(impl->work, input->node, res, complete_streaming, input);
		else
			complete_streaming(input->node, input, res, 0);
	}
	if (out_state == PW_PORT_STATE_PAUSED) {
		if ((res = pw_node_set_state(output->node, PW_NODE_STATE_RUNNING)) < 0) {
			asprintf(&error, "error starting output node: %d", res);
			goto error;
		}

		if (SPA_RESULT_IS_ASYNC(res))
			pw_work_queue_add(impl->work, output->node, res, complete_streaming, output);
		else
			complete_streaming(output->node, output, res, 0);
	}
	return SPA_RESULT_OK;

      error:
	pw_link_update_state(this, PW_LINK_STATE_ERROR, error);
	return res;
}

static int
do_activate_link(struct spa_loop *loop,
		 bool async, uint32_t seq, size_t size, const void *data, void *user_data)
{
        struct pw_link *this = user_data;
	spa_graph_port_link(&this->rt.out_port, &this->rt.in_port);
	return SPA_RESULT_OK;
}

static int check_states(struct pw_link *this, void *user_data, int res)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	uint32_t in_state, out_state;
	struct pw_port *input, *output;

	if (this->state == PW_LINK_STATE_ERROR)
		return SPA_RESULT_ERROR;

	input = this->input;
	output = this->output;

	if (input == NULL || output == NULL)
		return SPA_RESULT_OK;

	if (input->node->info.state == PW_NODE_STATE_ERROR ||
	    output->node->info.state == PW_NODE_STATE_ERROR)
		return SPA_RESULT_ERROR;

	in_state = input->state;
	out_state = output->state;

	pw_log_debug("link %p: input state %d, output state %d", this, in_state, out_state);

	if (in_state == PW_PORT_STATE_ERROR || out_state == PW_PORT_STATE_ERROR) {
		pw_link_update_state(this, PW_LINK_STATE_ERROR, NULL);
		return SPA_RESULT_ERROR;
	}

	if (in_state == PW_PORT_STATE_STREAMING && out_state == PW_PORT_STATE_STREAMING) {
		pw_loop_invoke(output->node->data_loop,
			       do_activate_link, SPA_ID_INVALID, 0, NULL, false, this);
		pw_link_update_state(this, PW_LINK_STATE_RUNNING, NULL);
		return SPA_RESULT_OK;
	}

	if ((res = do_negotiate(this, in_state, out_state)) != SPA_RESULT_OK)
		goto exit;

	if ((res = do_allocation(this, in_state, out_state)) != SPA_RESULT_OK)
		goto exit;

	if ((res = do_start(this, in_state, out_state)) != SPA_RESULT_OK)
		goto exit;

      exit:
	if (SPA_RESULT_IS_ERROR(res)) {
		pw_log_debug("link %p: got error result %d", this, res);
		return res;
	}

	pw_work_queue_add(impl->work,
			  this, SPA_RESULT_WAIT_SYNC, (pw_work_func_t) check_states, this);
	return res;
}

static void
input_node_async_complete(void *data, uint32_t seq, int res)
{
	struct impl *impl = data;
	struct pw_node *node = impl->this.input->node;

	pw_log_debug("link %p: node %p async complete %d %d", impl, node, seq, res);
	pw_work_queue_complete(impl->work, node, seq, res);
}

static void
output_node_async_complete(void *data, uint32_t seq, int res)
{
	struct impl *impl = data;
	struct pw_node *node = impl->this.output->node;

	pw_log_debug("link %p: node %p async complete %d %d", impl, node, seq, res);
	pw_work_queue_complete(impl->work, node, seq, res);
}

static void clear_port_buffers(struct pw_link *link, struct pw_port *port)
{
	if (link->buffer_owner != port)
		pw_port_use_buffers(port, NULL, 0);
}

static int
do_remove_input(struct spa_loop *loop,
	        bool async, uint32_t seq, size_t size, const void *data, void *user_data)
{
	struct pw_link *this = user_data;
	spa_graph_port_remove(&this->rt.in_port);
	return SPA_RESULT_OK;
}

static void input_remove(struct pw_link *this, struct pw_port *port)
{
	struct impl *impl = (struct impl *) this;

	pw_log_debug("link %p: remove input port %p", this, port);
	spa_hook_remove(&impl->input_port_listener);
	spa_hook_remove(&impl->input_node_listener);

	pw_loop_invoke(port->node->data_loop,
		       do_remove_input, 1, 0, NULL, true, this);

	clear_port_buffers(this, this->input);
}

static int
do_remove_output(struct spa_loop *loop,
	         bool async, uint32_t seq, size_t size, const void *data, void *user_data)
{
	struct pw_link *this = user_data;
	spa_graph_port_remove(&this->rt.out_port);
	return SPA_RESULT_OK;
}

static void output_remove(struct pw_link *this, struct pw_port *port)
{
	struct impl *impl = (struct impl *) this;

	pw_log_debug("link %p: remove output port %p", this, port);
	spa_hook_remove(&impl->output_port_listener);
	spa_hook_remove(&impl->output_node_listener);

	pw_loop_invoke(port->node->data_loop,
		       do_remove_output, 1, 0, NULL, true, this);

	clear_port_buffers(this, this->output);
}

static void on_port_destroy(struct pw_link *this, struct pw_port *port)
{
	struct pw_port *other;

	if (port == this->input) {
		input_remove(this, port);
		other = this->output;
	} else if (port == this->output) {
		output_remove(this, port);
		other = this->input;
	} else
		return;

	if (this->buffer_owner == port) {
		this->buffers = NULL;
		this->n_buffers = 0;

		pw_log_debug("link %p: clear allocated buffers on port %p", this, other);
		pw_port_use_buffers(other, NULL, 0);
		this->buffer_owner = NULL;
	}

	spa_hook_list_call(&this->listener_list, struct pw_link_events, port_unlinked, port);

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

bool pw_link_activate(struct pw_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);

	if (impl->active)
		return true;

	impl->active = true;

	pw_log_debug("link %p: activate", this);
	this->output->node->n_used_output_links++;
	this->input->node->n_used_input_links++;

	pw_work_queue_add(impl->work,
			  this, SPA_RESULT_WAIT_SYNC, (pw_work_func_t) check_states, this);

	return true;
}

static int
do_deactivate_link(struct spa_loop *loop,
		   bool async, uint32_t seq, size_t size, const void *data, void *user_data)
{
        struct pw_link *this = user_data;
	spa_graph_port_unlink(&this->rt.out_port);
	return SPA_RESULT_OK;
}

bool pw_link_deactivate(struct pw_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	struct pw_node *input_node, *output_node;

	if (!impl->active)
		return true;

	impl->active = false;
	pw_log_debug("link %p: deactivate", this);
	pw_loop_invoke(this->output->node->data_loop,
		       do_deactivate_link, SPA_ID_INVALID, 0, NULL, true, this);

	input_node = this->input->node;
	output_node = this->output->node;

	input_node->n_used_input_links--;
	output_node->n_used_output_links--;

	pw_log_debug("link %p: in %d %d, out %d %d, %d %d %d %d", this,
			input_node->n_used_input_links,
			input_node->n_used_output_links,
			output_node->n_used_input_links,
			output_node->n_used_output_links,
			input_node->idle_used_input_links,
			input_node->idle_used_output_links,
			output_node->idle_used_input_links,
			output_node->idle_used_output_links);

	if (input_node->n_used_input_links <= input_node->idle_used_input_links &&
	    input_node->n_used_output_links <= input_node->idle_used_output_links &&
	    input_node->info.state > PW_NODE_STATE_IDLE) {
		pw_node_update_state(input_node, PW_NODE_STATE_IDLE, NULL);
		this->input->state = PW_PORT_STATE_PAUSED;
	}

	if (output_node->n_used_input_links <= output_node->idle_used_input_links &&
	    output_node->n_used_output_links <= output_node->idle_used_output_links &&
	    output_node->info.state > PW_NODE_STATE_IDLE) {
		pw_node_update_state(output_node, PW_NODE_STATE_IDLE, NULL);
		this->output->state = PW_PORT_STATE_PAUSED;
	}

	return true;
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
link_bind_func(struct pw_global *global,
	       struct pw_client *client, uint32_t permissions,
	       uint32_t version, uint32_t id)
{
	struct pw_link *this = global->object;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	pw_resource_add_listener(resource, &data->resource_listener, &resource_events, resource);

	pw_log_debug("link %p: bound to %d", this, resource->id);

	spa_list_insert(this->resource_list.prev, &resource->link);

	this->info.change_mask = ~0;
	pw_link_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return SPA_RESULT_OK;

      no_mem:
	pw_log_error("can't create link resource");
	pw_core_resource_error(client->core_resource,
			       client->core_resource->id, SPA_RESULT_NO_MEMORY, "no memory");
	return SPA_RESULT_NO_MEMORY;
}

static int
do_add_link(struct spa_loop *loop,
            bool async, uint32_t seq, size_t size, const void *data, void *user_data)
{
        struct pw_link *this = user_data;
        struct pw_port *port = ((struct pw_port **) data)[0];

        if (port->direction == PW_DIRECTION_OUTPUT) {
                spa_graph_port_add(&port->rt.mix_node, &this->rt.out_port);
        } else {
                spa_graph_port_add(&port->rt.mix_node, &this->rt.in_port);
        }

        return SPA_RESULT_OK;
}

static const struct pw_port_events input_port_events = {
	PW_VERSION_PORT_EVENTS,
	.destroy = input_port_destroy,
};

static const struct pw_port_events output_port_events = {
	PW_VERSION_PORT_EVENTS,
	.destroy = output_port_destroy,
};

static const struct pw_node_events input_node_events = {
	PW_VERSION_NODE_EVENTS,
	.async_complete = input_node_async_complete,
};

static const struct pw_node_events output_node_events = {
	PW_VERSION_NODE_EVENTS,
	.async_complete = output_node_async_complete,
};

struct pw_link *pw_link_new(struct pw_core *core,
			    struct pw_port *output,
			    struct pw_port *input,
			    struct spa_format *format_filter,
			    struct pw_properties *properties,
			    char **error,
			    size_t user_data_size)
{
	struct impl *impl;
	struct pw_link *this;
	struct pw_node *input_node, *output_node;

	if (output == input)
		goto same_ports;

	if (pw_link_find(output, input))
		goto link_exists;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL)
		goto no_mem;

	this = &impl->this;
	pw_log_debug("link %p: new", this);

	if (user_data_size > 0)
                this->user_data = SPA_MEMBER(impl, sizeof(struct impl), void);

	impl->work = pw_work_queue_new(core->main_loop);

	this->core = core;
	this->properties = properties;
	this->state = PW_LINK_STATE_INIT;

	this->input = input;
	this->output = output;

	input_node = input->node;
	output_node = output->node;

	spa_list_init(&this->resource_list);
	spa_hook_list_init(&this->listener_list);

	impl->format_filter = format_filter;

	pw_port_add_listener(input, &impl->input_port_listener, &input_port_events, impl);
	pw_node_add_listener(input_node, &impl->input_node_listener, &input_node_events, impl);
	pw_port_add_listener(output, &impl->output_port_listener, &output_port_events, impl);
	pw_node_add_listener(output_node, &impl->output_node_listener, &output_node_events, impl);

	pw_log_debug("link %p: constructed %p:%d -> %p:%d", impl,
		     output_node, output->port_id, input_node, input->port_id);

	input_node->live = output_node->live;
	if (output_node->clock)
		input_node->clock = output_node->clock;

	pw_log_debug("link %p: output node %p clock %p, live %d", this, output_node, output_node->clock,
                             output_node->live);

	spa_list_insert(output->links.prev, &this->output_link);
	spa_list_insert(input->links.prev, &this->input_link);

	this->info.output_node_id = output_node->global->id;
	this->info.output_port_id = output->port_id;
	this->info.input_node_id = input_node->global->id;
	this->info.input_port_id = input->port_id;
	this->info.format = NULL;
	this->info.props = this->properties ? &this->properties->dict : NULL;

	spa_graph_port_init(&this->rt.out_port,
			    PW_DIRECTION_OUTPUT,
			    this->rt.out_port.port_id,
			    0,
			    &this->io);
	spa_graph_port_init(&this->rt.in_port,
			    PW_DIRECTION_INPUT,
			    this->rt.in_port.port_id,
			    0,
			    &this->io);

	this->rt.in_port.scheduler_data = this;
	this->rt.out_port.scheduler_data = this;

	/* nodes can be in different data loops so we do this twice */
	pw_loop_invoke(output_node->data_loop, do_add_link,
		       SPA_ID_INVALID, sizeof(struct pw_port *), &output, false, this);
	pw_loop_invoke(input_node->data_loop, do_add_link,
		       SPA_ID_INVALID, sizeof(struct pw_port *), &input, false, this);

	spa_hook_list_call(&output->listener_list, struct pw_port_events, link_added, this);
	spa_hook_list_call(&input->listener_list, struct pw_port_events, link_added, this);

	return this;

      same_ports:
	asprintf(error, "can't link the same ports");
	return NULL;
      link_exists:
	asprintf(error, "link already exists");
	return NULL;
      no_mem:
	asprintf(error, "no memory");
	return NULL;
}

void pw_link_register(struct pw_link *link,
		      struct pw_client *owner,
		      struct pw_global *parent)
{
	struct pw_core *core = link->core;

	spa_list_insert(core->link_list.prev, &link->link);
	link->global = pw_core_add_global(core, owner, parent, core->type.link, PW_VERSION_LINK,
			   link_bind_func, link);
	link->info.id = link->global->id;
}


void pw_link_destroy(struct pw_link *link)
{
	struct impl *impl = SPA_CONTAINER_OF(link, struct impl, this);
	struct pw_resource *resource, *tmp;

	pw_log_debug("link %p: destroy", impl);
	spa_hook_list_call(&link->listener_list, struct pw_link_events, destroy);

	pw_link_deactivate(link);

	if (link->global) {
		spa_list_remove(&link->link);
		pw_global_destroy(link->global);
	}

	spa_list_for_each_safe(resource, tmp, &link->resource_list, link)
	    pw_resource_destroy(resource);

	input_remove(link, link->input);
	spa_list_remove(&link->input_link);
	spa_hook_list_call(&link->input->listener_list, struct pw_port_events, link_removed, link);
	link->input = NULL;

	output_remove(link, link->output);
	spa_list_remove(&link->output_link);
	spa_hook_list_call(&link->output->listener_list, struct pw_port_events, link_removed, link);
	link->output = NULL;

	spa_hook_list_call(&link->listener_list, struct pw_link_events, free);

	pw_work_queue_destroy(impl->work);

	if (link->properties)
		pw_properties_free(link->properties);

	if (link->info.format)
		free(link->info.format);

	if (link->buffer_owner == link) {
		free(link->buffers);
		pw_memblock_free(&link->buffer_mem);
	}
	free(impl);
}

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

struct pw_core *pw_link_get_core(struct pw_link *link)
{
	return link->core;
}

void *pw_link_get_user_data(struct pw_link *link)
{
	return link->user_data;
}

const struct pw_link_info *pw_link_get_info(struct pw_link *link)
{
	return &link->info;
}

struct pw_global *pw_link_get_global(struct pw_link *link)
{
	return link->global;
}

struct pw_port *pw_link_get_output(struct pw_link *link)
{
	return link->output;
}

struct pw_port *pw_link_get_input(struct pw_link *link)
{
	return link->input;
}

void pw_link_inc_idle(struct pw_link *link)
{
	link->input->node->idle_used_input_links++;
	link->output->node->idle_used_output_links++;
}
