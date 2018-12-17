/* Spa
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
#include <math.h>

#include <spa/support/log.h>
#include <spa/utils/list.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/pod/filter.h>
#include <spa/control/control.h>
#include <spa/debug/types.h>

#define NAME "channelmix"

#define DEFAULT_RATE		44100
#define DEFAULT_CHANNELS	2

#define MAX_BUFFERS     32

struct impl;

#define DEFAULT_MUTE	false
#define DEFAULT_VOLUME	1.0f

struct props {
	float volume;
	bool mute;
};

static void props_reset(struct props *props)
{
	props->mute = DEFAULT_MUTE;
	props->volume = DEFAULT_VOLUME;
}

struct buffer {
	struct spa_list link;
#define BUFFER_FLAG_OUT		(1 << 0)
	uint32_t flags;
	struct spa_buffer *outbuf;
	struct spa_meta_header *h;
};

struct port {
	uint32_t id;

	struct spa_io_buffers *io;
	struct spa_io_sequence *control;
	struct spa_port_info info;

	bool have_format;
	struct spa_audio_info format;
	uint32_t stride;
	uint32_t blocks;
	uint32_t size;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;

	struct spa_list queue;
};

#include "channelmix-ops.c"

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_cpu *cpu;

	struct props props;

	const struct spa_node_callbacks *callbacks;
	void *user_data;

	struct port in_port;
	struct port out_port;

	bool started;

	uint32_t cpu_flags;
	channelmix_func_t convert;
	uint32_t n_matrix;
	float matrix[4096];
};

#define CHECK_PORT(this,d,id)		(id == 0)
#define GET_IN_PORT(this,id)		(&this->in_port)
#define GET_OUT_PORT(this,id)		(&this->out_port)
#define GET_PORT(this,d,id)		(d == SPA_DIRECTION_INPUT ? GET_IN_PORT(this,id) : GET_OUT_PORT(this,id))

#define _MASK(ch)	(1ULL << SPA_AUDIO_CHANNEL_ ## ch)
#define STEREO	(_MASK(FL)|_MASK(FR))

#define M		0
#define FL		1
#define FR		2
#define FC		3
#define LFE		4
#define SL		5
#define SR		6
#define FLC		7
#define FRC		8
#define RC		9
#define RL		10
#define RR		11
#define TC		12
#define TFL		13
#define TFC		14
#define TFR		15
#define TRL		16
#define TRC		17
#define TRR		18
#define NUM_CHAN	19

#define SQRT3_2      1.224744871f  /* sqrt(3/2) */
#define SQRT1_2      0.707106781f
#define SQRT2	     1.414213562f

#define MATRIX_NORMAL	0
#define MATRIX_DOLBY	1
#define MATRIX_DPLII	2

static uint64_t default_mask(uint32_t channels)
{
	uint64_t mask = 0;
	switch (channels) {
	case 8:
		mask |= _MASK(RL);
		mask |= _MASK(RR);
		/* fallthrough */
	case 6:
		mask |= _MASK(SL);
		mask |= _MASK(SR);
		mask |= _MASK(LFE);
		/* fallthrough */
	case 3:
		mask |= _MASK(FC);
		/* fallthrough */
	case 2:
		mask |= _MASK(FL);
		mask |= _MASK(FR);
		break;
	case 1:
		mask |= _MASK(MONO);
		break;
	case 4:
		mask |= _MASK(FL);
		mask |= _MASK(FR);
		mask |= _MASK(RL);
		mask |= _MASK(RR);
		break;
	}
	return mask;
}

