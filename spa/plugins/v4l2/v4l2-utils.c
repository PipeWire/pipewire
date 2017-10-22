/* Spa
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>

static void v4l2_on_fd_events(struct spa_source *source);

static int xioctl(int fd, int request, void *arg)
{
	int err;

	do {
		err = ioctl(fd, request, arg);
	} while (err == -1 && errno == EINTR);

	return err;
}

static int spa_v4l2_open(struct impl *this)
{
	struct port *state = &this->out_ports[0];
	struct stat st;
	struct props *props = &this->props;

	if (state->opened)
		return 0;

	if (props->device[0] == '\0') {
		spa_log_error(state->log, "v4l2: Device property not set");
		return -1;
	}

	spa_log_info(state->log, "v4l2: Playback device is '%s'", props->device);

	if (stat(props->device, &st) < 0) {
		spa_log_error(state->log, "v4l2: Cannot identify '%s': %d, %s",
			      props->device, errno, strerror(errno));
		return -1;
	}

	if (!S_ISCHR(st.st_mode)) {
		spa_log_error(state->log, "v4l2: %s is no device", props->device);
		return -1;
	}

	state->fd = open(props->device, O_RDWR | O_NONBLOCK, 0);

	if (state->fd == -1) {
		spa_log_error(state->log, "v4l2: Cannot open '%s': %d, %s",
			      props->device, errno, strerror(errno));
		return -1;
	}

	if (xioctl(state->fd, VIDIOC_QUERYCAP, &state->cap) < 0) {
		perror("QUERYCAP");
		return -1;
	}

	if ((state->cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
		spa_log_error(state->log, "v4l2: %s is no video capture device", props->device);
		return -1;
	}

	state->source.func = v4l2_on_fd_events;
	state->source.data = this;
	state->source.fd = state->fd;
	state->source.mask = SPA_IO_IN | SPA_IO_ERR;
	state->source.rmask = 0;

	state->opened = true;

	return 0;
}

static int spa_v4l2_buffer_recycle(struct impl *this, uint32_t buffer_id)
{
	struct port *state = &this->out_ports[0];
	struct buffer *b = &state->buffers[buffer_id];

	if (!b->outstanding)
		return SPA_RESULT_OK;

	b->outstanding = false;
	spa_log_trace(state->log, "v4l2 %p: recycle buffer %d", this, buffer_id);

	if (xioctl(state->fd, VIDIOC_QBUF, &b->v4l2_buffer) < 0) {
		perror("VIDIOC_QBUF");
	}
	return SPA_RESULT_OK;
}

static int spa_v4l2_clear_buffers(struct impl *this)
{
	struct port *state = &this->out_ports[0];
	struct v4l2_requestbuffers reqbuf;
	int i;

	if (state->n_buffers == 0)
		return SPA_RESULT_OK;

	for (i = 0; i < state->n_buffers; i++) {
		struct buffer *b;

		b = &state->buffers[i];
		if (b->outstanding) {
			spa_log_info(state->log, "v4l2: queueing outstanding buffer %p", b);
			spa_v4l2_buffer_recycle(this, i);
		}
		if (b->allocated) {
			if (b->outbuf->datas[0].data)
				munmap(b->outbuf->datas[0].data, b->outbuf->datas[0].maxsize);
			if (b->outbuf->datas[0].fd != -1)
				close(b->outbuf->datas[0].fd);
			b->outbuf->datas[0].type = SPA_ID_INVALID;
		}
	}

	spa_zero(reqbuf);
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqbuf.memory = state->memtype;
	reqbuf.count = 0;

	if (xioctl(state->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		perror("VIDIOC_REQBUFS");
	}
	state->n_buffers = 0;

	return SPA_RESULT_OK;
}

static int spa_v4l2_port_set_enabled(struct impl *this, bool enabled)
{
	struct port *state = &this->out_ports[0];
	if (state->source_enabled != enabled) {
		spa_log_info(state->log, "v4l2: enabled %d", enabled);
		state->source_enabled = enabled;
		if (enabled)
			spa_loop_add_source(state->data_loop, &state->source);
		else
			spa_loop_remove_source(state->data_loop, &state->source);
	}
	return SPA_RESULT_OK;
}

static int spa_v4l2_close(struct impl *this)
{
	struct port *state = &this->out_ports[0];

	if (!state->opened)
		return 0;

	if (state->n_buffers > 0)
		return 0;

	spa_v4l2_port_set_enabled(this, false);

	spa_log_info(state->log, "v4l2: close");

	if (close(state->fd))
		perror("close");

	state->fd = -1;
	state->opened = false;

	return 0;
}

struct format_info {
	uint32_t fourcc;
	off_t format_offset;
	off_t media_type_offset;
	off_t media_subtype_offset;
};

#define VIDEO   offsetof(struct type, media_type.video)
#define IMAGE   offsetof(struct type, media_type.image)

#define RAW     offsetof(struct type, media_subtype.raw)

#define BAYER   offsetof(struct type, media_subtype_video.bayer)
#define MJPG    offsetof(struct type, media_subtype_video.mjpg)
#define JPEG    offsetof(struct type, media_subtype_video.jpeg)
#define DV      offsetof(struct type, media_subtype_video.dv)
#define MPEGTS  offsetof(struct type, media_subtype_video.mpegts)
#define H264    offsetof(struct type, media_subtype_video.h264)
#define H263    offsetof(struct type, media_subtype_video.h263)
#define MPEG1   offsetof(struct type, media_subtype_video.mpeg1)
#define MPEG2   offsetof(struct type, media_subtype_video.mpeg2)
#define MPEG4   offsetof(struct type, media_subtype_video.mpeg4)
#define XVID    offsetof(struct type, media_subtype_video.xvid)
#define VC1     offsetof(struct type, media_subtype_video.vc1)
#define VP8     offsetof(struct type, media_subtype_video.vp8)

#define FORMAT_UNKNOWN    offsetof(struct type, video_format.UNKNOWN)
#define FORMAT_ENCODED    offsetof(struct type, video_format.ENCODED)
#define FORMAT_RGB15      offsetof(struct type, video_format.RGB15)
#define FORMAT_BGR15      offsetof(struct type, video_format.BGR15)
#define FORMAT_RGB16      offsetof(struct type, video_format.RGB16)
#define FORMAT_BGR        offsetof(struct type, video_format.BGR)
#define FORMAT_RGB        offsetof(struct type, video_format.RGB)
#define FORMAT_BGRA       offsetof(struct type, video_format.BGRA)
#define FORMAT_BGRx       offsetof(struct type, video_format.BGRx)
#define FORMAT_ARGB       offsetof(struct type, video_format.ARGB)
#define FORMAT_xRGB       offsetof(struct type, video_format.xRGB)
#define FORMAT_GRAY8      offsetof(struct type, video_format.GRAY8)
#define FORMAT_GRAY16_LE  offsetof(struct type, video_format.GRAY16_LE)
#define FORMAT_GRAY16_BE  offsetof(struct type, video_format.GRAY16_BE)
#define FORMAT_YVU9       offsetof(struct type, video_format.YVU9)
#define FORMAT_YV12       offsetof(struct type, video_format.YV12)
#define FORMAT_YUY2       offsetof(struct type, video_format.YUY2)
#define FORMAT_YVYU       offsetof(struct type, video_format.YVYU)
#define FORMAT_UYVY       offsetof(struct type, video_format.UYVY)
#define FORMAT_Y42B       offsetof(struct type, video_format.Y42B)
#define FORMAT_Y41B       offsetof(struct type, video_format.Y41B)
#define FORMAT_YUV9       offsetof(struct type, video_format.YUV9)
#define FORMAT_I420       offsetof(struct type, video_format.I420)
#define FORMAT_NV12       offsetof(struct type, video_format.NV12)
#define FORMAT_NV12_64Z32 offsetof(struct type, video_format.NV12_64Z32)
#define FORMAT_NV21       offsetof(struct type, video_format.NV21)
#define FORMAT_NV16       offsetof(struct type, video_format.NV16)
#define FORMAT_NV61       offsetof(struct type, video_format.NV61)
#define FORMAT_NV24       offsetof(struct type, video_format.NV24)

static const struct format_info format_info[] = {
	/* RGB formats */
	{V4L2_PIX_FMT_RGB332, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_ARGB555, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_XRGB555, FORMAT_RGB15, VIDEO, RAW},
	{V4L2_PIX_FMT_ARGB555X, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_XRGB555X, FORMAT_BGR15, VIDEO, RAW},
	{V4L2_PIX_FMT_RGB565, FORMAT_RGB16, VIDEO, RAW},
	{V4L2_PIX_FMT_RGB565X, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_BGR666, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_BGR24, FORMAT_BGR, VIDEO, RAW},
	{V4L2_PIX_FMT_RGB24, FORMAT_RGB, VIDEO, RAW},
	{V4L2_PIX_FMT_ABGR32, FORMAT_BGRA, VIDEO, RAW},
	{V4L2_PIX_FMT_XBGR32, FORMAT_BGRx, VIDEO, RAW},
	{V4L2_PIX_FMT_ARGB32, FORMAT_ARGB, VIDEO, RAW},
	{V4L2_PIX_FMT_XRGB32, FORMAT_xRGB, VIDEO, RAW},

	/* Deprecated Packed RGB Image Formats (alpha ambiguity) */
	{V4L2_PIX_FMT_RGB444, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_RGB555, FORMAT_RGB15, VIDEO, RAW},
	{V4L2_PIX_FMT_RGB555X, FORMAT_BGR15, VIDEO, RAW},
	{V4L2_PIX_FMT_BGR32, FORMAT_BGRx, VIDEO, RAW},
	{V4L2_PIX_FMT_RGB32, FORMAT_xRGB, VIDEO, RAW},

	/* Grey formats */
	{V4L2_PIX_FMT_GREY, FORMAT_GRAY8, VIDEO, RAW},
	{V4L2_PIX_FMT_Y4, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_Y6, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_Y10, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_Y12, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_Y16, FORMAT_GRAY16_LE, VIDEO, RAW},
	{V4L2_PIX_FMT_Y16_BE, FORMAT_GRAY16_BE, VIDEO, RAW},
	{V4L2_PIX_FMT_Y10BPACK, FORMAT_UNKNOWN, VIDEO, RAW},

	/* Palette formats */
	{V4L2_PIX_FMT_PAL8, FORMAT_UNKNOWN, VIDEO, RAW},

	/* Chrominance formats */
	{V4L2_PIX_FMT_UV8, FORMAT_UNKNOWN, VIDEO, RAW},

	/* Luminance+Chrominance formats */
	{V4L2_PIX_FMT_YVU410, FORMAT_YVU9, VIDEO, RAW},
	{V4L2_PIX_FMT_YVU420, FORMAT_YV12, VIDEO, RAW},
	{V4L2_PIX_FMT_YVU420M, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_YUYV, FORMAT_YUY2, VIDEO, RAW},
	{V4L2_PIX_FMT_YYUV, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_YVYU, FORMAT_YVYU, VIDEO, RAW},
	{V4L2_PIX_FMT_UYVY, FORMAT_UYVY, VIDEO, RAW},
	{V4L2_PIX_FMT_VYUY, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV422P, FORMAT_Y42B, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV411P, FORMAT_Y41B, VIDEO, RAW},
	{V4L2_PIX_FMT_Y41P, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV444, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV555, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV565, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV32, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV410, FORMAT_YUV9, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV420, FORMAT_I420, VIDEO, RAW},
	{V4L2_PIX_FMT_YUV420M, FORMAT_I420, VIDEO, RAW},
	{V4L2_PIX_FMT_HI240, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_HM12, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_M420, FORMAT_UNKNOWN, VIDEO, RAW},

	/* two planes -- one Y, one Cr + Cb interleaved  */
	{V4L2_PIX_FMT_NV12, FORMAT_NV12, VIDEO, RAW},
	{V4L2_PIX_FMT_NV12M, FORMAT_NV12, VIDEO, RAW},
	{V4L2_PIX_FMT_NV12MT, FORMAT_NV12_64Z32, VIDEO, RAW},
	{V4L2_PIX_FMT_NV12MT_16X16, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_NV21, FORMAT_NV21, VIDEO, RAW},
	{V4L2_PIX_FMT_NV21M, FORMAT_NV21, VIDEO, RAW},
	{V4L2_PIX_FMT_NV16, FORMAT_NV16, VIDEO, RAW},
	{V4L2_PIX_FMT_NV16M, FORMAT_NV16, VIDEO, RAW},
	{V4L2_PIX_FMT_NV61, FORMAT_NV61, VIDEO, RAW},
	{V4L2_PIX_FMT_NV61M, FORMAT_NV61, VIDEO, RAW},
	{V4L2_PIX_FMT_NV24, FORMAT_NV24, VIDEO, RAW},
	{V4L2_PIX_FMT_NV42, FORMAT_UNKNOWN, VIDEO, RAW},

	/* Bayer formats - see http://www.siliconimaging.com/RGB%20Bayer.htm */
	{V4L2_PIX_FMT_SBGGR8, FORMAT_UNKNOWN, VIDEO, BAYER},
	{V4L2_PIX_FMT_SGBRG8, FORMAT_UNKNOWN, VIDEO, BAYER},
	{V4L2_PIX_FMT_SGRBG8, FORMAT_UNKNOWN, VIDEO, BAYER},
	{V4L2_PIX_FMT_SRGGB8, FORMAT_UNKNOWN, VIDEO, BAYER},

	/* compressed formats */
	{V4L2_PIX_FMT_MJPEG, FORMAT_ENCODED, VIDEO, MJPG},
	{V4L2_PIX_FMT_JPEG, FORMAT_ENCODED, IMAGE, JPEG},
	{V4L2_PIX_FMT_PJPG, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_DV, FORMAT_ENCODED, VIDEO, DV},
	{V4L2_PIX_FMT_MPEG, FORMAT_ENCODED, VIDEO, MPEGTS},
	{V4L2_PIX_FMT_H264, FORMAT_ENCODED, VIDEO, H264},
	{V4L2_PIX_FMT_H264_NO_SC, FORMAT_ENCODED, VIDEO, H264},
	{V4L2_PIX_FMT_H264_MVC, FORMAT_ENCODED, VIDEO, H264},
	{V4L2_PIX_FMT_H263, FORMAT_ENCODED, VIDEO, H263},
	{V4L2_PIX_FMT_MPEG1, FORMAT_ENCODED, VIDEO, MPEG1},
	{V4L2_PIX_FMT_MPEG2, FORMAT_ENCODED, VIDEO, MPEG2},
	{V4L2_PIX_FMT_MPEG4, FORMAT_ENCODED, VIDEO, MPEG4},
	{V4L2_PIX_FMT_XVID, FORMAT_ENCODED, VIDEO, XVID},
	{V4L2_PIX_FMT_VC1_ANNEX_G, FORMAT_ENCODED, VIDEO, VC1},
	{V4L2_PIX_FMT_VC1_ANNEX_L, FORMAT_ENCODED, VIDEO, VC1},
	{V4L2_PIX_FMT_VP8, FORMAT_ENCODED, VIDEO, VP8},

	/*  Vendor-specific formats   */
	{V4L2_PIX_FMT_WNVA, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_SN9C10X, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_PWC1, FORMAT_UNKNOWN, VIDEO, RAW},
	{V4L2_PIX_FMT_PWC2, FORMAT_UNKNOWN, VIDEO, RAW},
};

