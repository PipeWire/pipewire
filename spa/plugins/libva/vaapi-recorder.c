/*
 * Copyright (c) 2012 Intel Corporation. All Rights Reserved.
 * Copyright © 2013 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <pthread.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>
#include <va/va_vpp.h>

#include "compositor.h"
#include "vaapi-recorder.h"

#define NAL_REF_IDC_NONE        0
#define NAL_REF_IDC_LOW         1
#define NAL_REF_IDC_MEDIUM      2
#define NAL_REF_IDC_HIGH        3

#define NAL_NON_IDR             1
#define NAL_IDR                 5
#define NAL_SPS                 7
#define NAL_PPS                 8
#define NAL_SEI                 6

#define SLICE_TYPE_P            0
#define SLICE_TYPE_B            1
#define SLICE_TYPE_I            2

#define ENTROPY_MODE_CAVLC      0
#define ENTROPY_MODE_CABAC      1

#define PROFILE_IDC_BASELINE    66
#define PROFILE_IDC_MAIN        77
#define PROFILE_IDC_HIGH        100

struct vaapi_recorder {
	int drm_fd, output_fd;
	int width, height;
	int frame_count;

	int error;
	int destroying;
	pthread_t worker_thread;
	pthread_mutex_t mutex;
	pthread_cond_t input_cond;

	struct {
		int valid;
		int prime_fd, stride;
	} input;

	VADisplay va_dpy;

	/* video post processing is used for colorspace conversion */
	struct {
		VAConfigID cfg;
		VAContextID ctx;
		VABufferID pipeline_buf;
		VASurfaceID output;
	} vpp;

	struct {
		VAConfigID cfg;
		VAContextID ctx;
		VASurfaceID reference_picture[3];

		int intra_period;
		int output_size;
		int constraint_set_flag;

		struct {
			VAEncSequenceParameterBufferH264 seq;
			VAEncPictureParameterBufferH264 pic;
			VAEncSliceParameterBufferH264 slice;
		} param;
	} encoder;
};

static void *
worker_thread_function(void *);

/* bistream code used for writing the packed headers */

#define BITSTREAM_ALLOCATE_STEPPING	 4096

struct bitstream {
	unsigned int *buffer;
	int bit_offset;
	int max_size_in_dword;
};

static unsigned int
va_swap32(unsigned int val)
{
	unsigned char *pval = (unsigned char *)&val;

	return ((pval[0] << 24) |
		(pval[1] << 16) |
		(pval[2] << 8)  |
		(pval[3] << 0));
}

static void
bitstream_start(struct bitstream *bs)
{
	bs->max_size_in_dword = BITSTREAM_ALLOCATE_STEPPING;
	bs->buffer = calloc(bs->max_size_in_dword * sizeof(unsigned int), 1);
	bs->bit_offset = 0;
}

static void
bitstream_end(struct bitstream *bs)
{
	int pos = (bs->bit_offset >> 5);
	int bit_offset = (bs->bit_offset & 0x1f);
	int bit_left = 32 - bit_offset;

	if (bit_offset) {
		bs->buffer[pos] = va_swap32((bs->buffer[pos] << bit_left));
	}
}

static void
bitstream_put_ui(struct bitstream *bs, unsigned int val, int size_in_bits)
{
	int pos = (bs->bit_offset >> 5);
	int bit_offset = (bs->bit_offset & 0x1f);
	int bit_left = 32 - bit_offset;

	if (!size_in_bits)
		return;

	bs->bit_offset += size_in_bits;

	if (bit_left > size_in_bits) {
		bs->buffer[pos] = (bs->buffer[pos] << size_in_bits | val);
		return;
	}

	size_in_bits -= bit_left;
	bs->buffer[pos] =
		(bs->buffer[pos] << bit_left) | (val >> size_in_bits);
	bs->buffer[pos] = va_swap32(bs->buffer[pos]);

	if (pos + 1 == bs->max_size_in_dword) {
		bs->max_size_in_dword += BITSTREAM_ALLOCATE_STEPPING;
		bs->buffer =
			realloc(bs->buffer,
				bs->max_size_in_dword * sizeof(unsigned int));
	}

	bs->buffer[pos + 1] = val;
}

static void
bitstream_put_ue(struct bitstream *bs, unsigned int val)
{
	int size_in_bits = 0;
	int tmp_val = ++val;

	while (tmp_val) {
		tmp_val >>= 1;
		size_in_bits++;
	}

	bitstream_put_ui(bs, 0, size_in_bits - 1); /* leading zero */
	bitstream_put_ui(bs, val, size_in_bits);
}

