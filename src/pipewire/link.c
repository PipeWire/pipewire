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

#include <spa/pod/parser.h>
#include <spa/pod/compare.h>
#include <spa/param/param.h>

#include "private.h"
#include "pipewire.h"
#include "interfaces.h"
#include "control.h"
#include "link.h"
#include "work-queue.h"

#undef spa_debug
#include <spa/debug/node.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>

#define MAX_BUFFERS     64

/** \cond */
struct impl {
	struct pw_link this;

	bool prepare;
	bool have_io;
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

	pw_link_events_state_changed(link, old, state, error);

	link->info.change_mask |= PW_LINK_CHANGE_MASK_STATE;
	pw_link_events_info_changed(link, &link->info);

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
	struct pw_port_mix *mix = data;
	struct pw_port *port = mix->p;

	if (SPA_RESULT_IS_OK(res)) {
		pw_port_mix_update_state(port, mix, PW_PORT_STATE_READY);
		pw_port_update_state(port, PW_PORT_STATE_READY);
		pw_log_debug("port %p: state READY", port);
	} else {
		pw_port_mix_update_state(port, mix, PW_PORT_STATE_ERROR);
		pw_port_update_state(port, PW_PORT_STATE_ERROR);
		pw_log_warn("port %p: failed to go to READY", port);
	}
}

static void complete_paused(void *obj, void *data, int res, uint32_t id)
{
	struct pw_port_mix *mix = data;
	struct pw_port *port = mix->p;

	if (SPA_RESULT_IS_OK(res)) {
		pw_port_mix_update_state(port, mix, PW_PORT_STATE_PAUSED);
		pw_port_update_state(port, PW_PORT_STATE_PAUSED);
		pw_log_debug("port %p: state PAUSED", port);
	} else {
		pw_port_mix_update_state(port, mix, PW_PORT_STATE_ERROR);
		pw_port_update_state(port, PW_PORT_STATE_ERROR);
		pw_log_warn("port %p: failed to go to PAUSED", port);
	}
}