static const struct format_info *fourcc_to_format_info(uint32_t fourcc)
{
	int i;

	for (i = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		if (format_info[i].fourcc == fourcc)
			return &format_info[i];
	}
	return NULL;
}

#if 0
static const struct format_info *video_format_to_format_info(uint32_t format)
{
	int i;

	for (i = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		if (format_info[i].format == format)
			return &format_info[i];
	}
	return NULL;
}
#endif

static const struct format_info *find_format_info_by_media_type(struct type *types,
								uint32_t type,
								uint32_t subtype,
								uint32_t format,
								int startidx)
{
	int i;

	for (i = startidx; i < SPA_N_ELEMENTS(format_info); i++) {
		uint32_t media_type, media_subtype, media_format;

		media_type = *SPA_MEMBER(types, format_info[i].media_type_offset, uint32_t);
		media_subtype = *SPA_MEMBER(types, format_info[i].media_subtype_offset, uint32_t);
		media_format = *SPA_MEMBER(types, format_info[i].format_offset, uint32_t);

		if ((media_type == type) &&
		    (media_subtype == subtype) && (format == 0 || media_format == format))
			return &format_info[i];
	}
	return NULL;
}

static uint32_t
enum_filter_format(struct type *type, const struct spa_format *filter, uint32_t index)
{
	uint32_t video_format = 0;

	if ((filter->body.media_type.value == type->media_type.video ||
	     filter->body.media_type.value == type->media_type.image)) {
		if (filter->body.media_subtype.value == type->media_subtype.raw) {
			struct spa_pod_prop *p;
			uint32_t n_values;
			const uint32_t *values;

			if (!(p = spa_format_find_prop(filter, type->format_video.format)))
				return type->video_format.UNKNOWN;

			if (p->body.value.type != SPA_POD_TYPE_ID)
				return type->video_format.UNKNOWN;

			values = SPA_POD_BODY_CONST(&p->body.value);
			n_values = SPA_POD_PROP_N_VALUES(p);

			if (p->body.flags & SPA_POD_PROP_FLAG_UNSET) {
				if (index + 1 < n_values)
					video_format = values[index + 1];
			} else {
				if (index == 0)
					video_format = values[0];
			}
		} else {
			if (index == 0)
				video_format = type->video_format.ENCODED;
		}
	}
	return video_format;
}