static void
bitstream_put_se(struct bitstream *bs, int val)
{
	unsigned int new_val;

	if (val <= 0)
		new_val = -2 * val;
	else
		new_val = 2 * val - 1;

	bitstream_put_ue(bs, new_val);
}

static void
bitstream_byte_aligning(struct bitstream *bs, int bit)
{
	int bit_offset = (bs->bit_offset & 0x7);
	int bit_left = 8 - bit_offset;
	int new_val;

	if (!bit_offset)
		return;

	if (bit)
		new_val = (1 << bit_left) - 1;
	else
		new_val = 0;

	bitstream_put_ui(bs, new_val, bit_left);
}

static VAStatus
encoder_create_config(struct vaapi_recorder *r)
{
	VAConfigAttrib attrib[2];
	VAStatus status;

	/* FIXME: should check if VAEntrypointEncSlice is supported */

	/* FIXME: should check if specified attributes are supported */

	attrib[0].type = VAConfigAttribRTFormat;
	attrib[0].value = VA_RT_FORMAT_YUV420;

	attrib[1].type = VAConfigAttribRateControl;
	attrib[1].value = VA_RC_CQP;

	status = vaCreateConfig(r->va_dpy, VAProfileH264Main,
				VAEntrypointEncSlice, attrib, 2,
				&r->encoder.cfg);
	if (status != VA_STATUS_SUCCESS)
		return status;

	status = vaCreateContext(r->va_dpy, r->encoder.cfg,
				 r->width, r->height, VA_PROGRESSIVE, 0, 0,
				 &r->encoder.ctx);
	if (status != VA_STATUS_SUCCESS) {
		vaDestroyConfig(r->va_dpy, r->encoder.cfg);
		return status;
	}

	return VA_STATUS_SUCCESS;
}

static void
encoder_destroy_config(struct vaapi_recorder *r)
{
	vaDestroyContext(r->va_dpy, r->encoder.ctx);
	vaDestroyConfig(r->va_dpy, r->encoder.cfg);
}

static void
encoder_init_seq_parameters(struct vaapi_recorder *r)
{
	int width_in_mbs, height_in_mbs;
	int frame_cropping_flag = 0;
	int frame_crop_bottom_offset = 0;

	width_in_mbs = (r->width + 15) / 16;
	height_in_mbs = (r->height + 15) / 16;

	r->encoder.param.seq.level_idc = 41;
	r->encoder.param.seq.intra_period = r->encoder.intra_period;
	r->encoder.param.seq.max_num_ref_frames = 4;
	r->encoder.param.seq.picture_width_in_mbs = width_in_mbs;
	r->encoder.param.seq.picture_height_in_mbs = height_in_mbs;
	r->encoder.param.seq.seq_fields.bits.frame_mbs_only_flag = 1;

	/* Tc = num_units_in_tick / time_scale */
	r->encoder.param.seq.time_scale = 1800;
	r->encoder.param.seq.num_units_in_tick = 15;

	if (height_in_mbs * 16 - r->height > 0) {
		frame_cropping_flag = 1;
		frame_crop_bottom_offset = (height_in_mbs * 16 - r->height) / 2;
	}

	r->encoder.param.seq.frame_cropping_flag = frame_cropping_flag;
	r->encoder.param.seq.frame_crop_bottom_offset = frame_crop_bottom_offset;

	r->encoder.param.seq.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 2;
}

static VABufferID
encoder_update_seq_parameters(struct vaapi_recorder *r)
{
	VABufferID seq_buf;
	VAStatus status;

	status = vaCreateBuffer(r->va_dpy, r->encoder.ctx,
				VAEncSequenceParameterBufferType,
				sizeof(r->encoder.param.seq),
				1, &r->encoder.param.seq,
				&seq_buf);

	if (status == VA_STATUS_SUCCESS)
		return seq_buf;
	else
		return VA_INVALID_ID;
}

static void
encoder_init_pic_parameters(struct vaapi_recorder *r)
{
	VAEncPictureParameterBufferH264 *pic = &r->encoder.param.pic;

	pic->pic_init_qp = 0;

	/* ENTROPY_MODE_CABAC */
	pic->pic_fields.bits.entropy_coding_mode_flag = 1;

	pic->pic_fields.bits.deblocking_filter_control_present_flag = 1;
}

