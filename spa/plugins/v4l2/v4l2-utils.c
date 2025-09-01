/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>

#include <spa/pod/dynamic.h>
#include <spa/utils/cleanup.h>
#include <spa/utils/result.h>

static int xioctl(int fd, int request, void *arg)
{
	int err;

	do {
		err = ioctl(fd, request, arg);
	} while (err == -1 && errno == EINTR);

	return err;
}

int spa_v4l2_open(struct spa_v4l2_device *dev, const char *path)
{
	struct stat st;
	int err;

	if (dev->fd != -1)
		return 0;

	if (path == NULL) {
		spa_log_error(dev->log, "Device property not set");
		return -EIO;
	}

	spa_log_info(dev->log, "device is '%s'", path);

	dev->fd = open(path, O_RDWR | O_NONBLOCK, 0);
	if (dev->fd == -1) {
		err = errno;
		spa_log_error(dev->log, "Cannot open '%s': %d, %s",
			      path, err, strerror(err));
		goto error;
	}

	if (fstat(dev->fd, &st) < 0) {
		err = errno;
		spa_log_error(dev->log, "Cannot identify '%s': %d, %s",
				path, err, strerror(err));
		goto error_close;
	}

	if (!S_ISCHR(st.st_mode)) {
		spa_log_error(dev->log, "%s is no device", path);
		err = ENODEV;
		goto error_close;
	}

	if (xioctl(dev->fd, VIDIOC_QUERYCAP, &dev->cap) < 0) {
		err = errno;
		spa_log_error(dev->log, "'%s' QUERYCAP: %m", path);
		goto error_close;
	}
	snprintf(dev->path, sizeof(dev->path), "%s", path);
	return 0;

error_close:
	close(dev->fd);
	dev->fd = -1;
error:
	return -err;
}

int spa_v4l2_is_capture(struct spa_v4l2_device *dev)
{
	uint32_t caps = dev->cap.capabilities;
	if ((caps & V4L2_CAP_DEVICE_CAPS))
		caps = dev->cap.device_caps;
	return (caps & V4L2_CAP_VIDEO_CAPTURE) == V4L2_CAP_VIDEO_CAPTURE;
}

int spa_v4l2_close(struct spa_v4l2_device *dev)
{
	if (dev->fd == -1)
		return 0;

	if (dev->active || dev->have_format)
		return 0;

	spa_log_info(dev->log, "close '%s'", dev->path);

	if (close(dev->fd))
		spa_log_warn(dev->log, "close: %m");

	dev->fd = -1;
	return 0;
}

static int spa_v4l2_buffer_recycle(struct impl *this, uint32_t buffer_id)
{
	struct port *port = &this->out_ports[0];
	struct buffer *b = &port->buffers[buffer_id];
	struct spa_v4l2_device *dev = &port->dev;
	int err;

	if (!SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_OUTSTANDING))
		return 0;

	SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_OUTSTANDING);
	spa_log_trace(this->log, "v4l2 %p: recycle buffer %d", this, buffer_id);

	if (xioctl(dev->fd, VIDIOC_QBUF, &b->v4l2_buffer) < 0) {
		err = errno;
		spa_log_error(this->log, "'%s' VIDIOC_QBUF: %m", this->props.device);
		return -err;
	}
	return 0;
}

static int spa_v4l2_clear_buffers(struct impl *this)
{
	struct port *port = &this->out_ports[0];
	struct v4l2_requestbuffers reqbuf;
	uint32_t i;

	if (port->n_buffers == 0)
		return 0;

	for (i = 0; i < port->n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d;

		b = &port->buffers[i];
		d = b->outbuf->datas;

		if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_OUTSTANDING)) {
			spa_log_debug(this->log, "queueing outstanding buffer %p", b);
			spa_v4l2_buffer_recycle(this, i);
		}
		if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_MAPPED)) {
			munmap(b->ptr, d[0].maxsize);
		}
		if (b->mmap_ptr)
			munmap(b->mmap_ptr, b->v4l2_buffer.length);
		if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_ALLOCATED)) {
			spa_log_debug(this->log, "close %d", (int) d[0].fd);
			close(d[0].fd);
		}
		d[0].type = SPA_ID_INVALID;
	}

	spa_zero(reqbuf);
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqbuf.memory = port->memtype;
	reqbuf.count = 0;

	if (xioctl(port->dev.fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		spa_log_warn(this->log, "VIDIOC_REQBUFS: %m");
	}
	port->n_buffers = 0;

	return 0;
}


struct format_info {
	uint32_t fourcc;
	uint32_t format;
	uint32_t media_type;
	uint32_t media_subtype;
};

#define VIDEO   SPA_MEDIA_TYPE_video
#define IMAGE   SPA_MEDIA_TYPE_image

#define RAW     SPA_MEDIA_SUBTYPE_raw

#define BAYER   SPA_MEDIA_SUBTYPE_bayer
#define MJPG    SPA_MEDIA_SUBTYPE_mjpg
#define JPEG    SPA_MEDIA_SUBTYPE_jpeg
#define DV      SPA_MEDIA_SUBTYPE_dv
#define MPEGTS  SPA_MEDIA_SUBTYPE_mpegts
#define H264    SPA_MEDIA_SUBTYPE_h264
#define H263    SPA_MEDIA_SUBTYPE_h263
#define MPEG1   SPA_MEDIA_SUBTYPE_mpeg1
#define MPEG2   SPA_MEDIA_SUBTYPE_mpeg2
#define MPEG4   SPA_MEDIA_SUBTYPE_mpeg4
#define XVID    SPA_MEDIA_SUBTYPE_xvid
#define VC1     SPA_MEDIA_SUBTYPE_vc1
#define VP8     SPA_MEDIA_SUBTYPE_vp8

#define FORMAT_UNKNOWN    SPA_VIDEO_FORMAT_UNKNOWN
#define FORMAT_ENCODED    SPA_VIDEO_FORMAT_ENCODED
#define FORMAT_RGB15      SPA_VIDEO_FORMAT_RGB15
#define FORMAT_BGR15      SPA_VIDEO_FORMAT_BGR15
#define FORMAT_RGB16      SPA_VIDEO_FORMAT_RGB16
#define FORMAT_BGR        SPA_VIDEO_FORMAT_BGR
#define FORMAT_RGB        SPA_VIDEO_FORMAT_RGB
#define FORMAT_BGRA       SPA_VIDEO_FORMAT_BGRA
#define FORMAT_BGRx       SPA_VIDEO_FORMAT_BGRx
#define FORMAT_ARGB       SPA_VIDEO_FORMAT_ARGB
#define FORMAT_xRGB       SPA_VIDEO_FORMAT_xRGB
#define FORMAT_GRAY8      SPA_VIDEO_FORMAT_GRAY8
#define FORMAT_GRAY16_LE  SPA_VIDEO_FORMAT_GRAY16_LE
#define FORMAT_GRAY16_BE  SPA_VIDEO_FORMAT_GRAY16_BE
#define FORMAT_YVU9       SPA_VIDEO_FORMAT_YVU9
#define FORMAT_YV12       SPA_VIDEO_FORMAT_YV12
#define FORMAT_YUY2       SPA_VIDEO_FORMAT_YUY2
#define FORMAT_YVYU       SPA_VIDEO_FORMAT_YVYU
#define FORMAT_UYVY       SPA_VIDEO_FORMAT_UYVY
#define FORMAT_Y42B       SPA_VIDEO_FORMAT_Y42B
#define FORMAT_Y41B       SPA_VIDEO_FORMAT_Y41B
#define FORMAT_YUV9       SPA_VIDEO_FORMAT_YUV9
#define FORMAT_I420       SPA_VIDEO_FORMAT_I420
#define FORMAT_NV12       SPA_VIDEO_FORMAT_NV12
#define FORMAT_NV12_64Z32 SPA_VIDEO_FORMAT_NV12_64Z32
#define FORMAT_NV21       SPA_VIDEO_FORMAT_NV21
#define FORMAT_NV16       SPA_VIDEO_FORMAT_NV16
#define FORMAT_NV61       SPA_VIDEO_FORMAT_NV61
#define FORMAT_NV24       SPA_VIDEO_FORMAT_NV24

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
	{V4L2_PIX_FMT_JPEG, FORMAT_ENCODED, VIDEO, MJPG},
	{V4L2_PIX_FMT_PJPG, FORMAT_ENCODED, VIDEO, MJPG},
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
	SPA_FOR_EACH_ELEMENT_VAR(format_info, i) {
		if (i->fourcc == fourcc)
			return i;
	}
	return NULL;
}

#if 0
static const struct format_info *video_format_to_format_info(uint32_t format)
{
	SPA_FOR_EACH_ELEMENT_VAR(format_info, i) {
		if (i->format == format)
			return i;
	}
	return NULL;
}
#endif

static const struct format_info *find_format_info_by_media_type(uint32_t type,
								uint32_t subtype,
								uint32_t format,
								int startidx)
{
	size_t i;

	for (i = startidx; i < SPA_N_ELEMENTS(format_info); i++) {
		const struct format_info *fi = &format_info[i];
		if ((fi->media_type == type) &&
		    (fi->media_subtype == subtype) &&
		    (format == 0 || fi->format == format))
			return fi;
	}
	return NULL;
}