static bool
filter_framesize(struct v4l2_frmsizeenum *frmsize,
		 const struct spa_rectangle *min,
		 const struct spa_rectangle *max,
		 const struct spa_rectangle *step)
{
	if (frmsize->type == V4L2_FRMSIZE_TYPE_DISCRETE) {
		if (frmsize->discrete.width < min->width ||
		    frmsize->discrete.height < min->height ||
		    frmsize->discrete.width > max->width ||
		    frmsize->discrete.height > max->height) {
			return false;
		}
	} else if (frmsize->type == V4L2_FRMSIZE_TYPE_CONTINUOUS ||
		   frmsize->type == V4L2_FRMSIZE_TYPE_STEPWISE) {
		/* FIXME, use LCM */
		frmsize->stepwise.step_width *= step->width;
		frmsize->stepwise.step_height *= step->height;

		if (frmsize->stepwise.max_width < min->width ||
		    frmsize->stepwise.max_height < min->height ||
		    frmsize->stepwise.min_width > max->width ||
		    frmsize->stepwise.min_height > max->height)
			return false;

		frmsize->stepwise.min_width = SPA_MAX(frmsize->stepwise.min_width, min->width);
		frmsize->stepwise.min_height = SPA_MAX(frmsize->stepwise.min_height, min->height);
		frmsize->stepwise.max_width = SPA_MIN(frmsize->stepwise.max_width, max->width);
		frmsize->stepwise.max_height = SPA_MIN(frmsize->stepwise.max_height, max->height);
	} else
		return false;

	return true;
}