static VABufferID
encoder_update_pic_parameters(struct vaapi_recorder *r,
			      VABufferID output_buf)
{
	VAEncPictureParameterBufferH264 *pic = &r->encoder.param.pic;
	VAStatus status;
	VABufferID pic_param_buf;
	VASurfaceID curr_pic, pic0;

	curr_pic = r->encoder.reference_picture[r->frame_count % 2];
	pic0 = r->encoder.reference_picture[(r->frame_count + 1) % 2];

	pic->CurrPic.picture_id = curr_pic;
	pic->CurrPic.TopFieldOrderCnt = r->frame_count * 2;
	pic->ReferenceFrames[0].picture_id = pic0;
	pic->ReferenceFrames[1].picture_id = r->encoder.reference_picture[2];
	pic->ReferenceFrames[2].picture_id = VA_INVALID_ID;

	pic->coded_buf = output_buf;
	pic->frame_num = r->frame_count;

	pic->pic_fields.bits.idr_pic_flag = (r->frame_count == 0);
	pic->pic_fields.bits.reference_pic_flag = 1;

	status = vaCreateBuffer(r->va_dpy, r->encoder.ctx,
				VAEncPictureParameterBufferType,
				sizeof(VAEncPictureParameterBufferH264), 1,
				pic, &pic_param_buf);

	if (status == VA_STATUS_SUCCESS)
		return pic_param_buf;
	else
		return VA_INVALID_ID;
}

static VABufferID
encoder_update_slice_parameter(struct vaapi_recorder *r, int slice_type)
{
	VABufferID slice_param_buf;
	VAStatus status;

	int width_in_mbs = (r->width + 15) / 16;
	int height_in_mbs = (r->height + 15) / 16;

	memset(&r->encoder.param.slice, 0, sizeof r->encoder.param.slice);

	r->encoder.param.slice.num_macroblocks = width_in_mbs * height_in_mbs;
	r->encoder.param.slice.slice_type = slice_type;

	r->encoder.param.slice.slice_alpha_c0_offset_div2 = 2;
	r->encoder.param.slice.slice_beta_offset_div2 = 2;

	status = vaCreateBuffer(r->va_dpy, r->encoder.ctx,
				VAEncSliceParameterBufferType,
				sizeof(r->encoder.param.slice), 1,
				&r->encoder.param.slice,
				&slice_param_buf);

	if (status == VA_STATUS_SUCCESS)
		return slice_param_buf;
	else
		return VA_INVALID_ID;
}

static VABufferID
encoder_update_misc_hdr_parameter(struct vaapi_recorder *r)
{
	VAEncMiscParameterBuffer *misc_param;
	VAEncMiscParameterHRD *hrd;
	VABufferID buffer;
	VAStatus status;

	int total_size =
		sizeof(VAEncMiscParameterBuffer) +
		sizeof(VAEncMiscParameterRateControl);

	status = vaCreateBuffer(r->va_dpy, r->encoder.ctx,
				VAEncMiscParameterBufferType, total_size,
				1, NULL, &buffer);
	if (status != VA_STATUS_SUCCESS)
		return VA_INVALID_ID;

	status = vaMapBuffer(r->va_dpy, buffer, (void **) &misc_param);
	if (status != VA_STATUS_SUCCESS) {
		vaDestroyBuffer(r->va_dpy, buffer);
		return VA_INVALID_ID;
	}

	misc_param->type = VAEncMiscParameterTypeHRD;
	hrd = (VAEncMiscParameterHRD *) misc_param->data;

	hrd->initial_buffer_fullness = 0;
	hrd->buffer_size = 0;

	vaUnmapBuffer(r->va_dpy, buffer);

	return buffer;
}

static int
setup_encoder(struct vaapi_recorder *r)
{
	VAStatus status;

	status = encoder_create_config(r);
	if (status != VA_STATUS_SUCCESS) {
		return -1;
	}

	status = vaCreateSurfaces(r->va_dpy, VA_RT_FORMAT_YUV420,
				  r->width, r->height,
				  r->encoder.reference_picture, 3,
				  NULL, 0);
	if (status != VA_STATUS_SUCCESS) {
		encoder_destroy_config(r);
		return -1;
	}

	/* VAProfileH264Main */
	r->encoder.constraint_set_flag |= (1 << 1); /* Annex A.2.2 */

	r->encoder.output_size = r->width * r->height;

	r->encoder.intra_period = 30;

	encoder_init_seq_parameters(r);
	encoder_init_pic_parameters(r);

	return 0;
}

