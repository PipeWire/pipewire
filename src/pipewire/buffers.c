/* PipeWire
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

#include <spa/node/utils.h>
#include <spa/pod/parser.h>
#include <spa/param/param.h>
#include <spa/buffer/alloc.h>

#include <spa/debug/node.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>

#include "pipewire/keys.h"
#include "pipewire/private.h"

#include "buffers.h"

#define NAME "buffers"

#define MAX_ALIGN	32
#define MAX_BUFFERS	64

struct port {
	struct spa_node *node;
	enum spa_direction direction;
	uint32_t port_id;
};

/* Allocate an array of buffers that can be shared */
static int alloc_buffers(struct pw_mempool *pool,
			 uint32_t n_buffers,
			 uint32_t n_params,
			 struct spa_pod **params,
			 uint32_t n_datas,
			 uint32_t *data_sizes,
			 int32_t *data_strides,
			 uint32_t *data_aligns,
			 uint32_t flags,
			 struct pw_buffers *allocation)
{
	struct spa_buffer **buffers;
	void *skel, *data;
	uint32_t i;
	uint32_t n_metas;
	struct spa_meta *metas;
	struct spa_data *datas;
	struct pw_memblock *m;
	struct spa_buffer_alloc_info info = { 0, };

	if (!SPA_FLAG_IS_SET(flags, PW_BUFFERS_FLAG_SHARED))
		SPA_FLAG_SET(info.flags, SPA_BUFFER_ALLOC_FLAG_INLINE_ALL);

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

			pw_log_debug(NAME" %p: enable meta %d %d", allocation, type, size);

			metas[n_metas].type = type;
			metas[n_metas].size = size;
			n_metas++;
		}
	}

	for (i = 0; i < n_datas; i++) {
		struct spa_data *d = &datas[i];

		spa_zero(*d);
		if (data_sizes[i] > 0) {
			d->type = SPA_DATA_MemPtr;
			d->maxsize = data_sizes[i];
			SPA_FLAG_SET(d->flags, SPA_DATA_FLAG_READWRITE);
		} else {
			d->type = SPA_ID_INVALID;
			d->maxsize = 0;
		}
		if (SPA_FLAG_IS_SET(flags, PW_BUFFERS_FLAG_DYNAMIC))
			SPA_FLAG_SET(d->flags, SPA_DATA_FLAG_DYNAMIC);
	}

        spa_buffer_alloc_fill_info(&info, n_metas, metas, n_datas, datas, data_aligns);

	buffers = calloc(1, info.max_align + n_buffers * (sizeof(struct spa_buffer *) + info.skel_size));
	if (buffers == NULL)
		return -errno;

	skel = SPA_MEMBER(buffers, n_buffers * sizeof(struct spa_buffer *), void);
	skel = SPA_PTR_ALIGN(skel, info.max_align, void);

	if (SPA_FLAG_IS_SET(flags, PW_BUFFERS_FLAG_SHARED)) {
		/* pointer to buffer structures */
		m = pw_mempool_alloc(pool,
				PW_MEMBLOCK_FLAG_READWRITE |
				PW_MEMBLOCK_FLAG_SEAL |
				PW_MEMBLOCK_FLAG_MAP,
				SPA_DATA_MemFd,
				n_buffers * info.mem_size);
		if (m == NULL)
			return -errno;

		data = m->map->ptr;
	} else {
		m = NULL;
		data = NULL;
	}

	pw_log_debug(NAME" %p: layout buffers skel:%p data:%p", allocation, skel, data);
	spa_buffer_alloc_layout_array(&info, n_buffers, buffers, skel, data);

	allocation->mem = m;
	allocation->n_buffers = n_buffers;
	allocation->buffers = buffers;
	allocation->flags = flags;

	return 0;
}

static int
param_filter(struct pw_buffers *this,
	     struct port *in_port,
	     struct port *out_port,
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
		pw_log_debug(NAME" %p: input param %d id:%d", this, iidx, id);
		if ((res = spa_node_port_enum_params_sync(in_port->node,
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
			pw_log_debug(NAME" %p: output param %d id:%d", this, oidx, id);
			if (spa_node_port_enum_params_sync(out_port->node,
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

static struct spa_pod *find_param(struct spa_pod **params, uint32_t n_params, uint32_t type)
{
	uint32_t i;

	for (i = 0; i < n_params; i++) {
		if (spa_pod_is_object_type(params[i], type))
			return params[i];
	}
	return NULL;
}

SPA_EXPORT
int pw_buffers_negotiate(struct pw_core *core, uint32_t flags,
		struct spa_node *outnode, uint32_t out_port_id,
		struct spa_node *innode, uint32_t in_port_id,
		struct pw_buffers *result)
{
	struct spa_pod **params, *param;
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t i, offset, n_params;
	uint32_t max_buffers;
	size_t minsize, stride, align;
	uint32_t data_sizes[1];
	int32_t data_strides[1];
	uint32_t data_aligns[1];
	struct port output = { outnode, SPA_DIRECTION_OUTPUT, out_port_id };
	struct port input = { innode, SPA_DIRECTION_INPUT, in_port_id };
	const char *str;
	int res;

	n_params = param_filter(result, &input, &output, SPA_PARAM_Buffers, &b);
	n_params += param_filter(result, &input, &output, SPA_PARAM_Meta, &b);

	params = alloca(n_params * sizeof(struct spa_pod *));
	for (i = 0, offset = 0; i < n_params; i++) {
		params[i] = SPA_MEMBER(buffer, offset, struct spa_pod);
		spa_pod_fixate(params[i]);
		pw_log_debug(NAME" %p: fixated param %d:", result, i);
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_pod(2, NULL, params[i]);
		offset += SPA_ROUND_UP_N(SPA_POD_SIZE(params[i]), 8);
	}

	if ((str = pw_properties_get(core->properties, "link.max-buffers")) != NULL)
		max_buffers = pw_properties_parse_int(str);
	else
		max_buffers = MAX_BUFFERS;

	if ((str = pw_properties_get(core->properties, PW_KEY_CPU_MAX_ALIGN)) != NULL)
		align = pw_properties_parse_int(str);
	else
		align = MAX_ALIGN;

	minsize = stride = 0;
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

		pw_log_debug(NAME" %p: %d %d %d %d -> %zd %zd %d %zd", result,
				qminsize, qstride, qmax_buffers, qalign,
				minsize, stride, max_buffers, align);
	} else {
		pw_log_warn(NAME" %p: no buffers param", result);
		minsize = 8192;
		max_buffers = 4;
	}

	if (SPA_FLAG_IS_SET(flags, PW_BUFFERS_FLAG_NO_MEM))
		minsize = 0;

	data_sizes[0] = minsize;
	data_strides[0] = stride;
	data_aligns[0] = align;

	if ((res = alloc_buffers(core->pool,
				 max_buffers,
				 n_params,
				 params,
				 1,
				 data_sizes, data_strides,
				 data_aligns,
				 flags,
				 result)) < 0) {
		pw_log_error(NAME" %p: can't alloc buffers: %s", result, spa_strerror(res));
	}

	return res;
}

SPA_EXPORT
void pw_buffers_clear(struct pw_buffers *buffers)
{
	if (buffers->mem)
		pw_memblock_unref(buffers->mem);
	free(buffers->buffers);
	spa_zero(*buffers);
}
