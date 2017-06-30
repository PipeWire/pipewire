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

#include <spa/lib/debug.h>
#include <spa/video/format.h>
#include <spa/pod-utils.h>

#include <spa/lib/format.h>
#include <spa/lib/props.h>

#include "pipewire/client/pipewire.h"
#include "pipewire/client/interfaces.h"

#include "pipewire/server/link.h"
#include "pipewire/server/work-queue.h"

#define MAX_BUFFERS     16

/** \cond */
struct impl {
	struct pw_link this;

	bool active;

	struct pw_work_queue *work;

	struct spa_format *format_filter;
	struct pw_properties *properties;

	struct pw_listener input_port_destroy;
	struct pw_listener input_async_complete;
	struct pw_listener output_port_destroy;
	struct pw_listener output_async_complete;

	void *buffer_owner;
	struct pw_memblock buffer_mem;
	struct spa_buffer **buffers;
	uint32_t n_buffers;
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

		pw_signal_emit(&link->state_changed, link, old, state);
	}
}

static void complete_ready(void *obj, void *data, int res, uint32_t id)
{
	struct pw_port *port = data;
	if (SPA_RESULT_IS_OK(res)) {
		port->state = PW_PORT_STATE_READY;
		pw_log_debug("port %p: state READY", port);
	} else
		pw_log_warn("port %p: failed to go to READY", port);
}

static void complete_paused(void *obj, void *data, int res, uint32_t id)
{
	struct pw_port *port = data;
	if (SPA_RESULT_IS_OK(res)) {
		port->state = PW_PORT_STATE_PAUSED;
		pw_log_debug("port %p: state PAUSED", port);
	} else
		pw_log_warn("port %p: failed to go to PAUSED", port);
}

static void complete_streaming(void *obj, void *data, int res, uint32_t id)
{
	struct pw_port *port = data;
	if (SPA_RESULT_IS_OK(res)) {
		port->state = PW_PORT_STATE_STREAMING;
		pw_log_debug("port %p: state STREAMING", port);
	} else
		pw_log_warn("port %p: failed to go to STREAMING", port);
}

static int do_negotiate(struct pw_link *this, uint32_t in_state, uint32_t out_state)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int res = SPA_RESULT_ERROR, res2;
	struct spa_format *format, *current;
	char *error = NULL;

	if (in_state != PW_PORT_STATE_CONFIGURE && out_state != PW_PORT_STATE_CONFIGURE)
		return SPA_RESULT_OK;

	pw_link_update_state(this, PW_LINK_STATE_NEGOTIATING, NULL);

	format = pw_core_find_format(this->core, this->output, this->input, NULL, 0, NULL, &error);
	if (format == NULL)
		goto error;

	format = spa_format_copy(format);

	if (out_state > PW_PORT_STATE_CONFIGURE && this->output->node->info.state == PW_NODE_STATE_IDLE) {
		if ((res = spa_node_port_get_format(this->output->node->node,
						    SPA_DIRECTION_OUTPUT,
						    this->output->port_id,
						    (const struct spa_format **) &current)) < 0) {
			asprintf(&error, "error get output format: %d", res);
			goto error;
		}
		if (spa_format_compare(current, format) < 0) {
			pw_log_debug("link %p: output format change, renegotiate", this);
			pw_node_set_state(this->output->node, PW_NODE_STATE_SUSPENDED);
			out_state = PW_PORT_STATE_CONFIGURE;
		}
		else
			pw_node_update_state(this->output->node, PW_NODE_STATE_RUNNING, NULL);
	}
	if (in_state > PW_PORT_STATE_CONFIGURE && this->input->node->info.state == PW_NODE_STATE_IDLE) {
		if ((res = spa_node_port_get_format(this->input->node->node,
						    SPA_DIRECTION_INPUT,
						    this->input->port_id,
						    (const struct spa_format **) &current)) < 0) {
			asprintf(&error, "error get input format: %d", res);
			goto error;
		}
		if (spa_format_compare(current, format) < 0) {
			pw_log_debug("link %p: input format change, renegotiate", this);
			pw_node_set_state(this->input->node, PW_NODE_STATE_SUSPENDED);
			in_state = PW_PORT_STATE_CONFIGURE;
		}
		else
			pw_node_update_state(this->input->node, PW_NODE_STATE_RUNNING, NULL);
	}

	pw_log_debug("link %p: doing set format", this);
	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_format(format);

	if (out_state == PW_PORT_STATE_CONFIGURE) {
		pw_log_debug("link %p: doing set format on output", this);
		if ((res = pw_port_set_format(this->output, SPA_PORT_FORMAT_FLAG_NEAREST, format)) < 0) {
			asprintf(&error, "error set output format: %d", res);
			goto error;
		}
		if (SPA_RESULT_IS_ASYNC(res))
			pw_work_queue_add(impl->work, this->output->node, res, complete_ready,
					  this->output);
	}
	if (in_state == PW_PORT_STATE_CONFIGURE) {
		pw_log_debug("link %p: doing set format on input", this);
		if ((res2 = pw_port_set_format(this->input, SPA_PORT_FORMAT_FLAG_NEAREST, format)) < 0) {
			asprintf(&error, "error set input format: %d", res2);
			goto error;
		}
		if (SPA_RESULT_IS_ASYNC(res2))
			pw_work_queue_add(impl->work, this->input->node, res2, complete_ready, this->input);
	}

	if (this->info.format)
		free(this->info.format);
	this->info.format = format;

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

	metas = alloca(sizeof(struct spa_meta) * n_params + 1);

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
spa_node_param_filter(struct pw_link *this,
		      struct spa_node *in_node,
		      uint32_t in_port,
		      struct spa_node *out_node, uint32_t out_port, struct spa_pod_builder *result)
{
	int res;
	struct spa_param *oparam, *iparam;
	int iidx, oidx, num = 0;