static int compare_fraction(struct v4l2_fract *f1, const struct spa_fraction *f2)
{
	uint64_t n1, n2;

	/* fractions are reduced when set, so we can quickly see if they're equal */
	if (f1->denominator == f2->num && f1->numerator == f2->denom)
		return 0;

	/* extend to 64 bits */
	n1 = ((int64_t) f1->denominator) * f2->denom;
	n2 = ((int64_t) f1->numerator) * f2->num;
	if (n1 < n2)
		return -1;
	return 1;
}

static bool
filter_framerate(struct v4l2_frmivalenum *frmival,
		 const struct spa_fraction *min,
		 const struct spa_fraction *max,
		 const struct spa_fraction *step)
{
	if (frmival->type == V4L2_FRMIVAL_TYPE_DISCRETE) {
		if (compare_fraction(&frmival->discrete, min) < 0 ||
		    compare_fraction(&frmival->discrete, max) > 0)
			return false;
	} else if (frmival->type == V4L2_FRMIVAL_TYPE_CONTINUOUS ||
		   frmival->type == V4L2_FRMIVAL_TYPE_STEPWISE) {
		/* FIXME, use LCM */
		frmival->stepwise.step.denominator *= step->num;
		frmival->stepwise.step.numerator *= step->denom;

		if (compare_fraction(&frmival->stepwise.max, min) < 0 ||
		    compare_fraction(&frmival->stepwise.min, max) > 0)
			return false;

		if (compare_fraction(&frmival->stepwise.min, min) < 0) {
			frmival->stepwise.min.denominator = min->num;
			frmival->stepwise.min.numerator = min->denom;
		}
		if (compare_fraction(&frmival->stepwise.max, max) > 0) {
			frmival->stepwise.max.denominator = max->num;
			frmival->stepwise.max.numerator = max->denom;
		}
	} else
		return false;

	return true;
}

#define FOURCC_ARGS(f) (f)&0x7f,((f)>>8)&0x7f,((f)>>16)&0x7f,((f)>>24)&0x7f