static int
enum_filter_format(uint32_t media_type, int32_t media_subtype,
		   const struct spa_pod *filter, uint32_t index)
{
	uint32_t video_format = SPA_VIDEO_FORMAT_UNKNOWN;

	switch (media_type) {
	case SPA_MEDIA_TYPE_video:
	case SPA_MEDIA_TYPE_image:
		if (media_subtype == SPA_MEDIA_SUBTYPE_raw) {
			const struct spa_pod_prop *p;
			const struct spa_pod *val;
			uint32_t n_values, choice;
			const uint32_t *values;

			if (!(p = spa_pod_find_prop(filter, NULL, SPA_FORMAT_VIDEO_format)))
				return -ENOENT;

			val = spa_pod_get_values(&p->value, &n_values, &choice);
			if (val->type != SPA_TYPE_Id || n_values == 0)
				return SPA_VIDEO_FORMAT_UNKNOWN;

			values = SPA_POD_BODY(val);

			if (choice == SPA_CHOICE_None) {
				if (index == 0)
					video_format = values[0];
			} else {
				if (index < n_values - 1)
					video_format = values[index + 1];
			}
		} else {
			if (index == 0)
				video_format = SPA_VIDEO_FORMAT_ENCODED;
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

		if (compare_fraction(&frmival->stepwise.min, min) < 0 ||
		    compare_fraction(&frmival->stepwise.max, max) > 0)
			return false;

		if (compare_fraction(&frmival->stepwise.max, min) < 0) {
			frmival->stepwise.max.denominator = min->num;
			frmival->stepwise.max.numerator = min->denom;
		}
		if (compare_fraction(&frmival->stepwise.min, max) > 0) {
			frmival->stepwise.min.denominator = max->num;
			frmival->stepwise.min.numerator = max->denom;
		}
	} else
		return false;

	return true;
}

struct spa_video_colorimetry v4l2_colorimetry_map[] = {
	{ /* V4L2_COLORSPACE_DEFAULT */
		.range = SPA_VIDEO_COLOR_RANGE_UNKNOWN,
	},
	{ /* V4L2_COLORSPACE_SMPTE170M */
		.range = SPA_VIDEO_COLOR_RANGE_16_235,
		.matrix = SPA_VIDEO_COLOR_MATRIX_BT601,
		.transfer = SPA_VIDEO_TRANSFER_BT601,
		.primaries = SPA_VIDEO_COLOR_PRIMARIES_SMPTE170M,
	},
	{ /* V4L2_COLORSPACE_SMPTE240M */
		.range = SPA_VIDEO_COLOR_RANGE_16_235,
		.matrix = SPA_VIDEO_COLOR_MATRIX_SMPTE240M,
		.transfer = SPA_VIDEO_TRANSFER_SMPTE240M,
		.primaries = SPA_VIDEO_COLOR_PRIMARIES_SMPTE240M,
	},
	{ /* V4L2_COLORSPACE_REC709 */
		.range = SPA_VIDEO_COLOR_RANGE_16_235,
		.matrix = SPA_VIDEO_COLOR_MATRIX_BT709,
		.transfer = SPA_VIDEO_TRANSFER_BT709,
		.primaries = SPA_VIDEO_COLOR_PRIMARIES_BT709,
	},
	{ /* V4L2_COLORSPACE_BT878 (deprecated) */
		.range = SPA_VIDEO_COLOR_RANGE_UNKNOWN,
	},
	{ /* V4L2_COLORSPACE_470_SYSTEM_M */
		.range = SPA_VIDEO_COLOR_RANGE_16_235,
		.matrix = SPA_VIDEO_COLOR_MATRIX_BT601,
		.transfer = SPA_VIDEO_TRANSFER_BT709,
		.primaries = SPA_VIDEO_COLOR_PRIMARIES_BT470M,
	},
	{ /* V4L2_COLORSPACE_470_SYSTEM_BG */
		.range = SPA_VIDEO_COLOR_RANGE_16_235,
		.matrix = SPA_VIDEO_COLOR_MATRIX_BT601,
		.transfer = SPA_VIDEO_TRANSFER_BT709,
		.primaries = SPA_VIDEO_COLOR_PRIMARIES_BT470BG,
	},
	{ /* V4L2_COLORSPACE_JPEG */
		.range = SPA_VIDEO_COLOR_RANGE_0_255,
		.matrix = SPA_VIDEO_COLOR_MATRIX_BT601,
		.transfer = SPA_VIDEO_TRANSFER_SRGB,
		.primaries = SPA_VIDEO_COLOR_PRIMARIES_BT709,
	},
	{ /* V4L2_COLORSPACE_SRGB */
		.range = SPA_VIDEO_COLOR_RANGE_16_235,
		.matrix = SPA_VIDEO_COLOR_MATRIX_BT601,
		.transfer = SPA_VIDEO_TRANSFER_SRGB,
		.primaries = SPA_VIDEO_COLOR_PRIMARIES_BT709,
	},
	{ /* V4L2_COLORSPACE_OPRGB */
		.range = SPA_VIDEO_COLOR_RANGE_16_235,
		.matrix = SPA_VIDEO_COLOR_MATRIX_BT601,
		.transfer = SPA_VIDEO_TRANSFER_ADOBERGB,
		.primaries = SPA_VIDEO_COLOR_PRIMARIES_ADOBERGB,
	},
	{ /* V4L2_COLORSPACE_BT2020 */
		.range = SPA_VIDEO_COLOR_RANGE_16_235,
		.matrix = SPA_VIDEO_COLOR_MATRIX_BT2020,
		.transfer = SPA_VIDEO_TRANSFER_BT2020_12,
		.primaries = SPA_VIDEO_COLOR_PRIMARIES_BT2020,
	},
	{ /* V4L2_COLORSPACE_RAW */
		.range = SPA_VIDEO_COLOR_RANGE_UNKNOWN,
	}
};

enum spa_video_color_range v4l2_color_range_map[] = {
	SPA_VIDEO_COLOR_RANGE_UNKNOWN,
	SPA_VIDEO_COLOR_RANGE_0_255,
	SPA_VIDEO_COLOR_RANGE_16_235
};

enum spa_video_color_matrix v4l2_color_matrix_map[] = {
	/* V4L2_YCBCR_ENC_DEFAULT */
	SPA_VIDEO_COLOR_MATRIX_UNKNOWN,
	/* V4L2_YCBCR_ENC_601 */
	SPA_VIDEO_COLOR_MATRIX_BT601,
	/* V4L2_YCBCR_ENC_709 */
	SPA_VIDEO_COLOR_MATRIX_BT709,
	/* V4L2_YCBCR_ENC_XV601 */
	SPA_VIDEO_COLOR_MATRIX_BT601,
	/* V4L2_YCBCR_ENC_XV709 */
	SPA_VIDEO_COLOR_MATRIX_BT709,
	/* V4L2_YCBCR_ENC_SYCC */
	SPA_VIDEO_COLOR_MATRIX_BT601,
	/* V4L2_YCBCR_ENC_BT2020 */
	SPA_VIDEO_COLOR_MATRIX_BT2020,
	/* V4L2_YCBCR_ENC_BT2020_CONST_LUM */
	SPA_VIDEO_COLOR_MATRIX_BT2020,
	/* V4L2_YCBCR_ENC_SMPTE240M */
	SPA_VIDEO_COLOR_MATRIX_SMPTE240M
};

enum spa_video_transfer_function v4l2_transfer_function_map[] = {
	/* V4L2_XFER_FUNC_DEFAULT */
	SPA_VIDEO_TRANSFER_UNKNOWN,
	/* V4L2_XFER_FUNC_709 */
	SPA_VIDEO_TRANSFER_BT709,
	/* V4L2_XFER_FUNC_SRGB */
	SPA_VIDEO_TRANSFER_SRGB,
	/* V4L2_XFER_FUNC_OPRGB */
	SPA_VIDEO_TRANSFER_ADOBERGB,
	/* V4L2_XFER_FUNC_SMPTE240M */
	SPA_VIDEO_TRANSFER_SMPTE240M,
	/* V4L2_XFER_FUNC_NONE */
	SPA_VIDEO_TRANSFER_GAMMA10,
	/* V4L2_XFER_FUNC_DCI_P3 */
	SPA_VIDEO_TRANSFER_UNKNOWN,
	/* V4L2_XFER_FUNC_SMPTE2084 */
	SPA_VIDEO_TRANSFER_SMPTE2084
};

static bool
parse_colorimetry(struct impl *this, const struct v4l2_pix_format *pix, bool is_rgb,
		  struct spa_video_colorimetry *colorimetry)
{
	struct spa_video_colorimetry c = { 0 };

	if (pix->colorspace < V4L2_COLORSPACE_RAW)
		c = v4l2_colorimetry_map[pix->colorspace];

	if (c.range == SPA_VIDEO_COLOR_RANGE_UNKNOWN)
		return false;

	switch (pix->quantization) {
	case V4L2_QUANTIZATION_FULL_RANGE:
	case V4L2_QUANTIZATION_LIM_RANGE:
		c.range = v4l2_color_range_map[pix->quantization];
		break;
	case V4L2_QUANTIZATION_DEFAULT:
		if (is_rgb)
			c.range = SPA_VIDEO_COLOR_RANGE_0_255;
		break;
	default:
		spa_log_warn(this->log, "Unknown enum v4l2_quantization value %d",
				pix->quantization);
		c.range = SPA_VIDEO_COLOR_RANGE_UNKNOWN;
		break;
	}

	if (pix->ycbcr_enc >= V4L2_YCBCR_ENC_SMPTE240M)
		spa_log_warn(this->log, "Unknown enum v4l2_ycbcr_encoding value %d",
				pix->ycbcr_enc);
	else if (pix->ycbcr_enc > 0)
		c.matrix = v4l2_color_matrix_map[pix->ycbcr_enc];

	if (pix->xfer_func >= V4L2_XFER_FUNC_SMPTE2084)
		spa_log_warn(this->log, "Unknown enum v4l2_xfer_func value %d",
				pix->xfer_func);
	else if (pix->xfer_func > 0)
		c.transfer = v4l2_transfer_function_map[pix->xfer_func];

	*colorimetry = c;
	return true;
}

#define FOURCC_ARGS(f) (f)&0x7f,((f)>>8)&0x7f,((f)>>16)&0x7f,((f)>>24)&0x7f

static int
spa_v4l2_enum_format(struct impl *this, int seq,
		     uint32_t start, uint32_t num,
		     const struct spa_pod *filter)
{
	struct port *port = &this->out_ports[0];
	int res, n_fractions;
	const struct format_info *info;
	struct spa_pod_choice *choice;
	uint32_t filter_media_type, filter_media_subtype;
	struct spa_v4l2_device *dev = &port->dev;
	uint8_t buffer[1024];
	spa_auto(spa_pod_dynamic_builder) b = { 0 };
	struct spa_pod_builder_state state;
	struct spa_pod_frame f[2];
	struct spa_result_node_params result;
	struct v4l2_format fmt;
	uint32_t count = 0, try_width = 0, try_height = 0;
	bool with_modifier;

	if ((res = spa_v4l2_open(dev, this->props.device)) < 0)
		return res;

	spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 8192);
	spa_pod_builder_get_state(&b.b, &state);

	result.id = SPA_PARAM_EnumFormat;
	result.next = start;

	if (result.next == 0) {
		spa_zero(port->fmtdesc);
		port->fmtdesc.index = 0;
		port->fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		port->next_fmtdesc = true;
		spa_zero(port->frmsize);
		port->next_frmsize = true;
		spa_zero(port->frmival);
	}

	if (filter) {
		if ((res = spa_format_parse(filter, &filter_media_type, &filter_media_subtype)) < 0)
			return res;
	}
	with_modifier = !filter || spa_pod_find_prop(filter, NULL, SPA_FORMAT_VIDEO_modifier);

	if (false) {
	      next_fmtdesc:
		port->fmtdesc.index++;
		port->next_fmtdesc = true;
	}

      next:
	result.index = result.next++;

	while (port->next_fmtdesc) {
		if (filter) {
			struct v4l2_format fmt;

			res = enum_filter_format(filter_media_type,
					    filter_media_subtype,
					    filter, port->fmtdesc.index);
			if (res == -ENOENT)
				goto do_enum_fmt;
			if (res < 0)
				goto exit;
			if (res == SPA_VIDEO_FORMAT_UNKNOWN)
				goto enum_end;

			info = find_format_info_by_media_type(filter_media_type,
							      filter_media_subtype,
							      res, 0);
			if (info == NULL)
				goto next_fmtdesc;

			port->fmtdesc.pixelformat = info->fourcc;

			spa_zero(fmt);
			fmt.type = port->fmtdesc.type;
			fmt.fmt.pix.pixelformat = info->fourcc;
			fmt.fmt.pix.field = V4L2_FIELD_ANY;
			fmt.fmt.pix.width = 0;
			fmt.fmt.pix.height = 0;

			if ((res = xioctl(dev->fd, VIDIOC_TRY_FMT, &fmt)) < 0) {
				spa_log_debug(this->log, "'%s' VIDIOC_TRY_FMT %08x: %m",
						this->props.device, info->fourcc);
				goto next_fmtdesc;
			}
			if (fmt.fmt.pix.pixelformat != info->fourcc) {
				spa_log_debug(this->log, "'%s' VIDIOC_TRY_FMT wanted %.4s gave %.4s",
						this->props.device, (char*)&info->fourcc,
						(char*)&fmt.fmt.pix.pixelformat);
				goto next_fmtdesc;
			}

		} else {
do_enum_fmt:
			if ((res = xioctl(dev->fd, VIDIOC_ENUM_FMT, &port->fmtdesc)) < 0) {
				if (errno == EINVAL)
					goto enum_end;

				res = -errno;
				spa_log_error(this->log, "'%s' VIDIOC_ENUM_FMT: %m",
						this->props.device);
				goto exit;
			}
		}
		port->next_fmtdesc = false;
		port->frmsize.index = 0;
		port->frmsize.pixel_format = port->fmtdesc.pixelformat;
		port->next_frmsize = true;
	}
	if (!(info = fourcc_to_format_info(port->fmtdesc.pixelformat)))
		goto next_fmtdesc;

      next_frmsize:
	while (port->next_frmsize) {
		if (filter) {
			const struct spa_pod_prop *p;
			struct spa_pod *val;
			uint32_t n_vals, choice;

			/* check if we have a fixed frame size */
			if (!(p = spa_pod_find_prop(filter, NULL, SPA_FORMAT_VIDEO_size)))
				goto do_frmsize;

			val = spa_pod_get_values(&p->value, &n_vals, &choice);
			if (val->type != SPA_TYPE_Rectangle || n_vals == 0)
				goto enum_end;

			if (choice == SPA_CHOICE_None) {
				const struct spa_rectangle *values = SPA_POD_BODY(val);

				if (port->frmsize.index > 0)
					goto next_fmtdesc;

				port->frmsize.type = V4L2_FRMSIZE_TYPE_DISCRETE;
				port->frmsize.discrete.width = values[0].width;
				port->frmsize.discrete.height = values[0].height;
				goto have_size;
			}
		}
	      do_frmsize:
		if ((res = xioctl(dev->fd, VIDIOC_ENUM_FRAMESIZES, &port->frmsize)) < 0) {
			if (errno == ENOTTY)
				goto next_fmtdesc;
			if (errno == EINVAL) {
				if (port->frmsize.index == 0) {
					port->frmsize.type = V4L2_FRMSIZE_TYPE_CONTINUOUS;
					port->frmsize.stepwise.min_width = 16;
					port->frmsize.stepwise.min_height = 16;
					port->frmsize.stepwise.max_width = 16384;
					port->frmsize.stepwise.max_height = 16384;
					port->frmsize.stepwise.step_width = 16;
					port->frmsize.stepwise.step_height = 16;
					port->fmtdesc.index++;
					port->next_fmtdesc = true;
					goto do_frmsize_filter;
				}
				else
					goto next_fmtdesc;
			}

			res = -errno;
			spa_log_error(this->log, "'%s' VIDIOC_ENUM_FRAMESIZES: %m",
					this->props.device);
			goto exit;
		}
do_frmsize_filter:
		if (filter) {
			static const struct spa_rectangle step = {1, 1};

			const struct spa_rectangle *values;
			const struct spa_pod_prop *p;
			struct spa_pod *val;
			uint32_t choice, i, n_values;

			/* check if we have a fixed frame size */
			if (!(p = spa_pod_find_prop(filter, NULL, SPA_FORMAT_VIDEO_size)))
				goto have_size;

			val = spa_pod_get_values(&p->value, &n_values, &choice);
			if (val->type != SPA_TYPE_Rectangle || n_values == 0)
				goto have_size;

			values = SPA_POD_BODY_CONST(val);

			if (choice == SPA_CHOICE_Range && n_values > 2) {
				if (filter_framesize(&port->frmsize, &values[1], &values[2], &step))
					goto have_size;
			} else if (choice == SPA_CHOICE_Step && n_values > 3) {
				if (filter_framesize(&port->frmsize, &values[1], &values[2], &values[3]))
					goto have_size;
			} else if (choice == SPA_CHOICE_Enum) {
				for (i = 1; i < n_values; i++) {
					if (filter_framesize(&port->frmsize, &values[i], &values[i], &step))
						goto have_size;
				}
			}
			/* nothing matches the filter, get next frame size */
			port->frmsize.index++;
			continue;
		}

	      have_size:
		if (port->frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
			/* we have a fixed size, use this to get the frame intervals */
			port->frmival.index = 0;
			port->frmival.pixel_format = port->frmsize.pixel_format;
			port->frmival.width = port->frmsize.discrete.width;
			port->frmival.height = port->frmsize.discrete.height;
			port->next_frmsize = false;
		} else if (port->frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS ||
			   port->frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
			/* we have a non fixed size, fix to something sensible to get the
			 * framerate */
			port->frmival.index = 0;
			port->frmival.pixel_format = port->frmsize.pixel_format;
			port->frmival.width = port->frmsize.stepwise.min_width;
			port->frmival.height = port->frmsize.stepwise.min_height;
			port->next_frmsize = false;
		} else {
			port->frmsize.index++;
		}
	}

	spa_pod_builder_reset(&b.b, &state);
	spa_pod_builder_push_object(&b.b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(&b.b,
			SPA_FORMAT_mediaType,    SPA_POD_Id(info->media_type),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(info->media_subtype),
			0);

	if (info->media_subtype == SPA_MEDIA_SUBTYPE_raw) {
		spa_pod_builder_prop(&b.b, SPA_FORMAT_VIDEO_format, 0);
		spa_pod_builder_id(&b.b, info->format);
		if (with_modifier) {
			spa_pod_builder_prop(&b.b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
			spa_pod_builder_long(&b.b, 0L);
		}
	}

	spa_pod_builder_prop(&b.b, SPA_FORMAT_VIDEO_size, 0);
	if (port->frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
		spa_pod_builder_rectangle(&b.b,
				port->frmsize.discrete.width,
				port->frmsize.discrete.height);
		try_width = port->frmsize.discrete.width;
		try_height = port->frmsize.discrete.height;
	} else if (port->frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS ||
		   port->frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
		spa_pod_builder_push_choice(&b.b, &f[1], SPA_CHOICE_None, 0);
		choice = (struct spa_pod_choice*)spa_pod_builder_frame(&b.b, &f[1]);

		spa_pod_builder_rectangle(&b.b,
				port->frmsize.stepwise.min_width,
				port->frmsize.stepwise.min_height);
		spa_pod_builder_rectangle(&b.b,
				port->frmsize.stepwise.min_width,
				port->frmsize.stepwise.min_height);
		spa_pod_builder_rectangle(&b.b,
				port->frmsize.stepwise.max_width,
				port->frmsize.stepwise.max_height);

		if (port->frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
			choice->body.type = SPA_CHOICE_Range;
		} else {
			choice->body.type = SPA_CHOICE_Step;
			spa_pod_builder_rectangle(&b.b,
					port->frmsize.stepwise.max_width,
					port->frmsize.stepwise.max_height);
		}
		spa_pod_builder_pop(&b.b, &f[1]);
		try_width = port->frmsize.stepwise.min_width;
		try_height = port->frmsize.stepwise.min_height;
	}

	spa_zero(fmt);
	fmt.type = port->fmtdesc.type;
	fmt.fmt.pix.pixelformat = info->fourcc;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	fmt.fmt.pix.width = try_width;
	fmt.fmt.pix.height = try_height;

	if ((res = xioctl(dev->fd, VIDIOC_TRY_FMT, &fmt)) < 0) {
		spa_log_debug(this->log, "'%s' VIDIOC_TRY_FMT %08x: %m",
				this->props.device, info->fourcc);
	} else {
		struct spa_video_colorimetry colorimetry;
		bool is_rgb = spa_format_video_is_rgb(info->format);

		if (parse_colorimetry(this, &fmt.fmt.pix, is_rgb, &colorimetry)) {
			spa_pod_builder_add(&b.b,
				SPA_FORMAT_VIDEO_colorRange,
				SPA_POD_Id(colorimetry.range),
				SPA_FORMAT_VIDEO_colorMatrix,
				SPA_POD_Id(colorimetry.matrix),
				SPA_FORMAT_VIDEO_transferFunction,
				SPA_POD_Id(colorimetry.transfer),
				SPA_FORMAT_VIDEO_colorPrimaries,
				SPA_POD_Id(colorimetry.primaries), 0);
		}
	}

	spa_pod_builder_prop(&b.b, SPA_FORMAT_VIDEO_framerate, 0);

	n_fractions = 0;

	spa_pod_builder_push_choice(&b.b, &f[1], SPA_CHOICE_None, 0);
	choice = (struct spa_pod_choice*)spa_pod_builder_frame(&b.b, &f[1]);
	port->frmival.index = 0;

	while (true) {
		if ((res = xioctl(dev->fd, VIDIOC_ENUM_FRAMEINTERVALS, &port->frmival)) < 0) {
			res = -errno;
			port->frmsize.index++;
			port->next_frmsize = true;
			if (errno == EINVAL || errno == ENOTTY) {
				if (port->frmival.index == 0) {
					port->frmival.type = V4L2_FRMIVAL_TYPE_CONTINUOUS;
					port->frmival.stepwise.min.denominator = 120;
					port->frmival.stepwise.min.numerator = 1;
					port->frmival.stepwise.max.denominator = 1;
					port->frmival.stepwise.max.numerator = 1;
					goto do_frminterval_filter;
				}
				else
					break;
			}
			spa_log_error(this->log, "'%s' VIDIOC_ENUM_FRAMEINTERVALS: %m",
					this->props.device);
			goto exit;
		}
do_frminterval_filter:
		if (filter) {
			static const struct spa_fraction step = {1, 1};

			const struct spa_fraction *values;
			const struct spa_pod_prop *p;
			struct spa_pod *val;
			uint32_t i, n_values, choice;

			if (!(p = spa_pod_find_prop(filter, NULL, SPA_FORMAT_VIDEO_framerate)))
				goto have_framerate;

			val = spa_pod_get_values(&p->value, &n_values, &choice);
			if (val->type != SPA_TYPE_Fraction || n_values == 0)
				goto enum_end;

			values = SPA_POD_BODY(val);

			switch (choice) {
			case SPA_CHOICE_None:
				if (filter_framerate(&port->frmival, &values[0], &values[0], &step))
					goto have_framerate;
				break;

			case SPA_CHOICE_Range:
				if (n_values > 2 && filter_framerate(&port->frmival, &values[1], &values[2], &step))
					goto have_framerate;
				break;

			case SPA_CHOICE_Step:
				if (n_values > 3 && filter_framerate(&port->frmival, &values[1], &values[2], &values[3]))
					goto have_framerate;
				break;

			case SPA_CHOICE_Enum:
				for (i = 1; i < n_values; i++) {
					if (filter_framerate(&port->frmival, &values[i], &values[i], &step))
						goto have_framerate;
				}
				break;
			default:
				break;
			}
			port->frmival.index++;
			continue;
		}

	      have_framerate:

		if (port->frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
			choice->body.type = SPA_CHOICE_Enum;
			if (n_fractions == 0)
				spa_pod_builder_fraction(&b.b,
							 port->frmival.discrete.denominator,
							 port->frmival.discrete.numerator);
			spa_pod_builder_fraction(&b.b,
						 port->frmival.discrete.denominator,
						 port->frmival.discrete.numerator);
			port->frmival.index++;
			n_fractions++;
		} else if (port->frmival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS ||
			   port->frmival.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
			if (n_fractions == 0) {
				struct spa_fraction f = { 25, 1 };
				if (compare_fraction(&port->frmival.stepwise.max, &f) > 0) {
					f.denom = port->frmival.stepwise.max.numerator;
					f.num = port->frmival.stepwise.max.denominator;
				}
				if (compare_fraction(&port->frmival.stepwise.min, &f) < 0) {
					f.denom = port->frmival.stepwise.min.numerator;
					f.num = port->frmival.stepwise.min.denominator;
				}

				spa_pod_builder_fraction(&b.b, f.num, f.denom);
			}
			spa_pod_builder_fraction(&b.b,
						 port->frmival.stepwise.max.denominator,
						 port->frmival.stepwise.max.numerator);
			spa_pod_builder_fraction(&b.b,
						 port->frmival.stepwise.min.denominator,
						 port->frmival.stepwise.min.numerator);

			if (port->frmival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
				choice->body.type = SPA_CHOICE_Range;
				n_fractions += 2;
			} else {
				choice->body.type = SPA_CHOICE_Step;
				spa_pod_builder_fraction(&b.b,
							 port->frmival.stepwise.step.denominator,
							 port->frmival.stepwise.step.numerator);
				n_fractions += 3;
			}

			port->frmsize.index++;
			port->next_frmsize = true;
			break;
		}
	}
	if (n_fractions == 0)
		goto next_frmsize;
	if (n_fractions == 1)
		choice->body.type = SPA_CHOICE_None;
	spa_pod_builder_pop(&b.b, &f[1]);

	result.param = spa_pod_builder_pop(&b.b, &f[0]);

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count == num)
		goto enum_end;

	if (with_modifier && info->media_subtype == SPA_MEDIA_SUBTYPE_raw) {
		struct spa_pod_object *op = (struct spa_pod_object *) result.param;
		const struct spa_pod_prop *p;

		spa_pod_builder_push_object(&b.b, &f[0], op->body.type, op->body.id);

		SPA_POD_OBJECT_FOREACH(op, p) {
			if (p->key != SPA_FORMAT_VIDEO_modifier)
				spa_pod_builder_raw_padded(&b.b, p, SPA_POD_PROP_SIZE(p));
		}

		result.index = result.next++;
		result.param = spa_pod_builder_pop(&b.b, &f[0]);

		spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);
	}

	if (++count != num)
		goto next;

      enum_end:
	res = 0;
      exit:
	spa_v4l2_close(dev);
	return res;
}