static void
encoder_destroy(struct vaapi_recorder *r)
{
	vaDestroySurfaces(r->va_dpy, r->encoder.reference_picture, 3);

	encoder_destroy_config(r);
}

static void
nal_start_code_prefix(struct bitstream *bs)
{
	bitstream_put_ui(bs, 0x00000001, 32);
}

static void
nal_header(struct bitstream *bs, int nal_ref_idc, int nal_unit_type)
{
	/* forbidden_zero_bit: 0 */
	bitstream_put_ui(bs, 0, 1);

	bitstream_put_ui(bs, nal_ref_idc, 2);
	bitstream_put_ui(bs, nal_unit_type, 5);
}

static void
rbsp_trailing_bits(struct bitstream *bs)
{
	bitstream_put_ui(bs, 1, 1);
	bitstream_byte_aligning(bs, 0);
}

static void sps_rbsp(struct bitstream *bs,
		     VAEncSequenceParameterBufferH264 *seq,
		     int constraint_set_flag)
{
	int i;

	bitstream_put_ui(bs, PROFILE_IDC_MAIN, 8);

	/* constraint_set[0-3] flag */
	for (i = 0; i < 4; i++) {
		int set = (constraint_set_flag & (1 << i)) ? 1 : 0;
		bitstream_put_ui(bs, set, 1);
	}

	/* reserved_zero_4bits */
	bitstream_put_ui(bs, 0, 4);
	bitstream_put_ui(bs, seq->level_idc, 8);
	bitstream_put_ue(bs, seq->seq_parameter_set_id);

	bitstream_put_ue(bs, seq->seq_fields.bits.log2_max_frame_num_minus4);
	bitstream_put_ue(bs, seq->seq_fields.bits.pic_order_cnt_type);
	bitstream_put_ue(bs,
			 seq->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);

	bitstream_put_ue(bs, seq->max_num_ref_frames);

	/* gaps_in_frame_num_value_allowed_flag */
	bitstream_put_ui(bs, 0, 1);

	/* pic_width_in_mbs_minus1, pic_height_in_map_units_minus1 */
	bitstream_put_ue(bs, seq->picture_width_in_mbs - 1);
	bitstream_put_ue(bs, seq->picture_height_in_mbs - 1);

	bitstream_put_ui(bs, seq->seq_fields.bits.frame_mbs_only_flag, 1);
	bitstream_put_ui(bs, seq->seq_fields.bits.direct_8x8_inference_flag, 1);

	bitstream_put_ui(bs, seq->frame_cropping_flag, 1);

	if (seq->frame_cropping_flag) {
		bitstream_put_ue(bs, seq->frame_crop_left_offset);
		bitstream_put_ue(bs, seq->frame_crop_right_offset);
		bitstream_put_ue(bs, seq->frame_crop_top_offset);
		bitstream_put_ue(bs, seq->frame_crop_bottom_offset);
	}

	/* vui_parameters_present_flag */
	bitstream_put_ui(bs, 1, 1);

	/* aspect_ratio_info_present_flag */
	bitstream_put_ui(bs, 0, 1);
	/* overscan_info_present_flag */
	bitstream_put_ui(bs, 0, 1);

	/* video_signal_type_present_flag */
	bitstream_put_ui(bs, 0, 1);
	/* chroma_loc_info_present_flag */
	bitstream_put_ui(bs, 0, 1);

	/* timing_info_present_flag */
	bitstream_put_ui(bs, 1, 1);
	bitstream_put_ui(bs, seq->num_units_in_tick, 32);
	bitstream_put_ui(bs, seq->time_scale, 32);
	/* fixed_frame_rate_flag */
	bitstream_put_ui(bs, 1, 1);

	/* nal_hrd_parameters_present_flag */
	bitstream_put_ui(bs, 0, 1);

	/* vcl_hrd_parameters_present_flag */
	bitstream_put_ui(bs, 0, 1);

	/* low_delay_hrd_flag */
	bitstream_put_ui(bs, 0, 1);

	/* pic_struct_present_flag */
	bitstream_put_ui(bs, 0, 1);
	/* bitstream_restriction_flag */
	bitstream_put_ui(bs, 0, 1);

	rbsp_trailing_bits(bs);
}