static int make_matrix(struct impl *this,
		uint32_t src_chan, uint64_t src_mask,
		uint32_t dst_chan, uint64_t dst_mask)
{
	float matrix[NUM_CHAN][NUM_CHAN] = {{ 0.0f }};
	uint64_t unassigned;
	int i, j, matrix_encoding = MATRIX_NORMAL, c;
	float clev = SQRT1_2;
	float slev = SQRT1_2;
	float llev = 0.5f;
	float max = 0.0f;

	for (i = 0; i < NUM_CHAN; i++) {
		if (src_mask & dst_mask & (1ULL << (i + 2)))
			matrix[i][i]= 1.0f;
	}

	unassigned = src_mask & ~dst_mask;

	spa_log_debug(this->log, "unassigned %08lx", unassigned);

	if (unassigned & _MASK(FC)){
		if (dst_mask & _MASK(MONO)) {
			matrix[M][FC] += clev;
		}
		else if ((dst_mask & STEREO) == STEREO){
			if(src_mask & STEREO) {
				matrix[FL][FC] += clev;
				matrix[FR][FC] += clev;
			} else {
				matrix[FL][FC] += SQRT1_2;
				matrix[FR][FC] += SQRT1_2;
			}
		} else {
			spa_log_warn(this->log, "can't assign FC");
		}
	}

	if (unassigned & STEREO){
		if (dst_mask & _MASK(MONO)) {
			matrix[M][FL] += 0.5f;
			matrix[M][FR] += 0.5f;
		}
		else if (dst_mask & _MASK(FC)) {
			matrix[FC][FL] += SQRT1_2;
			matrix[FC][FR] += SQRT1_2;
			if (src_mask & _MASK(FC))
				matrix[FC][FC] = clev * SQRT2;
		} else {
			spa_log_warn(this->log, "can't assign STEREO");
		}
	}

	if (unassigned & _MASK(RC)) {
		if (dst_mask & _MASK(RL)){
			matrix[RL][RC] += SQRT1_2;
			matrix[RR][RC] += SQRT1_2;
		} else if (dst_mask & _MASK(SL)) {
			matrix[SL][RC] += SQRT1_2;
			matrix[SR][RC] += SQRT1_2;
		} else if(dst_mask & _MASK(FL)) {
			if (matrix_encoding == MATRIX_DOLBY ||
			    matrix_encoding == MATRIX_DPLII) {
				if (unassigned & (_MASK(RL)|_MASK(RR))) {
					matrix[FL][RC] -= slev * SQRT1_2;
					matrix[FR][RC] += slev * SQRT1_2;
		                } else {
					matrix[FL][RC] -= slev;
					matrix[FR][RC] += slev;
				}
			} else {
				matrix[FL][RC] += slev * SQRT1_2;
				matrix[FR][RC] += slev * SQRT1_2;
			}
		} else if (dst_mask & _MASK(FC)) {
			matrix[FC][RC] += slev * SQRT1_2;
		} else if (dst_mask & _MASK(MONO)) {
			matrix[M][RC] += slev * SQRT1_2;
		} else {
			spa_log_warn(this->log, "can't assign RC");
		}
	}

	if (unassigned & _MASK(RL)) {
		if (dst_mask & _MASK(RC)) {
			matrix[RC][RL] += SQRT1_2;
			matrix[RC][RR] += SQRT1_2;
		} else if (dst_mask & _MASK(SL)) {
			if (src_mask & _MASK(SL)) {
				matrix[SL][RL] += SQRT1_2;
				matrix[SR][RR] += SQRT1_2;
			} else {
				matrix[SL][RL] += 1.0f;
				matrix[SR][RR] += 1.0f;
			}
		} else if (dst_mask & _MASK(FL)) {
			if (matrix_encoding == MATRIX_DOLBY) {
				matrix[FL][RL] -= slev * SQRT1_2;
				matrix[FL][RR] -= slev * SQRT1_2;
				matrix[FR][RL] += slev * SQRT1_2;
				matrix[FR][RR] += slev * SQRT1_2;
			} else if (matrix_encoding == MATRIX_DPLII) {
				matrix[FL][RL] -= slev * SQRT3_2;
				matrix[FL][RR] -= slev * SQRT1_2;
				matrix[FR][RL] += slev * SQRT1_2;
				matrix[FR][RR] += slev * SQRT3_2;
			} else {
				matrix[FL][RL] += slev;
				matrix[FR][RR] += slev;
			}
		} else if (dst_mask & _MASK(FC)) {
			matrix[FC][RL]+= slev * SQRT1_2;
			matrix[FC][RR]+= slev * SQRT1_2;
		} else if (dst_mask & _MASK(MONO)) {
			matrix[M][RL]+= slev * SQRT1_2;
			matrix[M][RR]+= slev * SQRT1_2;
		} else {
			spa_log_warn(this->log, "can't assign RL");
		}
	}

	if (unassigned & _MASK(SL)) {
		if (dst_mask & _MASK(RL)) {
			if (src_mask & _MASK(RL)) {
				matrix[RL][SL] += SQRT1_2;
				matrix[RR][SR] += SQRT1_2;
			} else {
				matrix[RL][SL] += 1.0f;
				matrix[RR][SR] += 1.0f;
			}
		} else if (dst_mask & _MASK(RC)) {
			matrix[RC][SL]+= SQRT1_2;
			matrix[RC][SR]+= SQRT1_2;
		} else if (dst_mask & _MASK(FL)) {
			if (matrix_encoding == MATRIX_DOLBY) {
				matrix[FL][SL] -= slev * SQRT1_2;
				matrix[FL][SR] -= slev * SQRT1_2;
				matrix[FR][SL] += slev * SQRT1_2;
				matrix[FR][SR] += slev * SQRT1_2;
			} else if (matrix_encoding == MATRIX_DPLII) {
				matrix[FL][SL] -= slev * SQRT3_2;
				matrix[FL][SR] -= slev * SQRT1_2;
				matrix[FR][SL] += slev * SQRT1_2;
				matrix[FR][SR] += slev * SQRT3_2;
			} else {
				matrix[FL][SL] += slev;
				matrix[FR][SR] += slev;
			}
		} else if (dst_mask & _MASK(FC)) {
			matrix[FC][SL] += slev * SQRT1_2;
			matrix[FC][SR] += slev * SQRT1_2;
		} else if (dst_mask & _MASK(MONO)) {
			matrix[M][SL] += slev * SQRT1_2;
			matrix[M][SR] += slev * SQRT1_2;
		} else {
			spa_log_warn(this->log, "can't assign SL");
		}
	}

	if (unassigned & _MASK(FLC)) {
		if (dst_mask & _MASK(FL)) {
			matrix[FC][FLC]+= 1.0f;
			matrix[FC][FRC]+= 1.0f;
		} else if(dst_mask & _MASK(FC)) {
			matrix[FC][FLC]+= SQRT1_2;
			matrix[FC][FRC]+= SQRT1_2;
		} else {
			spa_log_warn(this->log, "can't assign FLC");
		}
	}
	if (unassigned & _MASK(LFE)) {
		if (dst_mask & _MASK(MONO)) {
			matrix[M][LFE] += llev;
		} else if (dst_mask & _MASK(FC)) {
			matrix[FC][LFE] += llev;
		} else if (dst_mask & _MASK(FL)) {
			matrix[FL][LFE] += llev * SQRT1_2;
			matrix[FR][LFE] += llev * SQRT1_2;
		} else {
			spa_log_warn(this->log, "can't assign LFE");
		}
	}

	c = 0;
	for (i = 0; i < NUM_CHAN; i++) {
		float sum = 0.0f;
		if ((dst_mask & (1UL << (i + 2))) == 0)
			continue;
		for (j = 0; j < NUM_CHAN; j++) {
			if ((src_mask & (1UL << (j + 2))) == 0)
				continue;
			this->matrix[c++] = matrix[i][j];
			sum += fabs(matrix[i][j]);
		}
		max = SPA_MAX(max, sum);
	}
	this->n_matrix = c;
	for (i = 0; i < dst_chan; i++) {
		for (j = 0; j < src_chan; j++) {
			spa_log_debug(this->log, "%d %d: %f", i, j, this->matrix[i * src_chan + j]);
		}
	}

	return 0;
}