	for (iidx = 0;; iidx++) {
		if (spa_node_port_enum_params(in_node, SPA_DIRECTION_INPUT, in_port, iidx, &iparam)
		    < 0)
			break;

		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_param(iparam);

		for (oidx = 0;; oidx++) {
			struct spa_pod_frame f;
			uint32_t offset;

			if (spa_node_port_enum_params
			    (out_node, SPA_DIRECTION_OUTPUT, out_port, oidx, &oparam) < 0)
				break;

			if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
				spa_debug_param(oparam);

			if (iparam->object.body.type != oparam->object.body.type)
				continue;

			offset = result->offset;
			spa_pod_builder_push_object(result, &f, 0, iparam->object.body.type);
			if ((res = spa_props_filter(result,
						    SPA_POD_CONTENTS(struct spa_param, iparam),
						    SPA_POD_CONTENTS_SIZE(struct spa_param, iparam),
						    SPA_POD_CONTENTS(struct spa_param, oparam),
						    SPA_POD_CONTENTS_SIZE(struct spa_param,
									  oparam))) < 0) {
				result->offset = offset;
				result->stack = NULL;
				continue;
			}
			spa_pod_builder_pop(result, &f);
			num++;
		}
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

	if (in_state != PW_PORT_STATE_READY && out_state != PW_PORT_STATE_READY)
		return SPA_RESULT_OK;

	pw_link_update_state(this, PW_LINK_STATE_ALLOCATING, NULL);

	pw_log_debug("link %p: doing alloc buffers %p %p", this, this->output->node,
		     this->input->node);
	/* find out what's possible */
	if ((res = spa_node_port_get_info(this->output->node->node,
					  SPA_DIRECTION_OUTPUT,
					  this->output->port_id, &oinfo)) < 0) {
		asprintf(&error, "error get output port info: %d", res);
		goto error;
	}
	if ((res = spa_node_port_get_info(this->input->node->node,
					  SPA_DIRECTION_INPUT, this->input->port_id, &iinfo)) < 0) {
		asprintf(&error, "error get input port info: %d", res);
		goto error;
	}

	in_flags = iinfo->flags;
	out_flags = oinfo->flags;

	if (out_flags & SPA_PORT_INFO_FLAG_LIVE) {
		pw_log_debug("setting link as live");
		this->output->node->live = true;
		this->input->node->live = true;
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

	if (impl->buffers == NULL) {
		struct spa_param **params, *param;
		uint8_t buffer[4096];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		int i, offset, n_params;
		uint32_t max_buffers;
		size_t minsize = 1024, stride = 0;

		n_params = spa_node_param_filter(this,
						 this->input->node->node,
						 this->input->port_id,
						 this->output->node->node,
						 this->output->port_id, &b);

		params = alloca(n_params * sizeof(struct spa_param *));
		for (i = 0, offset = 0; i < n_params; i++) {
			params[i] = SPA_MEMBER(buffer, offset, struct spa_param);
			spa_param_fixate(params[i]);
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
			} else {
				minsize = 4096;
			}
		}

		if ((in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) ||
		    (out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS))
			minsize = 0;

		if (this->output->n_buffers) {
			out_flags = 0;
			in_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
			impl->n_buffers = this->output->n_buffers;
			impl->buffers = this->output->buffers;
			impl->buffer_owner = this->output;
			pw_log_debug("reusing %d output buffers %p", impl->n_buffers,
				     impl->buffers);
		} else if (this->input->n_buffers) {
			out_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
			in_flags = 0;
			impl->n_buffers = this->input->n_buffers;
			impl->buffers = this->input->buffers;
			impl->buffer_owner = this->input;
			pw_log_debug("reusing %d input buffers %p", impl->n_buffers, impl->buffers);
		} else {
			size_t data_sizes[1];
			ssize_t data_strides[1];

			data_sizes[0] = minsize;
			data_strides[0] = stride;

			impl->buffer_owner = this;
			impl->n_buffers = max_buffers;
			impl->buffers = alloc_buffers(this,
						      impl->n_buffers,
						      n_params,
						      params,
						      1,
						      data_sizes, data_strides, &impl->buffer_mem);

			pw_log_debug("allocating %d input buffers %p %zd %zd", impl->n_buffers,
				     impl->buffers, minsize, stride);
		}

		if (out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) {
			if ((res = pw_port_alloc_buffers(this->output, params, n_params,
							 impl->buffers, &impl->n_buffers)) < 0) {
				asprintf(&error, "error alloc output buffers: %d", res);
				goto error;
			}
			if (SPA_RESULT_IS_ASYNC(res))
				pw_work_queue_add(impl->work, this->output->node, res, complete_paused,
						  this->output);
			this->output->buffer_mem = impl->buffer_mem;
			impl->buffer_owner = this->output;
			pw_log_debug("allocated %d buffers %p from output port", impl->n_buffers,
				     impl->buffers);
		} else if (in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) {
			if ((res = pw_port_alloc_buffers(this->input, params, n_params,
							 impl->buffers, &impl->n_buffers)) < 0) {
				asprintf(&error, "error alloc input buffers: %d", res);
				goto error;
			}
			if (SPA_RESULT_IS_ASYNC(res))
				pw_work_queue_add(impl->work, this->input->node, res, complete_paused,
						  this->input);
			this->input->buffer_mem = impl->buffer_mem;
			impl->buffer_owner = this->input;
			pw_log_debug("allocated %d buffers %p from input port", impl->n_buffers,
				     impl->buffers);
		}
	}

	if (in_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) {
		pw_log_debug("using %d buffers %p on input port", impl->n_buffers, impl->buffers);
		if ((res = pw_port_use_buffers(this->input, impl->buffers, impl->n_buffers)) < 0) {
			asprintf(&error, "error use input buffers: %d", res);
			goto error;
		}
		if (SPA_RESULT_IS_ASYNC(res))
			pw_work_queue_add(impl->work, this->input->node, res, complete_paused, this->input);
	} else if (out_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) {
		pw_log_debug("using %d buffers %p on output port", impl->n_buffers, impl->buffers);
		if ((res = pw_port_use_buffers(this->output, impl->buffers, impl->n_buffers)) < 0) {
			asprintf(&error, "error use output buffers: %d", res);
			goto error;
		}
		if (SPA_RESULT_IS_ASYNC(res))
			pw_work_queue_add(impl->work, this->output->node, res, complete_paused, this->output);
	} else {
		asprintf(&error, "no common buffer alloc found");
		goto error;
	}

	return SPA_RESULT_OK;

      error:
	this->output->buffers = NULL;
	this->output->n_buffers = 0;
	this->output->allocated = false;
	this->input->buffers = NULL;
	this->input->n_buffers = 0;
	this->input->allocated = false;
	pw_link_update_state(this, PW_LINK_STATE_ERROR, error);
	return res;
}

static int do_start(struct pw_link *this, uint32_t in_state, uint32_t out_state)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	char *error = NULL;
	int res;