static void pps_rbsp(struct bitstream *bs,
		     VAEncPictureParameterBufferH264 *pic)
{
	/* pic_parameter_set_id, seq_parameter_set_id */
	bitstream_put_ue(bs, pic->pic_parameter_set_id);
	bitstream_put_ue(bs, pic->seq_parameter_set_id);

	bitstream_put_ui(bs, pic->pic_fields.bits.entropy_coding_mode_flag, 1);

	/* pic_order_present_flag: 0 */
	bitstream_put_ui(bs, 0, 1);

	/* num_slice_groups_minus1 */
	bitstream_put_ue(bs, 0);

	bitstream_put_ue(bs, pic->num_ref_idx_l0_active_minus1);
	bitstream_put_ue(bs, pic->num_ref_idx_l1_active_minus1);

	bitstream_put_ui(bs, pic->pic_fields.bits.weighted_pred_flag, 1);
	bitstream_put_ui(bs, pic->pic_fields.bits.weighted_bipred_idc, 2);

	/* pic_init_qp_minus26, pic_init_qs_minus26, chroma_qp_index_offset */
	bitstream_put_se(bs, pic->pic_init_qp - 26);
	bitstream_put_se(bs, 0);
	bitstream_put_se(bs, 0);

	bitstream_put_ui(bs, pic->pic_fields.bits.deblocking_filter_control_present_flag, 1);

	/* constrained_intra_pred_flag, redundant_pic_cnt_present_flag */
	bitstream_put_ui(bs, 0, 1);
	bitstream_put_ui(bs, 0, 1);

	bitstream_put_ui(bs, pic->pic_fields.bits.transform_8x8_mode_flag, 1);

	/* pic_scaling_matrix_present_flag */
	bitstream_put_ui(bs, 0, 1);
	bitstream_put_se(bs, pic->second_chroma_qp_index_offset );

	rbsp_trailing_bits(bs);
}

static int
build_packed_pic_buffer(struct vaapi_recorder *r,
			void **header_buffer)
{
	struct bitstream bs;

	bitstream_start(&bs);
	nal_start_code_prefix(&bs);
	nal_header(&bs, NAL_REF_IDC_HIGH, NAL_PPS);
	pps_rbsp(&bs, &r->encoder.param.pic);
	bitstream_end(&bs);

	*header_buffer = bs.buffer;
	return bs.bit_offset;
}

static int
build_packed_seq_buffer(struct vaapi_recorder *r,
			void **header_buffer)
{
	struct bitstream bs;

	bitstream_start(&bs);
	nal_start_code_prefix(&bs);
	nal_header(&bs, NAL_REF_IDC_HIGH, NAL_SPS);
	sps_rbsp(&bs, &r->encoder.param.seq, r->encoder.constraint_set_flag);
	bitstream_end(&bs);

	*header_buffer = bs.buffer;
	return bs.bit_offset;
}

static int
create_packed_header_buffers(struct vaapi_recorder *r, VABufferID *buffers,
			     VAEncPackedHeaderType type,
			     void *data, int bit_length)
{
	VAEncPackedHeaderParameterBuffer packed_header;
	VAStatus status;

	packed_header.type = type;
	packed_header.bit_length = bit_length;
	packed_header.has_emulation_bytes = 0;

	status = vaCreateBuffer(r->va_dpy, r->encoder.ctx,
				VAEncPackedHeaderParameterBufferType,
				sizeof packed_header, 1, &packed_header,
				&buffers[0]);
	if (status != VA_STATUS_SUCCESS)
		return 0;

	status = vaCreateBuffer(r->va_dpy, r->encoder.ctx,
				VAEncPackedHeaderDataBufferType,
				(bit_length + 7) / 8, 1, data, &buffers[1]);
	if (status != VA_STATUS_SUCCESS) {
		vaDestroyBuffer(r->va_dpy, buffers[0]);
		return 0;
	}

	return 2;
}

static int
encoder_prepare_headers(struct vaapi_recorder *r, VABufferID *buffers)
{
	VABufferID *p;

	int bit_length;
	void *data;

	p = buffers;

	bit_length = build_packed_seq_buffer(r, &data);
	p += create_packed_header_buffers(r, p, VAEncPackedHeaderSequence,
					  data, bit_length);
	free(data);

	bit_length = build_packed_pic_buffer(r, &data);
	p += create_packed_header_buffers(r, p, VAEncPackedHeaderPicture,
					  data, bit_length);
	free(data);

	return p - buffers;
}