static int setup_convert(struct impl *this,
		enum spa_direction direction,
		const struct spa_audio_info *info)
{
	const struct spa_audio_info *src_info, *dst_info;
	uint32_t src_chan, dst_chan;
	const struct channelmix_info *chanmix_info;
	uint64_t src_mask, dst_mask;
	int i;

	if (direction == SPA_DIRECTION_INPUT) {
		src_info = info;
		dst_info = &GET_OUT_PORT(this, 0)->format;
	} else {
		src_info = &GET_IN_PORT(this, 0)->format;
		dst_info = info;
	}

	src_chan = src_info->info.raw.channels;
	dst_chan = dst_info->info.raw.channels;

	for (i = 0, src_mask = 0; i < src_chan; i++)
		src_mask |= 1UL << src_info->info.raw.position[i];
	for (i = 0, dst_mask = 0; i < dst_chan; i++)
		dst_mask |= 1UL << dst_info->info.raw.position[i];

	if (src_mask & 1)
		src_mask = default_mask(src_chan);
	if (dst_mask & 1)
		dst_mask = default_mask(dst_chan);

	spa_log_info(this->log, NAME " %p: %s/%d@%d->%s/%d@%d %08lx:%08lx", this,
			spa_debug_type_find_name(spa_type_audio_format, src_info->info.raw.format),
			src_chan,
			src_info->info.raw.rate,
			spa_debug_type_find_name(spa_type_audio_format, dst_info->info.raw.format),
			dst_chan,
			dst_info->info.raw.rate,
			src_mask, dst_mask);

	if (src_info->info.raw.rate != dst_info->info.raw.rate)
		return -EINVAL;

	/* find convert function */
	if ((chanmix_info = find_channelmix_info(src_chan, src_mask,
					dst_chan, dst_mask, this->cpu_flags)) == NULL)
		return -ENOTSUP;

	spa_log_info(this->log, NAME " %p: got channelmix features %08x:%08x",
			this, this->cpu_flags, chanmix_info->features);

	this->convert = chanmix_info->func;

	return make_matrix(this, src_chan, src_mask, dst_chan, dst_mask);
}