	if (in_state < PW_PORT_STATE_PAUSED || out_state < PW_PORT_STATE_PAUSED)
		return SPA_RESULT_OK;

	pw_link_update_state(this, PW_LINK_STATE_PAUSED, NULL);

	if (in_state == PW_PORT_STATE_PAUSED) {
		if  ((res = pw_node_set_state(this->input->node, PW_NODE_STATE_RUNNING)) < 0) {
			asprintf(&error, "error starting input node: %d", res);
			goto error;
		}

		if (SPA_RESULT_IS_ASYNC(res))
			pw_work_queue_add(impl->work, this->input->node, res, complete_streaming,
					  this->input);
		else
			complete_streaming(this->input->node, this->input, res, 0);
	}
	if (out_state == PW_PORT_STATE_PAUSED) {
		if ((res = pw_node_set_state(this->output->node, PW_NODE_STATE_RUNNING)) < 0) {
			asprintf(&error, "error starting output node: %d", res);
			goto error;
		}

		if (SPA_RESULT_IS_ASYNC(res))
			pw_work_queue_add(impl->work, this->output->node, res, complete_streaming,
					  this->output);
		else
			complete_streaming(this->output->node, this->output, res, 0);
	}
	return SPA_RESULT_OK;

      error:
	pw_link_update_state(this, PW_LINK_STATE_ERROR, error);
	return res;
}