static int
spa_v4l2_enum_format(struct impl *this,
		     struct spa_format **format,
		     const struct spa_format *filter,
		     uint32_t index)
{
	struct port *state = &this->out_ports[0];
	int res, n_fractions;
	const struct format_info *info;
	struct spa_pod_frame f[2];
	struct spa_pod_prop *prop;
	struct spa_pod_builder b = { NULL, };
	uint32_t media_type, media_subtype, video_format;

	if (spa_v4l2_open(this) < 0)
		return SPA_RESULT_ERROR;

	*format = NULL;

	if (index == 0) {
		spa_zero(state->fmtdesc);
		state->fmtdesc.index = 0;
		state->fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		state->next_fmtdesc = true;
		spa_zero(state->frmsize);
		state->next_frmsize = true;
		spa_zero(state->frmival);
	}

	if (false) {
	      next_fmtdesc:
		state->fmtdesc.index++;
		state->next_fmtdesc = true;
	}

	while (state->next_fmtdesc) {
		if (filter) {
			video_format =
			    enum_filter_format(&this->type, filter, state->fmtdesc.index);
			if (video_format == this->type.video_format.UNKNOWN)
				return SPA_RESULT_ENUM_END;

			info = find_format_info_by_media_type(&this->type,
							      filter->body.media_type.value,
							      filter->body.media_subtype.value,
							      video_format, 0);
			if (info == NULL)
				goto next_fmtdesc;

			state->fmtdesc.pixelformat = info->fourcc;
		} else {
			if ((res = xioctl(state->fd, VIDIOC_ENUM_FMT, &state->fmtdesc)) < 0) {
				if (errno != EINVAL)
					perror("VIDIOC_ENUM_FMT");
				return SPA_RESULT_ENUM_END;
			}
		}
		state->next_fmtdesc = false;
		state->frmsize.index = 0;
		state->frmsize.pixel_format = state->fmtdesc.pixelformat;
		state->next_frmsize = true;
	}
	if (!(info = fourcc_to_format_info(state->fmtdesc.pixelformat)))
		goto next_fmtdesc;

      next_frmsize:
	while (state->next_frmsize) {
		if (filter) {
			struct spa_pod_prop *p;

			/* check if we have a fixed frame size */
			if (!(p = spa_format_find_prop(filter, this->type.format_video.size)))
				goto do_frmsize;

			if (p->body.value.type != SPA_POD_TYPE_RECTANGLE)
				return SPA_RESULT_ENUM_END;

			if (!(p->body.flags & SPA_POD_PROP_FLAG_UNSET)) {
				const struct spa_rectangle *values =
				    SPA_POD_BODY_CONST(&p->body.value);

				if (state->frmsize.index > 0)
					goto next_fmtdesc;

				state->frmsize.type = V4L2_FRMSIZE_TYPE_DISCRETE;
				state->frmsize.discrete.width = values[0].width;
				state->frmsize.discrete.height = values[0].height;
				goto have_size;
			}
		}
	      do_frmsize:
		if ((res = xioctl(state->fd, VIDIOC_ENUM_FRAMESIZES, &state->frmsize)) < 0) {
			if (errno == EINVAL)
				goto next_fmtdesc;

			perror("VIDIOC_ENUM_FRAMESIZES");
			return SPA_RESULT_ENUM_END;
		}
		if (filter) {
			struct spa_pod_prop *p;
			const struct spa_rectangle step = { 1, 1 }, *values;
			uint32_t range;
			uint32_t i, n_values;

			/* check if we have a fixed frame size */
			if (!(p = spa_format_find_prop(filter, this->type.format_video.size)))
				goto have_size;

			range = p->body.flags & SPA_POD_PROP_RANGE_MASK;
			values = SPA_POD_BODY_CONST(&p->body.value);
			n_values = SPA_POD_PROP_N_VALUES(p);

			if (range == SPA_POD_PROP_RANGE_MIN_MAX && n_values > 2) {
				if (filter_framesize(&state->frmsize, &values[1], &values[2], &step))
					goto have_size;
			} else if (range == SPA_POD_PROP_RANGE_STEP && n_values > 3) {
				if (filter_framesize(&state->frmsize, &values[1], &values[2], &values[3]))
					goto have_size;
			} else if (range == SPA_POD_PROP_RANGE_ENUM) {
				for (i = 1; i < n_values; i++) {
					if (filter_framesize(&state->frmsize, &values[i], &values[i], &step))
						goto have_size;
				}
			}
			/* nothing matches the filter, get next frame size */
			state->frmsize.index++;
			continue;
		}

	      have_size:
		if (state->frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
			/* we have a fixed size, use this to get the frame intervals */
			state->frmival.index = 0;
			state->frmival.pixel_format = state->frmsize.pixel_format;
			state->frmival.width = state->frmsize.discrete.width;
			state->frmival.height = state->frmsize.discrete.height;
			state->next_frmsize = false;
		} else if (state->frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS ||
			   state->frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
			/* we have a non fixed size, fix to something sensible to get the
			 * framerate */
			state->frmival.index = 0;
			state->frmival.pixel_format = state->frmsize.pixel_format;
			state->frmival.width = state->frmsize.stepwise.min_width;
			state->frmival.height = state->frmsize.stepwise.min_height;
			state->next_frmsize = false;
		} else {
			state->frmsize.index++;
		}
	}

	media_type = *SPA_MEMBER(&this->type, info->media_type_offset, uint32_t);
	media_subtype = *SPA_MEMBER(&this->type, info->media_subtype_offset, uint32_t);
	video_format = *SPA_MEMBER(&this->type, info->format_offset, uint32_t);

	spa_pod_builder_init(&b, state->format_buffer, sizeof(state->format_buffer));
	spa_pod_builder_push_format(&b, &f[0], this->type.format, media_type, media_subtype);

	if (media_subtype == this->type.media_subtype.raw) {
		spa_pod_builder_add(&b,
		":", this->type.format_video.format, "I", video_format, 0);
	}
	spa_pod_builder_add(&b,
		":", this->type.format_video.size, "R", &SPA_RECTANGLE(state->frmsize.discrete.width,
								       state->frmsize.discrete.height), 0);

	spa_pod_builder_push_prop(&b, &f[1], this->type.format_video.framerate,
				  SPA_POD_PROP_RANGE_NONE | SPA_POD_PROP_FLAG_UNSET);

	prop = SPA_POD_BUILDER_DEREF(&b, f[1].ref, struct spa_pod_prop);
	n_fractions = 0;

	state->frmival.index = 0;

	while (true) {
		if ((res = xioctl(state->fd, VIDIOC_ENUM_FRAMEINTERVALS, &state->frmival)) < 0) {
			if (errno == EINVAL) {
				state->frmsize.index++;
				state->next_frmsize = true;
				if (state->frmival.index == 0)
					goto next_frmsize;
				break;
			}
			perror("VIDIOC_ENUM_FRAMEINTERVALS");
			return SPA_RESULT_ENUM_END;
		}
		if (filter) {
			struct spa_pod_prop *p;
			uint32_t range;
			uint32_t i, n_values;
			const struct spa_fraction step = { 1, 1 }, *values;

			if (!(p = spa_format_find_prop(filter, this->type.format_video.framerate)))
				goto have_framerate;

			if (p->body.value.type != SPA_POD_TYPE_FRACTION)
				return SPA_RESULT_ENUM_END;

			range = p->body.flags & SPA_POD_PROP_RANGE_MASK;
			values = SPA_POD_BODY_CONST(&p->body.value);
			n_values = SPA_POD_PROP_N_VALUES(p);

			if (!(p->body.flags & SPA_POD_PROP_FLAG_UNSET)) {
				if (filter_framerate(&state->frmival, &values[0], &values[0], &step))
					goto have_framerate;
			} else if (range == SPA_POD_PROP_RANGE_MIN_MAX && n_values > 2) {
				if (filter_framerate(&state->frmival, &values[1], &values[2], &step))
					goto have_framerate;
			} else if (range == SPA_POD_PROP_RANGE_STEP && n_values > 3) {
				if (filter_framerate(&state->frmival, &values[1], &values[2], &values[3]))
					goto have_framerate;
			} else if (range == SPA_POD_PROP_RANGE_ENUM) {
				for (i = 1; i < n_values; i++) {
					if (filter_framerate(&state->frmival, &values[i], &values[i], &step))
						goto have_framerate;
				}
			}
			state->frmival.index++;
			continue;
		}

	      have_framerate:

		if (state->frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
			prop->body.flags |= SPA_POD_PROP_RANGE_ENUM;
			if (n_fractions == 0)
				spa_pod_builder_fraction(&b,
							 state->frmival.discrete.denominator,
							 state->frmival.discrete.numerator);
			spa_pod_builder_fraction(&b,
						 state->frmival.discrete.denominator,
						 state->frmival.discrete.numerator);
			state->frmival.index++;
		} else if (state->frmival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS ||
			   state->frmival.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
			if (n_fractions == 0)
				spa_pod_builder_fraction(&b, 25, 1);
			spa_pod_builder_fraction(&b,
						 state->frmival.stepwise.min.denominator,
						 state->frmival.stepwise.min.numerator);
			spa_pod_builder_fraction(&b,
						 state->frmival.stepwise.max.denominator,
						 state->frmival.stepwise.max.numerator);

			if (state->frmival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
				prop->body.flags |= SPA_POD_PROP_RANGE_MIN_MAX;
			} else {
				prop->body.flags |= SPA_POD_PROP_RANGE_STEP;
				spa_pod_builder_fraction(&b,
							 state->frmival.stepwise.step.denominator,
							 state->frmival.stepwise.step.numerator);
			}
			break;
		}
		n_fractions++;
	}
	if (n_fractions <= 1) {
		prop->body.flags &= ~(SPA_POD_PROP_RANGE_MASK | SPA_POD_PROP_FLAG_UNSET);
	}
	spa_pod_builder_pop(&b, &f[1]);
	spa_pod_builder_pop(&b, &f[0]);

	*format = SPA_POD_BUILDER_DEREF(&b, f[0].ref, struct spa_format);

	return SPA_RESULT_OK;
}