static VAStatus
encoder_render_picture(struct vaapi_recorder *r, VASurfaceID input,
		       VABufferID *buffers, int count)
{
	VAStatus status;

	status = vaBeginPicture(r->va_dpy, r->encoder.ctx, input);
	if (status != VA_STATUS_SUCCESS)
		return status;

	status = vaRenderPicture(r->va_dpy, r->encoder.ctx, buffers, count);
	if (status != VA_STATUS_SUCCESS)
		return status;

	status = vaEndPicture(r->va_dpy, r->encoder.ctx);
	if (status != VA_STATUS_SUCCESS)
		return status;

	return vaSyncSurface(r->va_dpy, input);
}

static VABufferID
encoder_create_output_buffer(struct vaapi_recorder *r)
{
	VABufferID output_buf;
	VAStatus status;

	status = vaCreateBuffer(r->va_dpy, r->encoder.ctx,
				VAEncCodedBufferType, r->encoder.output_size,
				1, NULL, &output_buf);
	if (status == VA_STATUS_SUCCESS)
		return output_buf;
	else
		return VA_INVALID_ID;
}

enum output_write_status {
	OUTPUT_WRITE_SUCCESS,
	OUTPUT_WRITE_OVERFLOW,
	OUTPUT_WRITE_FATAL
};

static enum output_write_status
encoder_write_output(struct vaapi_recorder *r, VABufferID output_buf)
{
	VACodedBufferSegment *segment;
	VAStatus status;
	int count;

	status = vaMapBuffer(r->va_dpy, output_buf, (void **) &segment);
	if (status != VA_STATUS_SUCCESS)
		return OUTPUT_WRITE_FATAL;

	if (segment->status & VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK) {
		r->encoder.output_size *= 2;
		vaUnmapBuffer(r->va_dpy, output_buf);
		return OUTPUT_WRITE_OVERFLOW;
	}

	count = write(r->output_fd, segment->buf, segment->size);

	vaUnmapBuffer(r->va_dpy, output_buf);

	if (count < 0)
		return OUTPUT_WRITE_FATAL;

	return OUTPUT_WRITE_SUCCESS;
}

static void
encoder_encode(struct vaapi_recorder *r, VASurfaceID input)
{
	VABufferID output_buf = VA_INVALID_ID;

	VABufferID buffers[8];
	int count = 0;
	int i, slice_type;
	enum output_write_status ret;

	if ((r->frame_count % r->encoder.intra_period) == 0)
		slice_type = SLICE_TYPE_I;
	else
		slice_type = SLICE_TYPE_P;

	buffers[count++] = encoder_update_seq_parameters(r);
	buffers[count++] = encoder_update_misc_hdr_parameter(r);
	buffers[count++] = encoder_update_slice_parameter(r, slice_type);

	for (i = 0; i < count; i++)
		if (buffers[i] == VA_INVALID_ID)
			goto bail;

	if (r->frame_count == 0)
		count += encoder_prepare_headers(r, buffers + count);

	do {
		output_buf = encoder_create_output_buffer(r);
		if (output_buf == VA_INVALID_ID)
			goto bail;

		buffers[count++] =
			encoder_update_pic_parameters(r, output_buf);
		if (buffers[count - 1] == VA_INVALID_ID)
			goto bail;

		encoder_render_picture(r, input, buffers, count);
		ret = encoder_write_output(r, output_buf);

		vaDestroyBuffer(r->va_dpy, output_buf);
		output_buf = VA_INVALID_ID;

		vaDestroyBuffer(r->va_dpy, buffers[--count]);
	} while (ret == OUTPUT_WRITE_OVERFLOW);

	if (ret == OUTPUT_WRITE_FATAL)
		r->error = errno;

	for (i = 0; i < count; i++)
		vaDestroyBuffer(r->va_dpy, buffers[i]);

	r->frame_count++;
	return;

bail:
	for (i = 0; i < count; i++)
		vaDestroyBuffer(r->va_dpy, buffers[i]);
	if (output_buf != VA_INVALID_ID)
		vaDestroyBuffer(r->va_dpy, output_buf);
}