static int check_states(struct pw_link *this, void *user_data, int res)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	uint32_t in_state, out_state;

	if (this->state == PW_LINK_STATE_ERROR)
		return SPA_RESULT_ERROR;

	if (this->input == NULL || this->output == NULL)
		return SPA_RESULT_OK;

	if (this->input->node->info.state == PW_NODE_STATE_ERROR ||
	    this->output->node->info.state == PW_NODE_STATE_ERROR)
		return SPA_RESULT_ERROR;

	in_state = this->input->state;
	out_state = this->output->state;

	pw_log_debug("link %p: input state %d, output state %d", this, in_state, out_state);

	if (in_state == PW_PORT_STATE_STREAMING && out_state == PW_PORT_STATE_STREAMING) {
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
on_input_async_complete_notify(struct pw_listener *listener,
			       struct pw_node *node, uint32_t seq, int res)
{
	struct impl *impl = SPA_CONTAINER_OF(listener, struct impl, input_async_complete);

	pw_log_debug("link %p: node %p async complete %d %d", impl, node, seq, res);
	pw_work_queue_complete(impl->work, node, seq, res);
}

static void
on_output_async_complete_notify(struct pw_listener *listener,
				struct pw_node *node, uint32_t seq, int res)
{
	struct impl *impl = SPA_CONTAINER_OF(listener, struct impl, output_async_complete);

	pw_log_debug("link %p: node %p async complete %d %d", impl, node, seq, res);
	pw_work_queue_complete(impl->work, node, seq, res);
}

static int
do_remove_input(struct spa_loop *loop,
	        bool async, uint32_t seq, size_t size, void *data, void *user_data)
{
	struct pw_link *this = user_data;
	struct pw_port *port = ((struct pw_port **) data)[0];
	spa_graph_port_remove(port->rt.graph, &this->rt.in_port);
	return SPA_RESULT_OK;
}

static void input_remove(struct pw_link *this, struct pw_port *port)
{
	struct impl *impl = (struct impl *) this;

	pw_log_debug("link %p: remove input port %p", this, port);
	pw_signal_remove(&impl->input_port_destroy);
	pw_signal_remove(&impl->input_async_complete);
	pw_loop_invoke(port->node->data_loop->loop,
		       do_remove_input, 1, sizeof(struct pw_port*), &port, true, this);
}

static int
do_remove_output(struct spa_loop *loop,
	         bool async, uint32_t seq, size_t size, void *data, void *user_data)
{
	struct pw_link *this = user_data;
	struct pw_port *port = ((struct pw_port **) data)[0];
	spa_graph_port_remove(port->rt.graph, &this->rt.out_port);
	return SPA_RESULT_OK;
}

static void output_remove(struct pw_link *this, struct pw_port *port)
{
	struct impl *impl = (struct impl *) this;

	pw_log_debug("link %p: remove output port %p", this, port);
	pw_signal_remove(&impl->output_port_destroy);
	pw_signal_remove(&impl->output_async_complete);
	pw_loop_invoke(port->node->data_loop->loop,
		       do_remove_output, 1, sizeof(struct pw_port*), &port, true, this);
}

static void on_port_destroy(struct pw_link *this, struct pw_port *port)
{
	struct impl *impl = (struct impl *) this;
	struct pw_port *other;

	if (port == this->input) {
		input_remove(this, port);
		this->input = NULL;
		other = this->output;
	} else if (port == this->output) {
		output_remove(this, port);
		this->output = NULL;
		other = this->input;
	} else
		return;

	if (impl->buffer_owner == port) {
		impl->buffers = NULL;
		impl->n_buffers = 0;

		pw_log_debug("link %p: clear input allocated buffers on port %p", this, other);
		pw_port_use_buffers(other, NULL, 0);
		impl->buffer_owner = NULL;
	}

	pw_signal_emit(&this->port_unlinked, this, port);

	pw_link_update_state(this, PW_LINK_STATE_UNLINKED, NULL);
	pw_link_destroy(this);
}

static void on_input_port_destroy(struct pw_listener *listener, struct pw_port *port)
{
	struct impl *impl = SPA_CONTAINER_OF(listener, struct impl, input_port_destroy);

	on_port_destroy(&impl->this, port);
}

static void on_output_port_destroy(struct pw_listener *listener, struct pw_port *port)
{
	struct impl *impl = SPA_CONTAINER_OF(listener, struct impl, output_port_destroy);

	on_port_destroy(&impl->this, port);
}

bool pw_link_activate(struct pw_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);

	if (impl->active)
		return true;

	impl->active = true;

	pw_log_debug("link %p: activate", this);
	pw_work_queue_add(impl->work,
			  this, SPA_RESULT_WAIT_SYNC, (pw_work_func_t) check_states, this);
	return true;
}