static int do_negotiate(struct pw_link *this, uint32_t in_state, uint32_t out_state)
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
	uint32_t in_mix_state, out_mix_state;

	in_mix_state = this->rt.in_mix.state;
	out_mix_state = this->rt.out_mix.state;

	pw_log_debug("%d %d", in_mix_state, out_mix_state);

	if (in_mix_state != PW_PORT_STATE_CONFIGURE && out_mix_state != PW_PORT_STATE_CONFIGURE)
		return 0;

	pw_link_update_state(this, PW_LINK_STATE_NEGOTIATING, NULL);

	input = this->input;
	output = this->output;

	if ((res = pw_core_find_format(this->core, output, input, NULL, 0, NULL, &format, &b, &error)) < 0)
		goto error;

	in_state = input->state;
	out_state = output->state;

	format = pw_spa_pod_copy(format);
	spa_pod_fixate(format);

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	if (out_state > PW_PORT_STATE_CONFIGURE && output->node->info.state == PW_NODE_STATE_IDLE) {
		index = 0;
		res = spa_node_port_enum_params(output->node->node,
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
		res = spa_node_port_enum_params(input->node->node,
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

	if (out_mix_state == PW_PORT_STATE_CONFIGURE) {
		pw_log_debug("link %p: doing set format on output mix", this);
		if ((res = pw_port_set_param(output,
					     this->rt.out_mix.port.port_id,
					     SPA_PARAM_Format, SPA_NODE_PARAM_FLAG_NEAREST,
					     format)) < 0) {
			asprintf(&error, "error set output format: %d (%s)", res, spa_strerror(res));
			goto error;
		}
		if (SPA_RESULT_IS_ASYNC(res))
			pw_work_queue_add(impl->work, output->node, res, complete_ready,
					  &this->rt.out_mix);
	}
	if (in_mix_state == PW_PORT_STATE_CONFIGURE) {
		pw_log_debug("link %p: doing set format on input mix", this);
		if ((res2 = pw_port_set_param(input,
					      this->rt.in_mix.port.port_id,
					      SPA_PARAM_Format, SPA_NODE_PARAM_FLAG_NEAREST,
					      format)) < 0) {
			asprintf(&error, "error set input format: %d (%s)", res2, spa_strerror(res2));
			goto error;
		}
		if (SPA_RESULT_IS_ASYNC(res2)) {
			pw_work_queue_add(impl->work, input->node, res2, complete_ready,
					  &this->rt.in_mix);
			if (res == 0)
				res = res2;
		}
	}

	free(this->info.format);
	this->info.format = format;

	if (changed) {
		this->info.change_mask |= PW_LINK_CHANGE_MASK_FORMAT;

		pw_link_events_info_changed(this, &this->info);

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

/* Allocate an array of buffers that can be shared.
 *
 * All information will be allocated in \a mem. A pointer to a
 * newly allocated memory with an array of buffer pointers is
 * returned.
 *
 * \return an array of freshly allocated buffer pointers. free()
 *         after usage.
 *
 * Array of allocated buffers:
 *
 *      +==============================+
 *    +-| struct spa_buffer  *         | array of n_buffers of pointers
 *    | | ... <n_buffers>              |
 *    | +==============================+
 *    +>| struct spa_buffer            |
 *      |   uint32_t id                | id of buffer
 *      |   uint32_t n_metas           | number of metas
 *    +-|   struct spa_meta *metas     | pointer to array of metas
 *    | |   uint32_t n_datas           | number of datas
 *   +|-|   struct spa_data *datas     | pointer to array of datas
 *   || +------------------------------+
 *   |+>| struct spa_meta              |
 *   |  |   uint32_t type              | metadata
 *  +|--|   void *data                 | pointer to mmaped metadata in shared memory
 *  ||  |   uint32_t size              | size of metadata
 *  ||  | ... <n_metas>                | more metas follow
 *  ||  +------------------------------+
 *  |+->| struct spa_data              |
 *  |   |   uint32_t type              | memory type, either MemFd or INVALID
 *  |   |   uint32_t flags             |
 *  |   |   int fd                     | fd of shared memory block
 *  |   |   uint32_t mapoffset         | offset in shared memory of data
 *  |   |   uint32_t maxsize           | size of data block
 *  | +-|   void *data                 | pointer to mmaped data in shared memory
 *  |+|-|   struct spa_chunk *chunk    | pointer to mmaped chunk in shared memory
 *  ||| | ... <n_datas>                | more datas follow
 *  ||| +==============================+
 *  ||| | ... <n_buffers>              | more buffer/meta/data definitions follow
 *  ||| +==============================+
 *  |||
 *  |||
 *  |||  shared memory block:
 *  |||
 *  ||| +==============================+
 *  +-->| meta data memory             | metadata memory
 *   || | ... <n_metas>                |
 *   || +------------------------------+
 *   +->| struct spa_chunk             | memory for n_datas chunks
 *    | |   uint32_t offset            |
 *    | |   uint32_t size              |
 *    | |   int32_t stride             |
 *    | | ... <n_datas> chunks         |
 *    | +------------------------------+
 *    +>| data                         | memory for n_datas data
 *      | ... <n_datas> blocks         |
 *      +==============================+
 *      | ... <n_buffers>              | repeated for each buffer
 *      +==============================+
 *
 * The shared memory block should not contain any types or structure,
 * just the actual metadata contents.
 */
static int alloc_buffers(struct pw_link *this,
			 uint32_t n_buffers,
			 uint32_t n_params,
			 struct spa_pod **params,
			 uint32_t n_datas,
			 size_t *data_sizes,
			 ssize_t *data_strides,
			 struct allocation *allocation)
{
	int res;
	struct spa_buffer **buffers, *bp;
	uint32_t i;
	size_t skel_size, data_size, meta_size;
	struct spa_chunk *cdp;
	void *ddp;
	uint32_t n_metas;
	struct spa_meta *metas;
	struct pw_memblock *m;

	n_metas = data_size = meta_size = 0;

	skel_size = sizeof(struct spa_buffer);

	metas = alloca(sizeof(struct spa_meta) * n_params);

	/* collect metadata */
	for (i = 0; i < n_params; i++) {
		if (spa_pod_is_object_type (params[i], SPA_TYPE_OBJECT_ParamMeta)) {
			uint32_t type, size;

			if (spa_pod_object_parse(params[i],
				":", SPA_PARAM_META_type, "I", &type,
				":", SPA_PARAM_META_size, "i", &size, NULL) < 0)
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

	if ((res = pw_memblock_alloc(PW_MEMBLOCK_FLAG_WITH_FD |
				     PW_MEMBLOCK_FLAG_MAP_READWRITE |
				     PW_MEMBLOCK_FLAG_SEAL, n_buffers * data_size, &m)) < 0)
		return res;

	for (i = 0; i < n_buffers; i++) {
		uint32_t j;
		struct spa_buffer *b;
		void *p;

		buffers[i] = b = SPA_MEMBER(bp, skel_size * i, struct spa_buffer);

		p = SPA_MEMBER(m->ptr, data_size * i, void);

		b->id = i;
		b->n_metas = n_metas;
		b->metas = SPA_MEMBER(b, sizeof(struct spa_buffer), struct spa_meta);
		for (j = 0; j < n_metas; j++) {
			struct spa_meta *m = &b->metas[j];

			m->type = metas[j].type;
			m->size = metas[j].size;
			m->data = p;
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
				d->type = SPA_DATA_MemFd;
				d->flags = 0;
				d->fd = m->fd;
				d->mapoffset = SPA_PTRDIFF(ddp, m->ptr);
				d->maxsize = data_sizes[j];
				d->data = SPA_MEMBER(m->ptr, d->mapoffset, void);
				d->chunk->offset = 0;
				d->chunk->size = 0;
				d->chunk->stride = data_strides[j];
				ddp += data_sizes[j];
			} else {
				/* needs to be allocated by a node */
				d->type = SPA_ID_INVALID;
				d->data = NULL;
			}
		}
	}
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
		if ((res = spa_node_port_enum_params(in_port->node->node,
						     in_port->direction, in_port->port_id,
						     id, &iidx, NULL, &iparam, &ib)) < 0)
			break;

		if (res == 0) {
			if (num > 0)
				break;
			iparam = NULL;
		}

		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG) && iparam != NULL)
			spa_debug_pod(2, NULL, iparam);

		for (oidx = 0;;) {
			pw_log_debug("oparam %d", oidx);
			if (spa_node_port_enum_params(out_port->node->node, out_port->direction,
						      out_port->port_id, id, &oidx,
						      iparam, &oparam, result) <= 0) {
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
	struct spa_graph_port *p = &mix->port;
	int res = 0;

	mix->io = data;
	pw_log_debug("link %p: %s port %p %d.%d set io: %d %p %zd", this,
			pw_direction_as_string(port->direction),
			port, port->port_id, p->port_id, id, data, size);

	if (port->mix->port_set_io) {
		if ((res = spa_node_port_set_io(port->mix,
				     p->direction,
				     p->port_id,
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

	if (impl->have_io)
		return 0;

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

	impl->have_io = true;

	return 0;
}

static int do_allocation(struct pw_link *this, uint32_t in_state, uint32_t out_state)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int res;
	const struct spa_port_info *iinfo, *oinfo;
	uint32_t in_flags, out_flags;
	char *error = NULL;
	struct pw_port *input, *output;
	struct allocation allocation;
	uint32_t in_mix_state, out_mix_state;

	in_mix_state = this->rt.in_mix.state;
	out_mix_state = this->rt.out_mix.state;

	pw_log_debug("%d %d", in_mix_state, out_mix_state);

	if (in_mix_state != PW_PORT_STATE_READY && out_mix_state != PW_PORT_STATE_READY)
		return 0;

	pw_link_update_state(this, PW_LINK_STATE_ALLOCATING, NULL);

	input = this->input;
	output = this->output;

	pw_log_debug("link %p: doing alloc buffers %p %p", this, output->node, input->node);
	if ((res = spa_node_port_get_info(input->node->node,
				input->direction, input->port_id,
				&iinfo)) < 0)
		return res;
	if ((res = spa_node_port_get_info(output->node->node,
				output->direction, output->port_id,
				&oinfo)) < 0)
		return res;

	in_flags = iinfo->flags;
	out_flags = oinfo->flags;

	if (out_flags & SPA_PORT_INFO_FLAG_LIVE) {
		pw_log_debug("setting link as live");
		output->node->live = true;
		input->node->live = true;
	}

	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG)) {
		spa_debug_port_info(2, oinfo);
		spa_debug_port_info(2, iinfo);
	}
	if (output->allocation.n_buffers) {
		out_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
		in_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;

		move_allocation(&output->allocation, &allocation);

		pw_log_debug("link %p: reusing %d output buffers %p", this,
				allocation.n_buffers, allocation.buffers);
	} else if (input->allocation.n_buffers && input->mix == NULL) {
		out_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
		in_flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;

		move_allocation(&input->allocation, &allocation);

		pw_log_debug("link %p: reusing %d input buffers %p", this,
				allocation.n_buffers, allocation.buffers);
	} else {
		struct spa_pod **params, *param;
		uint8_t buffer[4096];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		uint32_t i, offset, n_params;
		uint32_t max_buffers;
		size_t minsize = 8192, stride = 0;
		size_t data_sizes[1];
		ssize_t data_strides[1];

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
		param = find_param(params, n_params, SPA_TYPE_OBJECT_ParamBuffers);
		if (param) {
			uint32_t qmax_buffers = max_buffers,
			    qminsize = minsize, qstride = stride;

			spa_pod_object_parse(param,
				":", SPA_PARAM_BUFFERS_size, "i", &qminsize,
				":", SPA_PARAM_BUFFERS_stride, "i", &qstride,
				":", SPA_PARAM_BUFFERS_buffers, "i", &qmax_buffers, NULL);

			max_buffers =
			    qmax_buffers == 0 ? max_buffers : SPA_MIN(qmax_buffers,
							      max_buffers);
			minsize = SPA_MAX(minsize, qminsize);
			stride = SPA_MAX(stride, qstride);

			pw_log_debug("%d %d %d -> %zd %zd %d", qminsize, qstride, qmax_buffers,
				     minsize, stride, max_buffers);
		} else {
			pw_log_warn("no buffers param");
			minsize = 8192;
			max_buffers = 4;
		}

		/* when one of the ports can allocate buffer memory, set the minsize to
		 * 0 to make sure we don't allocate memory in the shared memory */
		if ((in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) ||
		    (out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS)) {
			minsize = 0;
		}

		data_sizes[0] = minsize;
		data_strides[0] = stride;

		if ((res = alloc_buffers(this,
					 max_buffers,
					 n_params,
					 params,
					 1,
					 data_sizes, data_strides,
					 &allocation)) < 0) {
			asprintf(&error, "error alloc buffers: %d", res);
			goto error;
		}

		pw_log_debug("link %p: allocating %d buffers %p %zd %zd", this,
			     allocation.n_buffers, allocation.buffers, minsize, stride);

		if (out_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) {
			if ((res = pw_port_alloc_buffers(output,
							 this->rt.out_mix.port.port_id,
							 params, n_params,
							 allocation.buffers,
							 &allocation.n_buffers)) < 0) {
				asprintf(&error, "error alloc output buffers: %d", res);
				goto error;
			}
			if (SPA_RESULT_IS_ASYNC(res))
				pw_work_queue_add(impl->work, output->node, res, complete_paused,
					  &this->rt.out_mix);

			move_allocation(&allocation, &output->allocation);

			pw_log_debug("link %p: allocated %d buffers %p from output port", this,
				     allocation.n_buffers, allocation.buffers);
		} else if (in_flags & SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS) {
			if ((res = pw_port_alloc_buffers(input,
							 this->rt.in_mix.port.port_id,
							 params, n_params,
							 allocation.buffers,
							 &allocation.n_buffers)) < 0) {
				asprintf(&error, "error alloc input buffers: %d", res);
				goto error;
			}
			if (SPA_RESULT_IS_ASYNC(res))
				pw_work_queue_add(impl->work, input->node, res, complete_paused,
					  &this->rt.in_mix);

			pw_log_debug("link %p: allocated %d buffers %p from input port", this,
				     allocation.n_buffers, allocation.buffers);
		}
	}

	if (out_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) {
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
		if (SPA_RESULT_IS_ASYNC(res))
			pw_work_queue_add(impl->work, output->node, res, complete_paused,
					  &this->rt.out_mix);

		move_allocation(&allocation, &output->allocation);

	}
	if (in_flags & SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS) {
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
		if (SPA_RESULT_IS_ASYNC(res))
			pw_work_queue_add(impl->work, input->node, res, complete_paused,
					  &this->rt.in_mix);

	} else {
		asprintf(&error, "no common buffer alloc found");
		goto error;
	}

	if ((res = select_io(this)) < 0) {
		asprintf(&error, "link %p: error can set io: %s", this, spa_strerror(res));
		goto error;
	}

	return res;

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
	struct spa_graph_port *in, *out;
	struct pw_node *inode, *onode;

	pw_log_trace("link %p: activate", this);

	out = &this->rt.out_mix.port;
	in = &this->rt.in_mix.port;

	spa_graph_port_add(&this->output->rt.mix_node, out);
	spa_graph_port_add(&this->input->rt.mix_node, in);
	spa_graph_port_link(out, in);
	if (this->feedback) {
		inode = this->output->node;
		onode = this->input->node;
	}
	else {
		onode = this->output->node;
		inode = this->input->node;
	}
	if (inode != onode) {
		this->rt.link.signal_data = &inode->rt.root;
		spa_graph_link_add(&onode->rt.root,
				   inode->rt.root.state,
				   &this->rt.link);
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
	int in_mix_state, out_mix_state;

	if (this->info.state == PW_LINK_STATE_ERROR)
		return;

	input = this->input;
	output = this->output;

	if (input == NULL || output == NULL)
		return;

	if (input->node->info.state == PW_NODE_STATE_ERROR ||
	    output->node->info.state == PW_NODE_STATE_ERROR)
		return;

	in_state = input->state;
	out_state = output->state;

	pw_log_debug("link %p: input state %d, output state %d", this, in_state, out_state);

	if (in_state == PW_PORT_STATE_ERROR || out_state == PW_PORT_STATE_ERROR) {
		pw_link_update_state(this, PW_LINK_STATE_ERROR, NULL);
		return;
	}

	if (PW_PORT_IS_CONTROL(output) && PW_PORT_IS_CONTROL(input)) {
		pw_port_mix_update_state(input, &this->rt.in_mix, PW_PORT_STATE_PAUSED);
		pw_port_mix_update_state(output, &this->rt.out_mix, PW_PORT_STATE_PAUSED);
	}

	in_mix_state = this->rt.in_mix.state;
	out_mix_state = this->rt.out_mix.state;

	if (in_mix_state == PW_PORT_STATE_PAUSED && out_mix_state == PW_PORT_STATE_PAUSED) {
		pw_link_update_state(this, PW_LINK_STATE_PAUSED, NULL);
		return;
	}

	if ((res = do_negotiate(this, in_state, out_state)) != 0)
		goto exit;

	if ((res = do_allocation(this, in_state, out_state)) != 0)
		goto exit;

      exit:
	if (SPA_RESULT_IS_ERROR(res)) {
		pw_log_debug("link %p: got error result %d", this, res);
		return;
	}

	pw_work_queue_add(impl->work,
			  this, -EBUSY, (pw_work_func_t) check_states, this);
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
	pw_port_events_link_removed(this->input, this);

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
	pw_port_events_link_removed(this->output, this);

	clear_port_buffers(this, port);

	port_set_io(this, this->output, SPA_IO_Buffers, NULL, 0, mix);
	pw_port_release_mix(port, mix);
	this->output = NULL;
}

static void on_port_destroy(struct pw_link *this, struct pw_port *port)
{
	pw_link_events_port_unlinked(this, port);

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
	struct spa_graph_port *in, *out;

	in = &this->rt.in_mix.port;
	out = &this->rt.out_mix.port;

	pw_log_trace("link %p: disable %p and %p", this, in, out);

	spa_graph_port_unlink(out);
	spa_graph_port_remove(out);
	spa_graph_port_remove(in);
	if (this->input->node != this->output->node)
		spa_graph_link_remove(&this->rt.link);
	this->rt.link.signal_data = NULL;

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
	pw_port_mix_update_state(this->input, &this->rt.in_mix, PW_PORT_STATE_CONFIGURE);

	if (output_node->n_used_input_links <= output_node->idle_used_input_links &&
	    output_node->n_used_output_links <= output_node->idle_used_output_links &&
	    output_node->info.state > PW_NODE_STATE_IDLE) {
		pw_node_set_state(output_node, PW_NODE_STATE_IDLE);
		pw_log_debug("port %p: output state %d -> %d", this->output,
				this->output->state, PW_PORT_STATE_PAUSED);
	}
	pw_port_mix_update_state(this->output, &this->rt.out_mix, PW_PORT_STATE_CONFIGURE);

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

static void
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

	return;

      no_mem:
	pw_log_error("can't create link resource");
	pw_core_resource_error(client->core_resource, id, -ENOMEM, "no memory");
	return;
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

static bool pw_link_is_feedback(struct pw_node *output, struct pw_node *input)
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
			if (pw_link_is_feedback(l->input->node, input))
				return true;
		}
	}
	return false;
}

static void try_link_controls(struct impl *impl, struct pw_port *port, struct pw_port *target)
{
	struct pw_control *cin, *cout;
	int res;

	pw_log_debug("link %p: trying controls", impl);
	spa_list_for_each(cout, &port->control_list[SPA_DIRECTION_OUTPUT], port_link) {
		spa_list_for_each(cin, &target->control_list[SPA_DIRECTION_INPUT], port_link) {
			if ((res = pw_control_link(cout, cin)) < 0)
				pw_log_error("failed to link controls: %s", spa_strerror(res));
		}
	}
	spa_list_for_each(cin, &port->control_list[SPA_DIRECTION_INPUT], port_link) {
		spa_list_for_each(cout, &target->control_list[SPA_DIRECTION_OUTPUT], port_link) {
			if ((res = pw_control_link(cout, cin)) < 0)
				pw_log_error("failed to link controls: %s", spa_strerror(res));
		}
	}
}

static void try_unlink_controls(struct impl *impl, struct pw_port *port, struct pw_port *target)
{
	struct pw_control *cin, *cout;
	int res;

	pw_log_debug("link %p: unlinking controls", impl);
	spa_list_for_each(cout, &port->control_list[SPA_DIRECTION_OUTPUT], port_link) {
		spa_list_for_each(cin, &target->control_list[SPA_DIRECTION_INPUT], port_link) {
			if ((res = pw_control_unlink(cout, cin)) < 0)
				pw_log_error("failed to unlink controls: %s", spa_strerror(res));
		}
	}
	spa_list_for_each(cin, &port->control_list[SPA_DIRECTION_INPUT], port_link) {
		spa_list_for_each(cout, &target->control_list[SPA_DIRECTION_OUTPUT], port_link) {
			if ((res = pw_control_unlink(cout, cin)) < 0)
				pw_log_error("failed to unlink controls: %s", spa_strerror(res));
		}
	}
}

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
	this->feedback = pw_link_is_feedback(input_node, output_node);
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

	this->rt.link.signal = spa_graph_link_signal_node;

	pw_log_debug("link %p: constructed %p:%d.%d -> %p:%d.%d", impl,
		     output_node, output->port_id, this->rt.out_mix.port.port_id,
		     input_node, input->port_id, this->rt.in_mix.port.port_id);

	find_driver(this);

	spa_hook_list_call(&output->listener_list, struct pw_port_events, link_added, 0, this);
	spa_hook_list_call(&input->listener_list, struct pw_port_events, link_added, 0, this);

	try_link_controls(impl, output, input);

	return this;

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
	.bind = global_bind,
};

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

void pw_link_destroy(struct pw_link *link)
{
	struct impl *impl = SPA_CONTAINER_OF(link, struct impl, this);

	pw_log_debug("link %p: destroy", impl);
	pw_link_events_destroy(link);

	pw_link_deactivate(link);

	if (link->registered)
		spa_list_remove(&link->link);

	try_unlink_controls(impl, link->input, link->output);

	input_remove(link, link->input);
	output_remove(link, link->output);

	if (link->global) {
		spa_hook_remove(&link->global_listener);
		pw_global_destroy(link->global);
	}

	pw_log_debug("link %p: free", impl);
	pw_link_events_free(link);

	pw_work_queue_destroy(impl->work);

	if (link->properties)
		pw_properties_free(link->properties);

	free(link->info.format);
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