static int
setup_vpp(struct vaapi_recorder *r)
{
	VAStatus status;

	status = vaCreateConfig(r->va_dpy, VAProfileNone,
				VAEntrypointVideoProc, NULL, 0,
				&r->vpp.cfg);
	if (status != VA_STATUS_SUCCESS) {
		weston_log("vaapi: failed to create VPP config\n");
		return -1;
	}

	status = vaCreateContext(r->va_dpy, r->vpp.cfg, r->width, r->height,
				 0, NULL, 0, &r->vpp.ctx);
	if (status != VA_STATUS_SUCCESS) {
		weston_log("vaapi: failed to create VPP context\n");
		goto err_cfg;
	}

	status = vaCreateBuffer(r->va_dpy, r->vpp.ctx,
				VAProcPipelineParameterBufferType,
				sizeof(VAProcPipelineParameterBuffer),
				1, NULL, &r->vpp.pipeline_buf);
	if (status != VA_STATUS_SUCCESS) {
		weston_log("vaapi: failed to create VPP pipeline buffer\n");
		goto err_ctx;
	}

	status = vaCreateSurfaces(r->va_dpy, VA_RT_FORMAT_YUV420,
				  r->width, r->height, &r->vpp.output, 1,
				  NULL, 0);
	if (status != VA_STATUS_SUCCESS) {
		weston_log("vaapi: failed to create YUV surface\n");
		goto err_buf;
	}

	return 0;

err_buf:
	vaDestroyBuffer(r->va_dpy, r->vpp.pipeline_buf);
err_ctx:
	vaDestroyConfig(r->va_dpy, r->vpp.ctx);
err_cfg:
	vaDestroyConfig(r->va_dpy, r->vpp.cfg);

	return -1;
}

static void
vpp_destroy(struct vaapi_recorder *r)
{
	vaDestroySurfaces(r->va_dpy, &r->vpp.output, 1);
	vaDestroyBuffer(r->va_dpy, r->vpp.pipeline_buf);
	vaDestroyConfig(r->va_dpy, r->vpp.ctx);
	vaDestroyConfig(r->va_dpy, r->vpp.cfg);
}

static int
setup_worker_thread(struct vaapi_recorder *r)
{
	pthread_mutex_init(&r->mutex, NULL);
	pthread_cond_init(&r->input_cond, NULL);
	pthread_create(&r->worker_thread, NULL, worker_thread_function, r);

	return 1;
}

static void
destroy_worker_thread(struct vaapi_recorder *r)
{
	pthread_mutex_lock(&r->mutex);

	/* Make sure the worker thread finishes */
	r->destroying = 1;
	pthread_cond_signal(&r->input_cond);

	pthread_mutex_unlock(&r->mutex);

	pthread_join(r->worker_thread, NULL);

	pthread_mutex_destroy(&r->mutex);
	pthread_cond_destroy(&r->input_cond);
}

struct vaapi_recorder *
vaapi_recorder_create(int drm_fd, int width, int height, const char *filename)
{
	struct vaapi_recorder *r;
	VAStatus status;
	int major, minor;
	int flags;

	r = zalloc(sizeof *r);
	if (r == NULL)
		return NULL;

	r->width = width;
	r->height = height;
	r->drm_fd = drm_fd;

	if (setup_worker_thread(r) < 0)
		goto err_free;

	flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
	r->output_fd = open(filename, flags, 0644);
	if (r->output_fd < 0)
		goto err_thread;

	r->va_dpy = vaGetDisplayDRM(drm_fd);
	if (!r->va_dpy) {
		weston_log("failed to create VA display\n");
		goto err_fd;
	}

	status = vaInitialize(r->va_dpy, &major, &minor);
	if (status != VA_STATUS_SUCCESS) {
		weston_log("vaapi: failed to initialize display\n");
		goto err_fd;
	}

	if (setup_vpp(r) < 0) {
		weston_log("vaapi: failed to initialize VPP pipeline\n");
		goto err_va_dpy;
	}

	if (setup_encoder(r) < 0) {
		goto err_vpp;
	}

	return r;

err_vpp:
	vpp_destroy(r);
err_va_dpy:
	vaTerminate(r->va_dpy);
err_fd:
	close(r->output_fd);
err_thread:
	destroy_worker_thread(r);
err_free:
	free(r);

	return NULL;
}

void
vaapi_recorder_destroy(struct vaapi_recorder *r)
{
	destroy_worker_thread(r);

	encoder_destroy(r);
	vpp_destroy(r);

	vaTerminate(r->va_dpy);

	close(r->output_fd);
	close(r->drm_fd);

	free(r);
}