static int impl_node_enum_params(struct spa_node *node,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod *filter,
				 struct spa_pod **result,
				 struct spa_pod_builder *builder)
{
	struct impl *this;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_PropInfo,
				    SPA_PARAM_Props };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_ParamList, id,
				SPA_PARAM_LIST_id, &SPA_POD_Id(list[*index]),
				0);
		else
			return 0;
		break;
	}
	case SPA_PARAM_PropInfo:
	{
		struct props *p = &this->props;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   &SPA_POD_Id(SPA_PROP_volume),
				SPA_PROP_INFO_name, &SPA_POD_Stringc("Volume"),
				SPA_PROP_INFO_type, &SPA_POD_CHOICE_RANGE_Float(p->volume, 0.0, 10.0),
				0);
			break;
		case 1:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_PropInfo, id,
				SPA_PROP_INFO_id,   &SPA_POD_Id(SPA_PROP_mute),
				SPA_PROP_INFO_name, &SPA_POD_Stringc("Mute"),
				SPA_PROP_INFO_type, &SPA_POD_Bool(p->mute),
				0);
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_Props:
	{
		struct props *p = &this->props;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_Props, id,
				SPA_PROP_volume,	&SPA_POD_Float(p->volume),
				SPA_PROP_mute,		&SPA_POD_Bool(p->mute),
				0);
			break;
		default:
			return 0;
		}
		break;
	}
	default:
		return -ENOENT;
	}

	(*index)++;

	if (spa_pod_filter(builder, result, param, filter) < 0)
		goto next;

	return 1;
}

static int apply_props(struct impl *this, const struct spa_pod *param)
{
	struct spa_pod_prop *prop;
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	struct props *p = &this->props;

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_volume:
			spa_pod_get_float(&prop->value, &p->volume);
			break;
		case SPA_PROP_mute:
			spa_pod_get_bool(&prop->value, &p->mute);
			break;
		default:
			break;
		}
	}
	return 0;
}

static int impl_node_set_io(struct spa_node *node, uint32_t id, void *data, size_t size)
{
	return -ENOTSUP;
}

static int impl_node_set_param(struct spa_node *node, uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	switch (id) {
	case SPA_PARAM_Props:
		return apply_props(this, param);
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_node_send_command(struct spa_node *node, const struct spa_command *command)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		this->started = true;
		break;
	case SPA_NODE_COMMAND_Pause:
		this->started = false;
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static int
impl_node_set_callbacks(struct spa_node *node,
			const struct spa_node_callbacks *callbacks,
			void *user_data)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	this->callbacks = callbacks;
	this->user_data = user_data;

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
		*n_input_ports = 1;
	if (max_input_ports)
		*max_input_ports = 1;
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

	if (n_input_ids && input_ids)
		input_ids[0] = 0;
	if (n_output_ids > 0 && output_ids)
		output_ids[0] = 0;

	return 0;
}

static int impl_node_add_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_remove_port(struct spa_node *node, enum spa_direction direction, uint32_t port_id)
{
	return -ENOTSUP;
}

static int
impl_node_port_get_info(struct spa_node *node,
			enum spa_direction direction,
			uint32_t port_id,
			const struct spa_port_info **info)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);
	*info = &port->info;

	return 0;
}