static int probe_expbuf(struct impl *this)
{
	struct port *port = &this->out_ports[0];
	struct spa_v4l2_device *dev = &port->dev;
	struct v4l2_requestbuffers reqbuf;
	struct v4l2_exportbuffer expbuf;

	if (port->probed_expbuf)
		return 0;
	port->probed_expbuf = true;

	spa_zero(reqbuf);
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqbuf.memory = V4L2_MEMORY_MMAP;
	reqbuf.count = port->max_buffers = MAX_BUFFERS;

	if (xioctl(dev->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		spa_log_error(this->log, "'%s' VIDIOC_REQBUFS: %m", this->props.device);
		return -errno;
	}
	port->max_buffers = reqbuf.count;

	spa_zero(expbuf);
	expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	expbuf.index = 0;
	expbuf.flags = O_CLOEXEC | O_RDONLY;
	if (xioctl(dev->fd, VIDIOC_EXPBUF, &expbuf) < 0) {
		spa_log_info(this->log, "'%s' EXPBUF not supported: %m", this->props.device);
		port->have_expbuf = false;
		port->alloc_buffers = false;
	} else {
		port->have_expbuf = true;
		port->alloc_buffers = true;
	}

	reqbuf.count = 0;
	if (xioctl(dev->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		spa_log_error(this->log, "'%s' VIDIOC_REQBUFS: %m", this->props.device);
		return -errno;
	}
	return 0;
}

static int spa_v4l2_set_format(struct impl *this, struct spa_video_info *format, uint32_t flags)
{
	struct port *port = &this->out_ports[0];
	struct spa_v4l2_device *dev = &port->dev;
	int res, cmd;
	struct v4l2_format reqfmt, fmt;
	struct v4l2_streamparm streamparm;
	const struct format_info *info = NULL;
	uint32_t video_format;
	struct spa_rectangle *size = NULL;
	struct spa_fraction *framerate = NULL;
	bool match;

	spa_zero(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	spa_zero(streamparm);
	streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	switch (format->media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		video_format = format->info.raw.format;
		size = &format->info.raw.size;
		framerate = &format->info.raw.framerate;
		break;
	case SPA_MEDIA_SUBTYPE_mjpg:
	case SPA_MEDIA_SUBTYPE_jpeg:
		video_format = SPA_VIDEO_FORMAT_ENCODED;
		size = &format->info.mjpg.size;
		framerate = &format->info.mjpg.framerate;
		break;
	case SPA_MEDIA_SUBTYPE_h264:
		video_format = SPA_VIDEO_FORMAT_ENCODED;
		size = &format->info.h264.size;
		framerate = &format->info.h264.framerate;
		break;
	default:
		video_format = SPA_VIDEO_FORMAT_ENCODED;
		break;
	}

	info = find_format_info_by_media_type(format->media_type,
					      format->media_subtype, video_format, 0);
	if (info == NULL || size == NULL || framerate == NULL) {
		spa_log_error(this->log, "%s: unknown media type %d %d %d",
				this->props.device, format->media_type,
				format->media_subtype, video_format);
		return -EINVAL;
	}

	fmt.fmt.pix.pixelformat = info->fourcc;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	fmt.fmt.pix.width = size->width;
	fmt.fmt.pix.height = size->height;
	streamparm.parm.capture.timeperframe.numerator = framerate->denom;
	streamparm.parm.capture.timeperframe.denominator = framerate->num;

	spa_log_info(this->log, "set %.4s %dx%d %d/%d", (char *)&fmt.fmt.pix.pixelformat,
		     fmt.fmt.pix.width, fmt.fmt.pix.height,
		     streamparm.parm.capture.timeperframe.denominator,
		     streamparm.parm.capture.timeperframe.numerator);

	reqfmt = fmt;

	if ((res = spa_v4l2_open(dev, this->props.device)) < 0)
		return res;

	cmd = (flags & SPA_NODE_PARAM_FLAG_TEST_ONLY) ? VIDIOC_TRY_FMT : VIDIOC_S_FMT;
	if (xioctl(dev->fd, cmd, &fmt) < 0) {
		res = -errno;
		spa_log_error(this->log, "'%s' VIDIOC_S_FMT: %m",
				this->props.device);
		return res;
	}

	/* some cheap USB cam's won't accept any change */
	if (xioctl(dev->fd, VIDIOC_S_PARM, &streamparm) < 0)
		spa_log_warn(this->log, "%s: VIDIOC_S_PARM: %m", this->props.device);

	match = (reqfmt.fmt.pix.pixelformat == fmt.fmt.pix.pixelformat &&
			reqfmt.fmt.pix.width == fmt.fmt.pix.width &&
			reqfmt.fmt.pix.height == fmt.fmt.pix.height);

	if (!match && !SPA_FLAG_IS_SET(flags, SPA_NODE_PARAM_FLAG_NEAREST)) {
		spa_log_error(this->log, "%s: wanted %.4s %dx%d, got %.4s %dx%d",
				this->props.device,
				(char *)&reqfmt.fmt.pix.pixelformat,
				reqfmt.fmt.pix.width, reqfmt.fmt.pix.height,
				(char *)&fmt.fmt.pix.pixelformat,
				fmt.fmt.pix.width, fmt.fmt.pix.height);
		return -EINVAL;
	}

	if (flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)
		return match ? 0 : 1;

	if (streamparm.parm.capture.timeperframe.denominator == 0)
		streamparm.parm.capture.timeperframe.denominator = 1;

	spa_log_info(this->log, "'%s' got %.4s %dx%d %d/%d",
			dev->path, (char *)&fmt.fmt.pix.pixelformat,
			fmt.fmt.pix.width, fmt.fmt.pix.height,
			streamparm.parm.capture.timeperframe.denominator,
			streamparm.parm.capture.timeperframe.numerator);

	dev->have_format = true;
	size->width = fmt.fmt.pix.width;
	size->height = fmt.fmt.pix.height;

	probe_expbuf(this);

	port->fmt = fmt;
	port->info.change_mask |= SPA_PORT_CHANGE_MASK_FLAGS | SPA_PORT_CHANGE_MASK_RATE;
	port->info.flags = (port->alloc_buffers ? SPA_PORT_FLAG_CAN_ALLOC_BUFFERS : 0) |
		SPA_PORT_FLAG_LIVE |
		SPA_PORT_FLAG_PHYSICAL |
		SPA_PORT_FLAG_TERMINAL;
	port->info.rate.num = streamparm.parm.capture.timeperframe.numerator;
	port->info.rate.denom = streamparm.parm.capture.timeperframe.denominator;

	return match ? 0 : 1;
}

static int query_ext_ctrl_ioctl(struct port *port, struct v4l2_query_ext_ctrl *qctrl)
{
	struct spa_v4l2_device *dev = &port->dev;
	struct v4l2_queryctrl qc;
	int res;

	if (port->have_query_ext_ctrl) {
		res = xioctl(dev->fd, VIDIOC_QUERY_EXT_CTRL, qctrl);
		if (res == 0 || errno != ENOTTY)
			return res;
		port->have_query_ext_ctrl = false;
	}
	spa_zero(qc);
	qc.id = qctrl->id;
	res = xioctl(dev->fd, VIDIOC_QUERYCTRL, &qc);
	if (res == 0) {
		qctrl->type = qc.type;
		memcpy(qctrl->name, qc.name, sizeof(qctrl->name));
		qctrl->minimum = qc.minimum;
		if (qc.type == V4L2_CTRL_TYPE_BITMASK) {
			qctrl->maximum = (__u32)qc.maximum;
			qctrl->default_value = (__u32)qc.default_value;
		} else {
			qctrl->maximum = qc.maximum;
			qctrl->default_value = qc.default_value;
		}
		qctrl->step = qc.step;
		qctrl->flags = qc.flags;
		qctrl->elems = 1;
		qctrl->nr_of_dims = 0;
		memset(qctrl->dims, 0, sizeof(qctrl->dims));
		switch (qctrl->type) {
		case V4L2_CTRL_TYPE_INTEGER64:
			qctrl->elem_size = sizeof(__s64);
			break;
		case V4L2_CTRL_TYPE_STRING:
			qctrl->elem_size = qc.maximum + 1;
			break;
		default:
			qctrl->elem_size = sizeof(__s32);
			break;
		}
		memset(qctrl->reserved, 0, sizeof(qctrl->reserved));
	}
	qctrl->id = qc.id;
	return res;
}

static struct {
	uint32_t v4l2_id;
	uint32_t spa_id;
} control_map[] = {
	{ V4L2_CID_BRIGHTNESS, SPA_PROP_brightness },
	{ V4L2_CID_CONTRAST, SPA_PROP_contrast },
	{ V4L2_CID_SATURATION, SPA_PROP_saturation },
	{ V4L2_CID_HUE, SPA_PROP_hue },
	{ V4L2_CID_GAMMA, SPA_PROP_gamma },
	{ V4L2_CID_EXPOSURE_ABSOLUTE, SPA_PROP_exposure },
	{ V4L2_CID_GAIN, SPA_PROP_gain },
	{ V4L2_CID_SHARPNESS, SPA_PROP_sharpness },
};

static uint32_t control_to_prop_id(struct impl *impl, uint32_t control_id)
{
	SPA_FOR_EACH_ELEMENT_VAR(control_map, c) {
		if (c->v4l2_id == control_id)
			return c->spa_id;
	}
	return SPA_PROP_START_CUSTOM + control_id;
}

static uint32_t prop_id_to_control(struct impl *impl, uint32_t prop_id)
{
	SPA_FOR_EACH_ELEMENT_VAR(control_map, c) {
		if (c->spa_id == prop_id)
			return c->v4l2_id;
	}
	if (prop_id >= SPA_PROP_START_CUSTOM)
		return prop_id - SPA_PROP_START_CUSTOM;
	return SPA_ID_INVALID;
}

static int
spa_v4l2_enum_controls(struct impl *this, int seq,
		       uint32_t start, uint32_t num,
		       const struct spa_pod *filter)
{
	struct port *port = &this->out_ports[0];
	struct spa_v4l2_device *dev = &port->dev;
	struct v4l2_query_ext_ctrl queryctrl;
	struct spa_pod *param;
	spa_auto(spa_pod_dynamic_builder) b = { 0 };
	struct spa_pod_builder_state state;
	uint32_t prop_id, ctrl_id;
	uint8_t buffer[1024];
	int res;
        const unsigned next_fl = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
	struct spa_pod_frame f[2];
	struct spa_result_node_params result;
	uint32_t count = 0;

	if ((res = spa_v4l2_open(dev, this->props.device)) < 0)
		return res;

	spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 4096);
	spa_pod_builder_get_state(&b.b, &state);

	result.id = SPA_PARAM_PropInfo;
	result.next = start;
      next:
	result.index = result.next;

	spa_zero(queryctrl);

	if (result.next == 0) {
		result.next |= next_fl;
		port->n_controls = 0;
	}

	queryctrl.id = result.next;
	spa_log_debug(this->log, "test control %08x", queryctrl.id);

	if (query_ext_ctrl_ioctl(port, &queryctrl) != 0) {
		if (errno == ENOTTY)
			goto enum_end;
		if (errno == EINVAL) {
			if (queryctrl.id != next_fl)
				goto enum_end;

			if (result.next & next_fl)
				result.next = V4L2_CID_USER_BASE;
			else if (result.next >= V4L2_CID_USER_BASE && result.next < V4L2_CID_LASTP1)
				result.next++;
			else if (result.next >= V4L2_CID_LASTP1)
				result.next = V4L2_CID_PRIVATE_BASE;
			else
				goto enum_end;
			goto next;
		}
		res = -errno;
		spa_log_error(this->log, "'%s' VIDIOC_QUERYCTRL: %m", this->props.device);
		spa_v4l2_close(dev);
		return res;
	}
	if (result.next & next_fl)
		result.next = queryctrl.id | next_fl;
	else
		result.next++;

	if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED)
		goto next;

	if (port->n_controls >= MAX_CONTROLS)
		goto enum_end;

	ctrl_id = queryctrl.id & ~next_fl;

	spa_pod_builder_reset(&b.b, &state);

	prop_id = control_to_prop_id(this, ctrl_id);

	port->controls[port->n_controls].id = prop_id;
	port->controls[port->n_controls].ctrl_id = ctrl_id;
	port->controls[port->n_controls].value = queryctrl.default_value;

	spa_log_debug(this->log, "Control '%s' %d %d", queryctrl.name, prop_id, ctrl_id);

	switch (queryctrl.type) {
	case V4L2_CTRL_TYPE_INTEGER:
		port->controls[port->n_controls].type = SPA_TYPE_Int;
		param = spa_pod_builder_add_object(&b.b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_id,   SPA_POD_Id(prop_id),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_STEP_Int(
							(int32_t)queryctrl.default_value,
							(int32_t)queryctrl.minimum,
							(int32_t)queryctrl.maximum,
							(int32_t)queryctrl.step),
			SPA_PROP_INFO_description, SPA_POD_String(queryctrl.name));
		break;
	case V4L2_CTRL_TYPE_BOOLEAN:
		port->controls[port->n_controls].type = SPA_TYPE_Bool;
		param = spa_pod_builder_add_object(&b.b,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_id,   SPA_POD_Id(prop_id),
			SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool((bool)queryctrl.default_value),
			SPA_PROP_INFO_description, SPA_POD_String(queryctrl.name));
		break;
	case V4L2_CTRL_TYPE_MENU:
	{
		struct v4l2_querymenu querymenu;

		port->controls[port->n_controls].type = SPA_TYPE_Int;
		spa_pod_builder_push_object(&b.b, &f[0], SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo);
		spa_pod_builder_add(&b.b,
			SPA_PROP_INFO_id,    SPA_POD_Id(prop_id),
			SPA_PROP_INFO_type,  SPA_POD_CHOICE_ENUM_Int(1, (int32_t)queryctrl.default_value),
			SPA_PROP_INFO_description,  SPA_POD_String(queryctrl.name),
			0);

		spa_zero(querymenu);
		querymenu.id = queryctrl.id;

		spa_pod_builder_prop(&b.b, SPA_PROP_INFO_labels, 0);

		spa_pod_builder_push_struct(&b.b, &f[1]);
		for (querymenu.index = queryctrl.minimum;
		    querymenu.index <= queryctrl.maximum;
		    querymenu.index++) {
			if (xioctl(dev->fd, VIDIOC_QUERYMENU, &querymenu) == 0) {
				spa_pod_builder_int(&b.b, querymenu.index);
				spa_pod_builder_string(&b.b, (const char *)querymenu.name);
			}
		}
		spa_pod_builder_pop(&b.b, &f[1]);
		param = spa_pod_builder_pop(&b.b, &f[0]);
		break;
	}
	case V4L2_CTRL_TYPE_INTEGER_MENU:
	case V4L2_CTRL_TYPE_BITMASK:
	case V4L2_CTRL_TYPE_BUTTON:
	case V4L2_CTRL_TYPE_INTEGER64:
	case V4L2_CTRL_TYPE_STRING:
	default:
		goto next;

	}

	port->n_controls++;

	if (spa_pod_filter(&b.b, &result.param, param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

      enum_end:
	res = 0;
	spa_v4l2_close(dev);
	return res;
}

static int
spa_v4l2_update_controls(struct impl *this)
{
	struct port *port = &this->out_ports[0];
	struct spa_v4l2_device *dev = &port->dev;
	int res;
	uint32_t i;

	if ((res = spa_v4l2_open(dev, this->props.device)) < 0)
		return res;

	for (i = 0; i < port->n_controls; i++) {
		struct control *c = &port->controls[i];
		struct v4l2_control control;

		spa_zero(control);
		control.id = c->ctrl_id;
		if (xioctl(dev->fd, VIDIOC_G_CTRL, &control) < 0) {
			/* Write only controls like relative pan/tilt return EACCES */
			if (errno == EACCES) {
				c->value = 0;
				continue;
			}
			res = -errno;
			goto done;
		}
		c->value = control.value;
	}
	res = 0;
done:
	spa_v4l2_close(dev);
	return res;
}

static int
spa_v4l2_set_control(struct impl *this, const struct spa_pod_prop *prop, const void *body)
{
	struct port *port = &this->out_ports[0];
	struct spa_v4l2_device *dev = &port->dev;
	struct v4l2_control control;
	int res;

	spa_zero(control);
	control.id = prop_id_to_control(this, prop->key);
	if (control.id == SPA_ID_INVALID)
		return -ENOENT;

	if ((res = spa_v4l2_open(dev, this->props.device)) < 0)
		return res;

	switch (prop->value.type) {
	case SPA_TYPE_Bool:
	{
		bool val;
		if ((res = spa_pod_get_bool(&prop->value, &val)) < 0)
			goto done;
		control.value = val;
		break;
	}
	case SPA_TYPE_Float:
	{
		float val;
		if ((res = spa_pod_get_float(&prop->value, &val)) < 0)
			goto done;
		control.value = (int32_t) val;
		break;
	}
	case SPA_TYPE_Int:
	{
		int32_t val;
		if ((res = spa_pod_get_int(&prop->value, &val)) < 0)
			goto done;
		control.value = val;
		break;
	}
	default:
		res = -EINVAL;
		goto done;
	}
	if (xioctl(dev->fd, VIDIOC_S_CTRL, &control) < 0) {
		res = -errno;
		goto done;
	}

	res = 0;

done:
	spa_v4l2_close(dev);
	return res;
}

static int mmap_read(struct impl *this)
{
	struct port *port = &this->out_ports[0];
	struct spa_v4l2_device *dev = &port->dev;
	struct v4l2_buffer buf;
	struct buffer *b;
	struct spa_data *d;
	int64_t pts;

	spa_zero(buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = port->memtype;

	if (xioctl(dev->fd, VIDIOC_DQBUF, &buf) < 0)
		return -errno;

	spa_log_trace(this->log, "v4l2 %p: have output %d/%d", this, buf.index, buf.sequence);

	/* Drop the first frame in order to work around common firmware
	 * timestamp issues */
	if (port->first_buffer) {
		port->first_buffer = false;
		if (xioctl(dev->fd, VIDIOC_QBUF, &buf) < 0)
			spa_log_warn(this->log, "v4l2 %p: error qbuf: %m", this);
		return 0;
	}

	pts = SPA_TIMEVAL_TO_NSEC(&buf.timestamp);


	if (this->clock) {
		double target = (double)port->info.rate.num / port->info.rate.denom;
		double corr;

		if (this->dll.bw == 0.0) {
			spa_dll_set_bw(&this->dll, SPA_DLL_BW_MAX, port->info.rate.denom, port->info.rate.denom);
			this->clock->next_nsec = pts;
			corr = 1.0;
		} else {
			double diff = ((double)this->clock->next_nsec - (double)pts) / SPA_NSEC_PER_SEC;
			double error = port->info.rate.denom * (diff - target);
			corr = spa_dll_update(&this->dll, SPA_CLAMPD(error, -128., 128.));
		}

		/* FIXME, we should follow the driver clock and target_ values.
		 * for now we ignore and use our own. */
		this->clock->target_rate = port->info.rate;
		this->clock->target_duration = 1;

		this->clock->nsec = pts;
		this->clock->rate = port->info.rate;
		this->clock->position = buf.sequence;
		this->clock->duration = 1;
		this->clock->delay = 0;
		this->clock->rate_diff = corr;
		this->clock->next_nsec += (uint64_t) (target * SPA_NSEC_PER_SEC * corr);
	}

	b = &port->buffers[buf.index];
	if (b->h) {
		b->h->flags = 0;
		if (buf.flags & V4L2_BUF_FLAG_ERROR)
			b->h->flags |= SPA_META_HEADER_FLAG_CORRUPTED;
		b->h->offset = 0;
		b->h->seq = buf.sequence;
		b->h->pts = pts;
		b->h->dts_offset = 0;
	}
	if (b->vt) {
		b->vt->transform = this->transform;
	}

	d = b->outbuf->datas;
	d[0].chunk->offset = 0;
	d[0].chunk->size = SPA_MIN(buf.bytesused, d[0].maxsize);
	d[0].chunk->stride = port->fmt.fmt.pix.bytesperline;
	d[0].chunk->flags = 0;
	if (buf.flags & V4L2_BUF_FLAG_ERROR)
		d[0].chunk->flags |= SPA_CHUNK_FLAG_CORRUPTED;

	if (b->mmap_ptr && b->ptr)
		memcpy(b->ptr, b->mmap_ptr, d[0].chunk->size);

	spa_list_append(&port->queue, &b->link);
	return 0;
}

static void v4l2_on_fd_events(struct spa_source *source)
{
	struct impl *this = source->data;
	struct spa_io_buffers *io;
	struct port *port = &this->out_ports[0];
	struct buffer *b;
	int res;

	if (source->rmask & SPA_IO_ERR) {
		struct port *port = &this->out_ports[0];
		spa_log_error(this->log, "'%p' error %08x", this->props.device, source->rmask);
		if (port->source.loop)
			spa_loop_remove_source(this->data_loop, &port->source);
		return;
	}

	if (!(source->rmask & SPA_IO_IN)) {
		spa_log_warn(this->log, "v4l2 %p: spurious wakeup %d", this, source->rmask);
		return;
	}

	if ((res = mmap_read(this)) < 0) {
		spa_log_warn(this->log, "v4l2 %p: mmap read error:%s", this, spa_strerror(res));
		return;
	}

	if (spa_list_is_empty(&port->queue))
		return;

	io = port->io;
	if (io == NULL) {
		b = spa_list_first(&port->queue, struct buffer, link);
		spa_list_remove(&b->link);
		SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUTSTANDING);
		spa_v4l2_buffer_recycle(this, b->id);
	}
	else if (io->status != SPA_STATUS_HAVE_DATA) {
		if (io->buffer_id < port->n_buffers)
			spa_v4l2_buffer_recycle(this, io->buffer_id);

		b = spa_list_first(&port->queue, struct buffer, link);
		spa_list_remove(&b->link);
		SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUTSTANDING);

		io->buffer_id = b->id;
		io->status = SPA_STATUS_HAVE_DATA;
		spa_log_trace(this->log, "v4l2 %p: now queued %d", this, b->id);
	}
	spa_node_call_ready(&this->callbacks, SPA_STATUS_HAVE_DATA);
}

static int spa_v4l2_use_buffers(struct impl *this, struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct port *port = &this->out_ports[0];
	struct spa_v4l2_device *dev = &port->dev;
	struct v4l2_requestbuffers reqbuf;
	unsigned int i;
	struct spa_data *d;

	if (n_buffers > 0) {
		d = buffers[0]->datas;

		if (d[0].type == SPA_DATA_MemFd ||
		    (d[0].type == SPA_DATA_MemPtr && d[0].data != NULL)) {
			port->memtype = V4L2_MEMORY_USERPTR;
		} else if (d[0].type == SPA_DATA_DmaBuf) {
			port->memtype = V4L2_MEMORY_DMABUF;
		} else {
			spa_log_error(this->log, "%s: can't use buffers of type %d",
					this->props.device, d[0].type);
			return -EINVAL;
		}
	}

	spa_zero(reqbuf);
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqbuf.memory = port->memtype;
	reqbuf.count = n_buffers;

	if (xioctl(dev->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		if (port->memtype != V4L2_MEMORY_USERPTR) {
			spa_log_error(this->log, "'%s' VIDIOC_REQBUFS %m", this->props.device);
			return -errno;
		}
		/* some drivers (v4l2loopback) don't support USERPTR
		 * and so we need to try again with MMAP and memcpy */
		port->memtype = V4L2_MEMORY_MMAP;
		spa_zero(reqbuf);
		reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		reqbuf.memory = port->memtype;
		reqbuf.count = n_buffers;

		if (xioctl(dev->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
			spa_log_error(this->log, "'%s' VIDIOC_REQBUFS %m", this->props.device);
			return -errno;
		}
	}
	spa_log_debug(this->log, "got %d buffers", reqbuf.count);
	if (reqbuf.count < n_buffers) {
		spa_log_error(this->log, "'%s' can't allocate enough buffers %d < %d",
				this->props.device, reqbuf.count, n_buffers);
		return -ENOMEM;
	}

	for (i = 0; i < reqbuf.count; i++) {
		struct buffer *b;

		b = &port->buffers[i];
		b->id = i;
		b->outbuf = buffers[i];
		b->flags = BUFFER_FLAG_OUTSTANDING;
		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));
		b->vt = spa_buffer_find_meta_data(buffers[i], SPA_META_VideoTransform, sizeof(*b->vt));

		spa_log_debug(this->log, "%s: import buffer %p", this->props.device, buffers[i]);

		if (buffers[i]->n_datas < 1) {
			spa_log_error(this->log, "%s: invalid memory on buffer %p",
					this->props.device, buffers[i]);
			return -EINVAL;
		}
		d = buffers[i]->datas;

		spa_zero(b->v4l2_buffer);
		b->v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b->v4l2_buffer.memory = port->memtype;
		b->v4l2_buffer.index = i;

		if (port->memtype == V4L2_MEMORY_USERPTR ||
		    port->memtype == V4L2_MEMORY_MMAP) {
			if (d[0].data == NULL) {
				void *data;

				data = mmap(NULL,
					    d[0].maxsize,
					    PROT_READ | PROT_WRITE, MAP_SHARED,
					    d[0].fd,
					    d[0].mapoffset);
				if (data == MAP_FAILED)
					return -errno;

				b->ptr = data;
				SPA_FLAG_SET(b->flags, BUFFER_FLAG_MAPPED);
			}
			else
				b->ptr = d[0].data;

			if (port->memtype == V4L2_MEMORY_USERPTR) {
				b->v4l2_buffer.m.userptr = (unsigned long) b->ptr;
				b->v4l2_buffer.length = d[0].maxsize;
			}
			else {
				if (xioctl(dev->fd, VIDIOC_QUERYBUF, &b->v4l2_buffer) < 0) {
					spa_log_error(this->log, "'%s' VIDIOC_QUERYBUF: %m", this->props.device);
					return -errno;
				}
				b->mmap_ptr = mmap(NULL,
						b->v4l2_buffer.length,
						PROT_READ, MAP_PRIVATE,
						dev->fd, b->v4l2_buffer.m.offset);
				if (b->mmap_ptr == MAP_FAILED) {
					spa_log_error(this->log, "'%s' mmap: %m", this->props.device);
					return -errno;
				}
			}
		}
		else if (port->memtype == V4L2_MEMORY_DMABUF) {
			b->v4l2_buffer.m.fd = d[0].fd;
		}
		else {
			spa_log_error(this->log, "%s: invalid port memory %d",
					this->props.device, port->memtype);
			return -EIO;
		}

		spa_v4l2_buffer_recycle(this, i);
	}
	port->n_buffers = reqbuf.count;

	return 0;
}

static int
mmap_init(struct impl *this,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct port *port = &this->out_ports[0];
	struct spa_v4l2_device *dev = &port->dev;
	struct v4l2_requestbuffers reqbuf;
	unsigned int i;
	bool use_expbuf = false;

	port->memtype = V4L2_MEMORY_MMAP;

	spa_zero(reqbuf);
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqbuf.memory = port->memtype;
	reqbuf.count = n_buffers;

	if (xioctl(dev->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		spa_log_error(this->log, "'%s' VIDIOC_REQBUFS: %m", this->props.device);
		return -errno;
	}

	spa_log_debug(this->log, "got %d buffers", reqbuf.count);

	if (reqbuf.count < n_buffers) {
		spa_log_error(this->log, "'%s' can't allocate enough buffers (%d < %d)",
				this->props.device, reqbuf.count, n_buffers);
		return -ENOMEM;
	}

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d;

		if (buffers[i]->n_datas < 1) {
			spa_log_error(this->log, "%s: invalid buffer data",
					this->props.device);
			return -EINVAL;
		}

		b = &port->buffers[i];
		b->id = i;
		b->outbuf = buffers[i];
		b->flags = BUFFER_FLAG_OUTSTANDING;
		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));
		b->vt = spa_buffer_find_meta_data(buffers[i], SPA_META_VideoTransform, sizeof(*b->vt));

		spa_zero(b->v4l2_buffer);
		b->v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b->v4l2_buffer.memory = port->memtype;
		b->v4l2_buffer.index = i;

		if (xioctl(dev->fd, VIDIOC_QUERYBUF, &b->v4l2_buffer) < 0) {
			spa_log_error(this->log, "'%s' VIDIOC_QUERYBUF: %m", this->props.device);
			return -errno;
		}

		if (b->v4l2_buffer.flags & V4L2_BUF_FLAG_QUEUED) {
			/* some drivers can give us an already queued buffer. */
			spa_log_warn(this->log, "buffer %d was already queued", i);
			n_buffers = i;
			break;
		}

		d = buffers[i]->datas;
		d[0].mapoffset = 0;
		d[0].maxsize = b->v4l2_buffer.length;
		d[0].chunk->offset = 0;
		d[0].chunk->size = 0;
		d[0].chunk->stride = port->fmt.fmt.pix.bytesperline;
		d[0].chunk->flags = 0;

		spa_log_debug(this->log, "data types %08x", d[0].type);

again:
		if (port->have_expbuf &&
		    d[0].type != SPA_ID_INVALID &&
		    (d[0].type & ((1u << SPA_DATA_DmaBuf)|(1u<<SPA_DATA_MemFd)))) {
			struct v4l2_exportbuffer expbuf;

			spa_zero(expbuf);
			expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			expbuf.index = i;
			expbuf.flags = O_CLOEXEC | O_RDONLY;
			if (xioctl(dev->fd, VIDIOC_EXPBUF, &expbuf) < 0) {
				if (errno == ENOTTY || errno == EINVAL) {
					spa_log_debug(this->log, "'%s' VIDIOC_EXPBUF not supported: %m",
							this->props.device);
					port->have_expbuf = false;
					goto again;
				}
				spa_log_error(this->log, "'%s' VIDIOC_EXPBUF: %m", this->props.device);
				return -errno;
			}
			if (d[0].type & (1u<<SPA_DATA_DmaBuf))
				d[0].type = SPA_DATA_DmaBuf;
			else
				d[0].type = SPA_DATA_MemFd;
			d[0].flags = SPA_DATA_FLAG_READABLE | SPA_DATA_FLAG_MAPPABLE;
			d[0].fd = expbuf.fd;
			d[0].data = NULL;
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_ALLOCATED);
			spa_log_debug(this->log, "EXPBUF fd:%d", expbuf.fd);
			use_expbuf = true;
		} else if (d[0].type & (1u << SPA_DATA_MemPtr)) {
			d[0].type = SPA_DATA_MemPtr;
			d[0].flags = SPA_DATA_FLAG_READABLE;
			d[0].fd = -1;
			d[0].mapoffset = b->v4l2_buffer.m.offset;
			d[0].data = mmap(NULL,
					b->v4l2_buffer.length,
					PROT_READ, MAP_SHARED,
					dev->fd,
					b->v4l2_buffer.m.offset);
			if (d[0].data == MAP_FAILED) {
				spa_log_error(this->log, "'%s' mmap: %m", this->props.device);
				return -errno;
			}
			b->ptr = d[0].data;
			SPA_FLAG_SET(b->flags, BUFFER_FLAG_MAPPED);
			spa_log_debug(this->log, "mmap offset:%u data:%p", d[0].mapoffset, b->ptr);
			use_expbuf = false;
		} else {
			spa_log_error(this->log, "%s: unsupported data type:%08x",
					this->props.device, d[0].type);
			port->alloc_buffers = false;
			return -ENOTSUP;
		}
		spa_v4l2_buffer_recycle(this, i);
	}
	spa_log_info(this->log, "%s: have %u buffers using %s", dev->path, n_buffers,
			use_expbuf ? "EXPBUF" : "MMAP");

	port->n_buffers = n_buffers;

	return 0;
}