bool pw_link_deactivate(struct pw_link *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	impl->active = false;
	return true;
}

static void link_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	spa_list_remove(&resource->link);
}

static int
link_bind_func(struct pw_global *global, struct pw_client *client, uint32_t version, uint32_t id)
{
	struct pw_link *this = global->object;
	struct pw_resource *resource;

	resource = pw_resource_new(client, id, global->type, 0);

	if (resource == NULL)
		goto no_mem;

	pw_resource_set_implementation(resource, global->object, PW_VERSION_LINK, NULL, link_unbind_func);

	pw_log_debug("link %p: bound to %d", global->object, resource->id);

	spa_list_insert(this->resource_list.prev, &resource->link);

	this->info.change_mask = ~0;
	pw_link_notify_info(resource, &this->info);

	return SPA_RESULT_OK;

      no_mem:
	pw_log_error("can't create link resource");
	pw_core_notify_error(client->core_resource,
			     client->core_resource->id, SPA_RESULT_NO_MEMORY, "no memory");
	return SPA_RESULT_NO_MEMORY;
}

static int
do_add_link(struct spa_loop *loop,
            bool async, uint32_t seq, size_t size, void *data, void *user_data)
{
        struct pw_link *this = user_data;
        struct pw_port *port = ((struct pw_port **) data)[0];

        if (port->direction == PW_DIRECTION_OUTPUT) {
                spa_graph_port_add(port->rt.graph,
                                   &port->rt.mix_node,
                                   &this->rt.out_port,
                                   PW_DIRECTION_OUTPUT,
                                   this->rt.out_port.port_id,
                                   0,
                                   &this->io);
        } else {
                spa_graph_port_add(port->rt.graph,
                                   &port->rt.mix_node,
                                   &this->rt.in_port,
                                   PW_DIRECTION_INPUT,
                                   this->rt.in_port.port_id,
                                   0,
                                   &this->io);
        }

        return SPA_RESULT_OK;
}