static int spa_v4l2_set_format(struct impl *this, struct spa_video_info *format, bool try_only)
{
	struct port *state = &this->out_ports[0];
	int cmd;
	struct v4l2_format reqfmt, fmt;
	struct v4l2_streamparm streamparm;
	const struct format_info *info = NULL;
	uint32_t video_format;
	struct spa_rectangle *size = NULL;
	struct spa_fraction *framerate = NULL;

	spa_zero(fmt);
	spa_zero(streamparm);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (format->media_subtype == this->type.media_subtype.raw) {
		video_format = format->info.raw.format;
		size = &format->info.raw.size;
		framerate = &format->info.raw.framerate;
	} else if (format->media_subtype == this->type.media_subtype_video.mjpg ||
		   format->media_subtype == this->type.media_subtype_video.jpeg) {
		video_format = this->type.video_format.ENCODED;
		size = &format->info.mjpg.size;
		framerate = &format->info.mjpg.framerate;
	} else if (format->media_subtype == this->type.media_subtype_video.h264) {
		video_format = this->type.video_format.ENCODED;
		size = &format->info.h264.size;
		framerate = &format->info.h264.framerate;
	} else {
		video_format = this->type.video_format.ENCODED;
	}

	info = find_format_info_by_media_type(&this->type,
					      format->media_type,
					      format->media_subtype, video_format, 0);
	if (info == NULL || size == NULL || framerate == NULL) {
		spa_log_error(state->log, "v4l2: unknown media type %d %d %d", format->media_type,
			      format->media_subtype, video_format);
		return -1;
	}


	fmt.fmt.pix.pixelformat = info->fourcc;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	fmt.fmt.pix.width = size->width;
	fmt.fmt.pix.height = size->height;
	streamparm.parm.capture.timeperframe.numerator = framerate->denom;
	streamparm.parm.capture.timeperframe.denominator = framerate->num;

	spa_log_info(state->log, "v4l2: set %08x %dx%d %d/%d", fmt.fmt.pix.pixelformat,
		     fmt.fmt.pix.width, fmt.fmt.pix.height,
		     streamparm.parm.capture.timeperframe.denominator,
		     streamparm.parm.capture.timeperframe.numerator);

	reqfmt = fmt;

	if (spa_v4l2_open(this) < 0)
		return -1;

	cmd = try_only ? VIDIOC_TRY_FMT : VIDIOC_S_FMT;
	if (xioctl(state->fd, cmd, &fmt) < 0) {
		perror("VIDIOC_S_FMT");
		return -1;
	}

	/* some cheap USB cam's won't accept any change */
	if (xioctl(state->fd, VIDIOC_S_PARM, &streamparm) < 0)
		perror("VIDIOC_S_PARM");

	spa_log_info(state->log, "v4l2: got %08x %dx%d %d/%d", fmt.fmt.pix.pixelformat,
		     fmt.fmt.pix.width, fmt.fmt.pix.height,
		     streamparm.parm.capture.timeperframe.denominator,
		     streamparm.parm.capture.timeperframe.numerator);

	if (reqfmt.fmt.pix.pixelformat != fmt.fmt.pix.pixelformat ||
	    reqfmt.fmt.pix.width != fmt.fmt.pix.width ||
	    reqfmt.fmt.pix.height != fmt.fmt.pix.height)
		return -1;

	if (try_only)
		return 0;

	size->width = fmt.fmt.pix.width;
	size->height = fmt.fmt.pix.height;
	framerate->num = streamparm.parm.capture.timeperframe.denominator;
	framerate->denom = streamparm.parm.capture.timeperframe.numerator;

	state->fmt = fmt;
	state->info.flags = (state->export_buf ? SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS : 0) |
	    SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS | SPA_PORT_INFO_FLAG_LIVE;
	state->info.rate = streamparm.parm.capture.timeperframe.denominator;

	return 0;
}