static int port_enum_formats(struct spa_node *node,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t *index,
			     struct spa_pod **param,
			     struct spa_pod_builder *builder)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *other;

	other = GET_PORT(this, SPA_DIRECTION_REVERSE(direction), 0);

	switch (*index) {
	case 0:
		if (other->have_format) {
			*param = spa_pod_builder_object(builder,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
				SPA_FORMAT_mediaType,      &SPA_POD_Id(SPA_MEDIA_TYPE_audio),
				SPA_FORMAT_mediaSubtype,   &SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_AUDIO_format,   &SPA_POD_Id(SPA_AUDIO_FORMAT_F32P),
				SPA_FORMAT_AUDIO_rate,     &SPA_POD_Int(other->format.info.raw.rate),
				SPA_FORMAT_AUDIO_channels, &SPA_POD_CHOICE_RANGE_Int(DEFAULT_CHANNELS, 1, INT32_MAX),
				0);
		} else {
			*param = spa_pod_builder_object(builder,
				SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
				SPA_FORMAT_mediaType,      &SPA_POD_Id(SPA_MEDIA_TYPE_audio),
				SPA_FORMAT_mediaSubtype,   &SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_AUDIO_format,   &SPA_POD_Id(SPA_AUDIO_FORMAT_F32P),
				SPA_FORMAT_AUDIO_rate,     &SPA_POD_CHOICE_RANGE_Int(DEFAULT_RATE, 1, INT32_MAX),
				SPA_FORMAT_AUDIO_channels, &SPA_POD_CHOICE_RANGE_Int(DEFAULT_CHANNELS, 1, INT32_MAX),
				0);
		}
		break;
	default:
		return 0;
	}
	return 1;
}

static int
impl_node_port_enum_params(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t id, uint32_t *index,
			   const struct spa_pod *filter,
			   struct spa_pod **result,
			   struct spa_pod_builder *builder)
{
	struct impl *this;
	struct port *port, *other;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	int res;

	spa_return_val_if_fail(node != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	spa_return_val_if_fail(builder != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);
	other = GET_PORT(this, SPA_DIRECTION_REVERSE(direction), port_id);

      next:
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_EnumFormat,
				    SPA_PARAM_Format,
				    SPA_PARAM_Buffers,
				    SPA_PARAM_Meta,
				    SPA_PARAM_IO };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(&b,
					SPA_TYPE_OBJECT_ParamList, id,
					SPA_PARAM_LIST_id, &SPA_POD_Id(list[*index]),
					0);
		else
			return 0;
		break;
	}
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(node, direction, port_id, index, &param, &b)) <= 0)
			return res;
		break;

	case SPA_PARAM_Format:
		if (!port->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		param = spa_format_audio_raw_build(&b, id, &port->format.info.raw);
		break;

	case SPA_PARAM_Buffers:
	{
		uint32_t buffers, size;

		if (!port->have_format)
			return -EIO;
		if (*index > 0)
			return 0;

		if (other->n_buffers > 0) {
			buffers = other->n_buffers;
			size = other->size / other->stride;
		} else {
			buffers = 1;
			size = port->format.info.raw.rate * 1024 / DEFAULT_RATE;
		}

		param = spa_pod_builder_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, &SPA_POD_CHOICE_RANGE_Int(buffers, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  &SPA_POD_Int(port->blocks),
			SPA_PARAM_BUFFERS_size,    &SPA_POD_CHOICE_RANGE_Int(
							size * port->stride,
							16 * port->stride,
							INT32_MAX / port->stride),
			SPA_PARAM_BUFFERS_stride,  &SPA_POD_Int(port->stride),
			SPA_PARAM_BUFFERS_align,   &SPA_POD_Int(16),
			0);
		break;
	}
	case SPA_PARAM_Meta:
		if (!port->have_format)
			return -EIO;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, &SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, &SPA_POD_Int(sizeof(struct spa_meta_header)),
				0);
			break;
		default:
			return 0;
		}
		break;

	case SPA_PARAM_IO:
		switch (*index) {
		case 0:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   &SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, &SPA_POD_Int(sizeof(struct spa_io_buffers)),
				0);
			break;
		case 1:
			param = spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id,   &SPA_POD_Id(SPA_IO_Control),
				SPA_PARAM_IO_size, &SPA_POD_Int(sizeof(struct spa_io_sequence)),
				0);
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