static int userptr_init(struct impl *this)
{
	return -ENOTSUP;
}

static int read_init(struct impl *this)
{
	return -ENOTSUP;
}

static int
spa_v4l2_alloc_buffers(struct impl *this,
		       struct spa_buffer **buffers,
		       uint32_t n_buffers)
{
	int res;
	struct port *port = &this->out_ports[0];
	struct spa_v4l2_device *dev = &port->dev;

	if (port->n_buffers > 0)
		return -EIO;

	if (dev->cap.capabilities & V4L2_CAP_STREAMING) {
		if ((res = mmap_init(this, buffers, n_buffers)) < 0)
			if ((res = userptr_init(this)) < 0)
				return res;
	} else if (dev->cap.capabilities & V4L2_CAP_READWRITE) {
		if ((res = read_init(this)) < 0)
			return res;
	} else {
		spa_log_error(this->log, "%s: invalid capabilities %08x",
				this->props.device, dev->cap.capabilities);
		return -EIO;
	}

	return 0;
}

static int spa_v4l2_stream_on(struct impl *this)
{
	struct port *port = &this->out_ports[0];
	struct spa_v4l2_device *dev = &port->dev;
	enum v4l2_buf_type type;

	if (dev->fd == -1)
		return -EIO;

	if (!dev->have_format)
		return -EIO;

	if (dev->active)
		return 0;

	spa_log_debug(this->log, "starting");

	if (port->current_format.media_subtype == SPA_MEDIA_SUBTYPE_raw ||
	    port->current_format.media_subtype == SPA_MEDIA_SUBTYPE_mjpg ||
	    port->current_format.media_subtype == SPA_MEDIA_SUBTYPE_jpeg)
		port->first_buffer = true;
	else
		port->first_buffer = false;
	mmap_read(this);

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(dev->fd, VIDIOC_STREAMON, &type) < 0) {
		spa_log_error(this->log, "'%s' VIDIOC_STREAMON: %m", this->props.device);
		return -errno;
	}
	this->dll.bw = 0.0;

	port->source.func = v4l2_on_fd_events;
	port->source.data = this;
	port->source.fd = dev->fd;
	port->source.mask = SPA_IO_IN | SPA_IO_ERR;
	port->source.rmask = 0;
	spa_loop_add_source(this->data_loop, &port->source);

	dev->active = true;

	return 0;
}

static int do_remove_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct port *port = user_data;
	if (port->source.loop)
		spa_loop_remove_source(loop, &port->source);
	return 0;
}

static int spa_v4l2_stream_off(struct impl *this)
{
	struct port *port = &this->out_ports[0];
	struct spa_v4l2_device *dev = &port->dev;
	enum v4l2_buf_type type;
	uint32_t i;

	if (!dev->active)
		return 0;

	if (dev->fd == -1)
		return -EIO;

	spa_log_debug(this->log, "stopping");

	spa_loop_locked(this->data_loop, do_remove_source, 0, NULL, 0, port);

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(dev->fd, VIDIOC_STREAMOFF, &type) < 0) {
		spa_log_error(this->log, "'%s' VIDIOC_STREAMOFF: %m", this->props.device);
		return -errno;
	}
	for (i = 0; i < port->n_buffers; i++) {
		struct buffer *b;

		b = &port->buffers[i];
		if (!SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_OUTSTANDING)) {
			if (xioctl(dev->fd, VIDIOC_QBUF, &b->v4l2_buffer) < 0)
				spa_log_warn(this->log, "VIDIOC_QBUF: %s", strerror(errno));
		}
	}
	spa_list_init(&port->queue);
	dev->active = false;

	return 0;
}