static VAStatus
create_surface_from_fd(struct vaapi_recorder *r, int prime_fd,
		       int stride, VASurfaceID *surface)
{
	VASurfaceAttrib va_attribs[2];
	VASurfaceAttribExternalBuffers va_attrib_extbuf;
	VAStatus status;

	unsigned long buffer_fd = prime_fd;

	va_attrib_extbuf.pixel_format = VA_FOURCC_BGRX;
	va_attrib_extbuf.width = r->width;
	va_attrib_extbuf.height = r->height;
	va_attrib_extbuf.data_size = r->height * stride;
	va_attrib_extbuf.num_planes = 1;
	va_attrib_extbuf.pitches[0] = stride;
	va_attrib_extbuf.offsets[0] = 0;
	va_attrib_extbuf.buffers = &buffer_fd;
	va_attrib_extbuf.num_buffers = 1;
	va_attrib_extbuf.flags = 0;
	va_attrib_extbuf.private_data = NULL;

	va_attribs[0].type = VASurfaceAttribMemoryType;
	va_attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
	va_attribs[0].value.type = VAGenericValueTypeInteger;
	va_attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;

	va_attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
	va_attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
	va_attribs[1].value.type = VAGenericValueTypePointer;
	va_attribs[1].value.value.p = &va_attrib_extbuf;

	status = vaCreateSurfaces(r->va_dpy, VA_RT_FORMAT_RGB32,
				  r->width, r->height, surface, 1,
				  va_attribs, 2);

	return status;
}

static VAStatus
convert_rgb_to_yuv(struct vaapi_recorder *r, VASurfaceID rgb_surface)
{
	VAProcPipelineParameterBuffer *pipeline_param;
	VAStatus status;

	status = vaMapBuffer(r->va_dpy, r->vpp.pipeline_buf,
			     (void **) &pipeline_param);
	if (status != VA_STATUS_SUCCESS)
		return status;

	memset(pipeline_param, 0, sizeof *pipeline_param);

	pipeline_param->surface = rgb_surface;
	pipeline_param->surface_color_standard  = VAProcColorStandardNone;

	pipeline_param->output_background_color = 0xff000000;
	pipeline_param->output_color_standard   = VAProcColorStandardNone;

	status = vaUnmapBuffer(r->va_dpy, r->vpp.pipeline_buf);
	if (status != VA_STATUS_SUCCESS)
		return status;

	status = vaBeginPicture(r->va_dpy, r->vpp.ctx, r->vpp.output);
	if (status != VA_STATUS_SUCCESS)
		return status;

	status = vaRenderPicture(r->va_dpy, r->vpp.ctx,
				 &r->vpp.pipeline_buf, 1);
	if (status != VA_STATUS_SUCCESS)
		return status;

	status = vaEndPicture(r->va_dpy, r->vpp.ctx);
	if (status != VA_STATUS_SUCCESS)
		return status;

	return status;
}

static void
recorder_frame(struct vaapi_recorder *r)
{
	VASurfaceID rgb_surface;
	VAStatus status;

	status = create_surface_from_fd(r, r->input.prime_fd,
					r->input.stride, &rgb_surface);
	if (status != VA_STATUS_SUCCESS) {
		weston_log("[libva recorder] "
			   "failed to create surface from bo\n");
		return;
	}

	close(r->input.prime_fd);

	status = convert_rgb_to_yuv(r, rgb_surface);
	if (status != VA_STATUS_SUCCESS) {
		weston_log("[libva recorder] "
			   "color space conversion failed\n");
		return;
	}

	encoder_encode(r, r->vpp.output);

	vaDestroySurfaces(r->va_dpy, &rgb_surface, 1);
}

static void *
worker_thread_function(void *data)
{
	struct vaapi_recorder *r = data;

	pthread_mutex_lock(&r->mutex);

	while (!r->destroying) {
		if (!r->input.valid)
			pthread_cond_wait(&r->input_cond, &r->mutex);

		/* If the thread is awaken by destroy_worker_thread(),
		 * there might not be valid input */
		if (!r->input.valid)
			continue;

		recorder_frame(r);
		r->input.valid = 0;
	}

	pthread_mutex_unlock(&r->mutex);

	return NULL;
}

int
vaapi_recorder_frame(struct vaapi_recorder *r, int prime_fd, int stride)
{
	int ret = 0;

	pthread_mutex_lock(&r->mutex);

	if (r->error) {
		errno = r->error;
		ret = -1;
		goto unlock;
	}

	/* The mutex is never released while encoding, so this point should
	 * never be reached if input.valid is true. */
	assert(!r->input.valid);

	r->input.prime_fd = prime_fd;
	r->input.stride = stride;
	r->input.valid = 1;
	pthread_cond_signal(&r->input_cond);

unlock:
	pthread_mutex_unlock(&r->mutex);

	return ret;
}