static int clear_buffers(struct impl *this, struct port *port)
{
	if (port->n_buffers > 0) {
		spa_log_debug(this->log, NAME " %p: clear buffers %p", this, port);
		port->n_buffers = 0;
		spa_list_init(&port->queue);
	}
	return 0;
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction,
			   uint32_t port_id,
			   uint32_t flags,
			   const struct spa_pod *format)
{
	struct impl *this = SPA_CONTAINER_OF(node, struct impl, node);
	struct port *port, *other;
	int res = 0;

	port = GET_PORT(this, direction, port_id);
	other = GET_PORT(this, SPA_DIRECTION_REVERSE(direction), port_id);

	if (format == NULL) {
		if (port->have_format) {
			port->have_format = false;
			clear_buffers(this, port);
		}
		this->convert = NULL;
	} else {
		struct spa_audio_info info = { 0 };

		if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
			return res;

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			return -EINVAL;

		if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
			return -EINVAL;

		if (info.info.raw.format != SPA_AUDIO_FORMAT_F32P)
			return -EINVAL;

		port->stride = sizeof(float);
		port->blocks = info.info.raw.channels;

		if (other->have_format) {
			if ((res = setup_convert(this, direction, &info)) < 0)
				return res;
		}
		port->format = info;
		port->have_format = true;

		spa_log_debug(this->log, NAME " %p: set format on port %d %d", this, port_id, res);
	}
	return res;
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
			   uint32_t port_id,
			   struct spa_buffer **buffers,
			   uint32_t n_buffers)
{
	struct impl *this;
	struct port *port;
	uint32_t i, size = SPA_ID_INVALID;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	spa_return_val_if_fail(port->have_format, -EIO);

	spa_log_debug(this->log, NAME " %p: use buffers %d on port %d", this, n_buffers, port_id);

	clear_buffers(this, port);

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d = buffers[i]->datas;

		b = &port->buffers[i];
		b->flags = 0;
		b->outbuf = buffers[i];
		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		if (size == SPA_ID_INVALID)
			size = d[0].maxsize;
		else
			if (size != d[0].maxsize)
				return -EINVAL;

		if (!((d[0].type == SPA_DATA_MemPtr ||
		       d[0].type == SPA_DATA_MemFd ||
		       d[0].type == SPA_DATA_DmaBuf) && d[0].data != NULL)) {
			spa_log_error(this->log, NAME " %p: invalid memory on buffer %p", this,
				      buffers[i]);
			return -EINVAL;
		}
		if (direction == SPA_DIRECTION_OUTPUT)
			spa_list_append(&port->queue, &b->link);
		else
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
	}
	port->n_buffers = n_buffers;
	port->size = size;

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
	return -ENOTSUP;
}

static int
impl_node_port_set_io(struct spa_node *node,
		      enum spa_direction direction, uint32_t port_id,
		      uint32_t id, void *data, size_t size)
{
	struct impl *this;
	struct port *port;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, direction, port_id), -EINVAL);

	port = GET_PORT(this, direction, port_id);

	switch (id) {
	case SPA_IO_Buffers:
		port->io = data;
		break;
	case SPA_IO_Control:
		port->control = data;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static void recycle_buffer(struct impl *this, uint32_t id)
{
	struct port *port = GET_OUT_PORT(this, 0);
	struct buffer *b = &port->buffers[id];

	if (SPA_FLAG_CHECK(b->flags, BUFFER_FLAG_OUT)) {
		spa_list_append(&port->queue, &b->link);
		SPA_FLAG_UNSET(b->flags, BUFFER_FLAG_OUT);
		spa_log_trace(this->log, NAME " %p: recycle buffer %d", this, id);
	}
}

static struct buffer *dequeue_buffer(struct impl *this, struct port *port)
{
	struct buffer *b;

	if (spa_list_is_empty(&port->queue))
		return NULL;

	b = spa_list_first(&port->queue, struct buffer, link);
	spa_list_remove(&b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);

	return b;
}


static int impl_node_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct impl *this;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	spa_return_val_if_fail(CHECK_PORT(this, SPA_DIRECTION_OUTPUT, port_id), -EINVAL);

	recycle_buffer(this, buffer_id);

	return 0;
}