static int mmap_read(struct impl *this)
{
	struct port *state = &this->out_ports[0];
	struct v4l2_buffer buf;
	struct buffer *b;
	struct spa_data *d;
	int64_t pts;
	struct spa_port_io *io = state->io;

	spa_zero(buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = state->memtype;

	if (xioctl(state->fd, VIDIOC_DQBUF, &buf) < 0) {
		switch (errno) {
		case EAGAIN:
			return SPA_RESULT_ERROR;
		case EIO:
		default:
			perror("VIDIOC_DQBUF");
			return SPA_RESULT_ERROR;
		}
	}

	state->last_ticks = (int64_t) buf.timestamp.tv_sec * SPA_USEC_PER_SEC +
			    (uint64_t) buf.timestamp.tv_usec;
	pts = state->last_ticks * 1000;

	if (buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
		state->last_monotonic = pts;
	else
		state->last_monotonic = SPA_TIME_INVALID;

	b = &state->buffers[buf.index];
	if (b->h) {
		b->h->flags = 0;
		if (buf.flags & V4L2_BUF_FLAG_ERROR)
			b->h->flags |= SPA_META_HEADER_FLAG_CORRUPTED;
		b->h->seq = buf.sequence;
		b->h->pts = pts;
	}

	d = b->outbuf->datas;
	d[0].chunk->offset = 0;
	d[0].chunk->size = buf.bytesused;
	d[0].chunk->stride = state->fmt.fmt.pix.bytesperline;

	if (io->buffer_id != SPA_ID_INVALID)
		spa_v4l2_buffer_recycle(this, io->buffer_id);

	b->outstanding = true;
	io->buffer_id = b->outbuf->id;
	io->status = SPA_RESULT_HAVE_BUFFER;
	this->callbacks->have_output(this->callbacks_data);

	return SPA_RESULT_OK;
}

static void v4l2_on_fd_events(struct spa_source *source)
{
	struct impl *this = source->data;

	if (source->rmask & SPA_IO_ERR)
		return;

	if (!(source->rmask & SPA_IO_IN))
		return;

	if (mmap_read(this) < 0)
		return;
}

static int spa_v4l2_use_buffers(struct impl *this, struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct port *state = &this->out_ports[0];
	struct v4l2_requestbuffers reqbuf;
	int i;
	struct spa_data *d;

	if (n_buffers > 0) {
		d = buffers[0]->datas;

		if ((d[0].type == this->type.data.MemPtr ||
		     d[0].type == this->type.data.MemFd) && d[0].data != NULL) {
			state->memtype = V4L2_MEMORY_USERPTR;
		} else if (d[0].type == this->type.data.DmaBuf) {
			state->memtype = V4L2_MEMORY_DMABUF;
		} else {
			spa_log_error(state->log, "v4l2: can't use buffers of type %s (%d)",
					spa_type_map_get_type (this->map, d[0].type), d[0].type);
			return SPA_RESULT_ERROR;
		}
	}

	spa_zero(reqbuf);
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqbuf.memory = state->memtype;
	reqbuf.count = n_buffers;

	if (xioctl(state->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		perror("VIDIOC_REQBUFS");
		return SPA_RESULT_ERROR;
	}
	spa_log_info(state->log, "v4l2: got %d buffers", reqbuf.count);
	if (reqbuf.count < n_buffers) {
		spa_log_error(state->log, "v4l2: can't allocate enough buffers");
		return SPA_RESULT_ERROR;
	}

	for (i = 0; i < reqbuf.count; i++) {
		struct buffer *b;

		b = &state->buffers[i];
		b->outbuf = buffers[i];
		b->outstanding = true;
		b->allocated = false;
		b->h = spa_buffer_find_meta(b->outbuf, this->type.meta.Header);

		spa_log_info(state->log, "v4l2: import buffer %p", buffers[i]);

		if (buffers[i]->n_datas < 1) {
			spa_log_error(state->log, "v4l2: invalid memory on buffer %p", buffers[i]);
			return SPA_RESULT_ERROR;
		}
		d = buffers[i]->datas;

		spa_zero(b->v4l2_buffer);
		b->v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b->v4l2_buffer.memory = state->memtype;
		b->v4l2_buffer.index = i;

		if (d[0].type == this->type.data.MemPtr || d[0].type == this->type.data.MemFd) {
			b->v4l2_buffer.m.userptr = (unsigned long) d[0].data;
			b->v4l2_buffer.length = d[0].maxsize;
		} else if (d[0].type == this->type.data.DmaBuf) {
			b->v4l2_buffer.m.fd = d[0].fd;
		}
		spa_v4l2_buffer_recycle(this, buffers[i]->id);
	}
	state->n_buffers = reqbuf.count;

	return SPA_RESULT_OK;
}

static int
mmap_init(struct impl *this,
	  struct spa_param **params,
	  uint32_t n_params,
	  struct spa_buffer **buffers,
	  uint32_t *n_buffers)
{
	struct port *state = &this->out_ports[0];
	struct v4l2_requestbuffers reqbuf;
	int i;

	state->memtype = V4L2_MEMORY_MMAP;

	spa_zero(reqbuf);
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqbuf.memory = state->memtype;
	reqbuf.count = *n_buffers;

	if (xioctl(state->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		perror("VIDIOC_REQBUFS");
		return SPA_RESULT_ERROR;
	}

	spa_log_info(state->log, "v4l2: got %d buffers", reqbuf.count);
	*n_buffers = reqbuf.count;

	if (reqbuf.count < 2) {
		spa_log_error(state->log, "v4l2: can't allocate enough buffers");
		return SPA_RESULT_ERROR;
	}
	if (state->export_buf)
		spa_log_info(state->log, "v4l2: using EXPBUF");

	for (i = 0; i < reqbuf.count; i++) {
		struct buffer *b;
		struct spa_data *d;

		if (buffers[i]->n_datas < 1) {
			spa_log_error(state->log, "v4l2: invalid buffer data");
			return SPA_RESULT_ERROR;
		}

		b = &state->buffers[i];
		b->outbuf = buffers[i];
		b->outstanding = true;
		b->allocated = true;
		b->h = spa_buffer_find_meta(b->outbuf, this->type.meta.Header);

		spa_zero(b->v4l2_buffer);
		b->v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b->v4l2_buffer.memory = state->memtype;
		b->v4l2_buffer.index = i;

		if (xioctl(state->fd, VIDIOC_QUERYBUF, &b->v4l2_buffer) < 0) {
			perror("VIDIOC_QUERYBUF");
			return SPA_RESULT_ERROR;
		}

		d = buffers[i]->datas;
		d[0].mapoffset = 0;
		d[0].maxsize = b->v4l2_buffer.length;
		d[0].chunk->offset = 0;
		d[0].chunk->size = b->v4l2_buffer.length;
		d[0].chunk->stride = state->fmt.fmt.pix.bytesperline;

		if (state->export_buf) {
			struct v4l2_exportbuffer expbuf;

			spa_zero(expbuf);
			expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			expbuf.index = i;
			expbuf.flags = O_CLOEXEC | O_RDONLY;
			if (xioctl(state->fd, VIDIOC_EXPBUF, &expbuf) < 0) {
				perror("VIDIOC_EXPBUF");
				continue;
			}
			d[0].type = this->type.data.DmaBuf;
			d[0].fd = expbuf.fd;
			d[0].data = NULL;
		} else {
			d[0].type = this->type.data.MemPtr;
			d[0].fd = -1;
			d[0].data = mmap(NULL,
					 b->v4l2_buffer.length,
					 PROT_READ, MAP_SHARED,
					 state->fd,
					 b->v4l2_buffer.m.offset);
			if (d[0].data == MAP_FAILED) {
				perror("mmap");
				continue;
			}
		}
		spa_v4l2_buffer_recycle(this, i);
	}
	state->n_buffers = reqbuf.count;

	return SPA_RESULT_OK;
}

static int userptr_init(struct impl *this)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int read_init(struct impl *this)
{
	return SPA_RESULT_NOT_IMPLEMENTED;
}

static int
spa_v4l2_alloc_buffers(struct impl *this,
		       struct spa_param **params,
		       uint32_t n_params,
		       struct spa_buffer **buffers,
		       uint32_t *n_buffers)
{
	int res;
	struct port *state = &this->out_ports[0];

	if (state->n_buffers > 0)
		return SPA_RESULT_ERROR;

	if (state->cap.capabilities & V4L2_CAP_STREAMING) {
		if ((res = mmap_init(this, params, n_params, buffers, n_buffers)) < 0)
			if ((res = userptr_init(this)) < 0)
				return res;
	} else if (state->cap.capabilities & V4L2_CAP_READWRITE) {
		if ((res = read_init(this)) < 0)
			return res;
	} else
		return SPA_RESULT_ERROR;

	return SPA_RESULT_OK;
}

static int spa_v4l2_stream_on(struct impl *this)
{
	struct port *state = &this->out_ports[0];
	enum v4l2_buf_type type;

	if (state->started)
		return SPA_RESULT_OK;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(state->fd, VIDIOC_STREAMON, &type) < 0) {
		spa_log_error(this->log, "VIDIOC_STREAMON: %s", strerror(errno));
		return SPA_RESULT_ERROR;
	}
	state->started = true;

	return SPA_RESULT_OK;
}

static int spa_v4l2_stream_off(struct impl *this)
{
	struct port *state = &this->out_ports[0];
	enum v4l2_buf_type type;
	int i;

	if (!state->started)
		return SPA_RESULT_OK;

	spa_v4l2_port_set_enabled(this, false);

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(state->fd, VIDIOC_STREAMOFF, &type) < 0) {
		spa_log_error(this->log, "VIDIOC_STREAMOFF: %s", strerror(errno));
		return SPA_RESULT_ERROR;
	}
	for (i = 0; i < state->n_buffers; i++) {
		struct buffer *b;

		b = &state->buffers[i];
		if (!b->outstanding)
			if (xioctl(state->fd, VIDIOC_QBUF, &b->v4l2_buffer) < 0)
				spa_log_warn(this->log, "VIDIOC_QBUF: %s", strerror(errno));
	}
	state->started = false;

	return SPA_RESULT_OK;
}