struct pw_link *pw_link_new(struct pw_core *core,
			    struct pw_port *output,
			    struct pw_port *input,
			    struct spa_format *format_filter,
			    struct pw_properties *properties,
			    char **error)
{
	struct impl *impl;
	struct pw_link *this;
	struct pw_node *input_node, *output_node;

	if (output == input)
		goto same_ports;

	if (pw_link_find(output, input))
		goto link_exists;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto no_mem;

	this = &impl->this;
	pw_log_debug("link %p: new", this);

	impl->work = pw_work_queue_new(core->main_loop->loop);

	this->core = core;
	this->properties = properties;
	this->state = PW_LINK_STATE_INIT;

	this->input = input;
	this->output = output;

	input_node = input->node;
	output_node = output->node;

	spa_list_init(&this->resource_list);
	pw_signal_init(&this->port_unlinked);
	pw_signal_init(&this->state_changed);
	pw_signal_init(&this->destroy_signal);

	impl->format_filter = format_filter;

	pw_signal_add(&input->destroy_signal,
		      &impl->input_port_destroy, on_input_port_destroy);

	pw_signal_add(&input_node->async_complete,
		      &impl->input_async_complete, on_input_async_complete_notify);

	pw_signal_add(&output->destroy_signal,
		      &impl->output_port_destroy, on_output_port_destroy);

	pw_signal_add(&output_node->async_complete,
		      &impl->output_async_complete, on_output_async_complete_notify);

	pw_log_debug("link %p: constructed %p:%d -> %p:%d", impl,
		     output_node, output->port_id, input_node, input->port_id);

	input_node->live = output_node->live;
	if (output_node->clock)
		input_node->clock = output_node->clock;

	pw_log_debug("link %p: output node %p clock %p, live %d", this, output_node, output_node->clock,
                             output_node->live);

	spa_list_insert(output->links.prev, &this->output_link);
	spa_list_insert(input->links.prev, &this->input_link);

	output_node->n_used_output_links++;
	input_node->n_used_input_links++;

	spa_list_insert(core->link_list.prev, &this->link);

	pw_core_add_global(core, NULL, core->type.link, 0, this, link_bind_func, &this->global);

	this->info.id = this->global->id;
	this->info.output_node_id = output ? output->node->global->id : -1;
	this->info.output_port_id = output ? output->port_id : -1;
	this->info.input_node_id = input ? input->node->global->id : -1;
	this->info.input_port_id = input ? input->port_id : -1;
	this->info.format = NULL;

	spa_graph_port_link(output_node->rt.sched->graph, &this->rt.out_port, &this->rt.in_port);

	pw_loop_invoke(output_node->data_loop->loop,
		       do_add_link,
		       SPA_ID_INVALID, sizeof(struct pw_port *), &output, false, this);
	pw_loop_invoke(input_node->data_loop->loop,
		       do_add_link,
		       SPA_ID_INVALID, sizeof(struct pw_port *), &input, false, this);

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

static void clear_port_buffers(struct pw_link *link, struct pw_port *port)
{
	struct impl *impl = SPA_CONTAINER_OF(link, struct impl, this);

	if (impl->buffer_owner != port)
		pw_port_use_buffers(port, NULL, 0);
}


void pw_link_destroy(struct pw_link *link)
{
	struct impl *impl = SPA_CONTAINER_OF(link, struct impl, this);
	struct pw_resource *resource, *tmp;

	pw_log_debug("link %p: destroy", impl);
	pw_signal_emit(&link->destroy_signal, link);

	pw_global_destroy(link->global);
	spa_list_remove(&link->link);

	spa_list_for_each_safe(resource, tmp, &link->resource_list, link)
	    pw_resource_destroy(resource);

	if (link->input) {
		input_remove(link, link->input);

		spa_list_remove(&link->input_link);
		link->input->node->n_used_input_links--;

		clear_port_buffers(link, link->input);

		if (link->input->node->n_used_input_links == 0 &&
		    link->input->node->n_used_output_links == 0 &&
		    link->input->node->info.state > PW_NODE_STATE_IDLE)
			pw_node_update_state(link->input->node, PW_NODE_STATE_IDLE, NULL);

		link->input = NULL;
	}
	if (link->output) {
		output_remove(link, link->output);

		spa_list_remove(&link->output_link);
		link->output->node->n_used_output_links--;

		clear_port_buffers(link, link->output);

		if (link->output->node->n_used_input_links == 0 &&
		    link->output->node->n_used_output_links == 0 &&
		    link->output->node->info.state > PW_NODE_STATE_IDLE)
			pw_node_update_state(link->output->node, PW_NODE_STATE_IDLE, NULL);

		link->output = NULL;
	}

	pw_work_queue_destroy(impl->work);

	if (link->info.format)
		free(link->info.format);

	if (impl->buffer_owner == link)
		pw_memblock_free(&impl->buffer_mem);

	free(impl);
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