static int
impl_node_port_send_command(struct spa_node *node,
			    enum spa_direction direction,
			    uint32_t port_id,
			    const struct spa_command *command)
{
	return -ENOTSUP;
}

static int process_control(struct impl *this, struct port *port, struct spa_pod_sequence *sequence)
{
	struct spa_pod_control *c;

	SPA_POD_SEQUENCE_FOREACH(sequence, c) {
		switch (c->type) {
		case SPA_CONTROL_Properties:
			apply_props(this, (const struct spa_pod *) &c->value);
			break;
		default:
			break;
                }
	}
	return 0;
}

static int impl_node_process(struct spa_node *node)
{
	struct impl *this;
	struct port *outport, *inport;
	struct spa_io_buffers *outio, *inio;
	struct buffer *sbuf, *dbuf;

	spa_return_val_if_fail(node != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(node, struct impl, node);

	outport = GET_OUT_PORT(this, 0);
	inport = GET_IN_PORT(this, 0);

	outio = outport->io;
	inio = inport->io;

	spa_return_val_if_fail(outio != NULL, -EIO);
	spa_return_val_if_fail(inio != NULL, -EIO);

	spa_log_trace(this->log, NAME " %p: status %d %d", this, inio->status, outio->status);

	if (outport->control)
		process_control(this, outport, &outport->control->sequence);

	if (outio->status == SPA_STATUS_HAVE_BUFFER)
		goto done;

	if (inio->status != SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_NEED_BUFFER;

	/* recycle */
	if (outio->buffer_id < outport->n_buffers) {
		recycle_buffer(this, outio->buffer_id);
		outio->buffer_id = SPA_ID_INVALID;
	}

	if (inio->buffer_id >= inport->n_buffers)
		return inio->status = -EINVAL;

	if ((dbuf = dequeue_buffer(this, outport)) == NULL)
		return outio->status = -EPIPE;

	sbuf = &inport->buffers[inio->buffer_id];

	{
		int i, n_bytes;
		struct spa_buffer *sb = sbuf->outbuf, *db = dbuf->outbuf;
		uint32_t n_src_datas = sb->n_datas;
		uint32_t n_dst_datas = db->n_datas;
		const void *src_datas[n_src_datas];
		void *dst_datas[n_dst_datas];

		n_bytes = sb->datas[0].chunk->size;

		for (i = 0; i < n_src_datas; i++)
			src_datas[i] = sb->datas[i].data;
		for (i = 0; i < n_dst_datas; i++) {
			dst_datas[i] = db->datas[i].data;
			db->datas[i].chunk->size =
				(n_bytes / inport->stride) * outport->stride;
		}

		this->convert(this, n_dst_datas, dst_datas,
				    n_src_datas, src_datas,
				    this->matrix, this->props.mute ? 0.0f : this->props.volume,
				    n_bytes);
	}

	outio->status = SPA_STATUS_HAVE_BUFFER;
	outio->buffer_id = dbuf->outbuf->id;

	inio->status = SPA_STATUS_NEED_BUFFER;

      done:
	return SPA_STATUS_HAVE_BUFFER | SPA_STATUS_NEED_BUFFER;
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
	struct port *port;
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

	if (this->cpu)
		this->cpu_flags = spa_cpu_get_flags(this->cpu);

	this->node = impl_node;

	port = GET_OUT_PORT(this, 0);
	port->id = 0;
	port->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
	spa_list_init(&port->queue);

	port = GET_IN_PORT(this, 0);
	port->id = 0;
	port->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
	spa_list_init(&port->queue);

	props_reset(&this->props);

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

const struct spa_handle_factory spa_channelmix_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	NAME,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
