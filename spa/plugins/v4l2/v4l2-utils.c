#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))

static void v4l2_on_fd_events (SpaSource *source);

static int
xioctl (int fd, int request, void *arg)
{
  int err;

  do {
    err = ioctl (fd, request, arg);
  } while (err == -1 && errno == EINTR);

  return err;
}

static int
spa_v4l2_open (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state[0];
  struct stat st;
  SpaV4l2SourceProps *props = &this->props;

  if (state->opened)
    return 0;

  if (props->device[0] == '\0') {
    spa_log_error (state->log, "v4l2: Device property not set");
    return -1;
  }

  spa_log_info (state->log, "v4l2: Playback device is '%s'", props->device);

  if (stat (props->device, &st) < 0) {
    spa_log_error (state->log, "v4l2: Cannot identify '%s': %d, %s",
            props->device, errno, strerror (errno));
    return -1;
  }

  if (!S_ISCHR (st.st_mode)) {
    spa_log_error (state->log, "v4l2: %s is no device", props->device);
    return -1;
  }

  state->fd = open (props->device, O_RDWR | O_NONBLOCK, 0);

  if (state->fd == -1) {
    spa_log_error (state->log, "v4l2: Cannot open '%s': %d, %s",
            props->device, errno, strerror (errno));
    return -1;
  }

  if (xioctl (state->fd, VIDIOC_QUERYCAP, &state->cap) < 0) {
    perror ("QUERYCAP");
    return -1;
  }

  if ((state->cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
    spa_log_error (state->log, "v4l2: %s is no video capture device", props->device);
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

static SpaResult
spa_v4l2_buffer_recycle (SpaV4l2Source *this, uint32_t buffer_id)
{
  SpaV4l2State *state = &this->state[0];
  V4l2Buffer *b = &state->buffers[buffer_id];

  if (!b->outstanding)
    return SPA_RESULT_OK;

  b->outstanding = false;

  if (xioctl (state->fd, VIDIOC_QBUF, &b->v4l2_buffer) < 0) {
    perror ("VIDIOC_QBUF");
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_clear_buffers (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state[0];
  struct v4l2_requestbuffers reqbuf;
  int i;

  if (state->n_buffers == 0)
    return SPA_RESULT_OK;

  for (i = 0; i < state->n_buffers; i++) {
    V4l2Buffer *b;

    b = &state->buffers[i];
    if (b->outstanding) {
      spa_log_info (state->log, "v4l2: queueing outstanding buffer %p", b);
      spa_v4l2_buffer_recycle (this, i);
    }
    if (b->allocated) {
      if (b->outbuf->datas[0].data)
        munmap (b->outbuf->datas[0].data, b->outbuf->datas[0].maxsize);
      if (b->outbuf->datas[0].fd != -1)
        close (b->outbuf->datas[0].fd);
      b->outbuf->datas[0].type = SPA_DATA_TYPE_INVALID;
    }
  }

  CLEAR(reqbuf);
  reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  reqbuf.memory = state->memtype;
  reqbuf.count = 0;

  if (xioctl (state->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
    perror ("VIDIOC_REQBUFS");
  }
  state->n_buffers = 0;

  return SPA_RESULT_OK;
}

static int
spa_v4l2_close (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state[0];

  if (!state->opened)
    return 0;

  if (state->n_buffers > 0)
    return 0;

  spa_log_info (state->log, "v4l2: close");

  if (state->source_enabled)
    spa_loop_remove_source (state->data_loop, &state->source);

  if (close(state->fd))
    perror ("close");

  state->fd = -1;
  state->opened = false;

  return 0;
}

typedef struct {
  uint32_t fourcc;
  SpaVideoFormat format;
  SpaMediaType media_type;
  SpaMediaSubType media_subtype;
} FormatInfo;

static const FormatInfo format_info[] =
{
  /* RGB formats */
  { V4L2_PIX_FMT_RGB332,       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_ARGB555,      SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_XRGB555,      SPA_VIDEO_FORMAT_RGB15, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_ARGB555X,     SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_XRGB555X,     SPA_VIDEO_FORMAT_BGR15, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_RGB565,       SPA_VIDEO_FORMAT_RGB16, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_RGB565X,      SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_BGR666,       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_BGR24,        SPA_VIDEO_FORMAT_BGR, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_RGB24,        SPA_VIDEO_FORMAT_RGB, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_ABGR32,       SPA_VIDEO_FORMAT_BGRA, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_XBGR32,       SPA_VIDEO_FORMAT_BGRx, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_ARGB32,       SPA_VIDEO_FORMAT_ARGB, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_XRGB32,       SPA_VIDEO_FORMAT_xRGB, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },

  /* Deprecated Packed RGB Image Formats (alpha ambiguity) */
  { V4L2_PIX_FMT_RGB444,       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_RGB555,       SPA_VIDEO_FORMAT_RGB15, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_RGB555X,      SPA_VIDEO_FORMAT_BGR15, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_BGR32,        SPA_VIDEO_FORMAT_BGRx, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_RGB32,        SPA_VIDEO_FORMAT_xRGB, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },

  /* Grey formats */
  { V4L2_PIX_FMT_GREY,         SPA_VIDEO_FORMAT_GRAY8, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_Y4,           SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_Y6,           SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_Y10,          SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_Y12,          SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_Y16,          SPA_VIDEO_FORMAT_GRAY16_LE, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_Y16_BE,       SPA_VIDEO_FORMAT_GRAY16_BE, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_Y10BPACK,     SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },

  /* Palette formats */
  { V4L2_PIX_FMT_PAL8,	       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },

  /* Chrominance formats */
  { V4L2_PIX_FMT_UV8,	       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },

  /* Luminance+Chrominance formats */
  { V4L2_PIX_FMT_YVU410,       SPA_VIDEO_FORMAT_YVU9, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_YVU420,       SPA_VIDEO_FORMAT_YV12, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_YVU420M,      SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_YUYV,	       SPA_VIDEO_FORMAT_YUY2, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_YYUV,	       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_YVYU,	       SPA_VIDEO_FORMAT_YVYU, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_UYVY,	       SPA_VIDEO_FORMAT_UYVY, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_VYUY,	       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_YUV422P,      SPA_VIDEO_FORMAT_Y42B, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_YUV411P,      SPA_VIDEO_FORMAT_Y41B, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_Y41P,	       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_YUV444,       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_YUV555,       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_YUV565,       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_YUV32,	       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_YUV410,       SPA_VIDEO_FORMAT_YUV9, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_YUV420,       SPA_VIDEO_FORMAT_I420, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_YUV420M,      SPA_VIDEO_FORMAT_I420, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_HI240,	       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_HM12,	       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_M420,	       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },

  /* two planes -- one Y, one Cr + Cb interleaved  */
  { V4L2_PIX_FMT_NV12,         SPA_VIDEO_FORMAT_NV12, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_NV12M,        SPA_VIDEO_FORMAT_NV12, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_NV12MT,       SPA_VIDEO_FORMAT_NV12_64Z32, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_NV12MT_16X16, SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_NV21,         SPA_VIDEO_FORMAT_NV21, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_NV21M,        SPA_VIDEO_FORMAT_NV21, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_NV16,         SPA_VIDEO_FORMAT_NV16, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_NV16M,        SPA_VIDEO_FORMAT_NV16, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_NV61,         SPA_VIDEO_FORMAT_NV61, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_NV61M,        SPA_VIDEO_FORMAT_NV61, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_NV24,         SPA_VIDEO_FORMAT_NV24, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_NV42,         SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },

  /* Bayer formats - see http://www.siliconimaging.com/RGB%20Bayer.htm */
  { V4L2_PIX_FMT_SBGGR8,       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_BAYER },
  { V4L2_PIX_FMT_SGBRG8,       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_BAYER },
  { V4L2_PIX_FMT_SGRBG8,       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_BAYER },
  { V4L2_PIX_FMT_SRGGB8,       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_BAYER },

  /* compressed formats */
  { V4L2_PIX_FMT_MJPEG,        SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_MJPG },
  { V4L2_PIX_FMT_JPEG,         SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_IMAGE, SPA_MEDIA_SUBTYPE_JPEG },
  { V4L2_PIX_FMT_PJPG,         SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_DV,           SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_DV },
  { V4L2_PIX_FMT_MPEG,         SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_MPEGTS },
  { V4L2_PIX_FMT_H264,         SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_H264 },
  { V4L2_PIX_FMT_H264_NO_SC,   SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_H264 },
  { V4L2_PIX_FMT_H264_MVC,     SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_H264 },
  { V4L2_PIX_FMT_H263,         SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_H263 },
  { V4L2_PIX_FMT_MPEG1,        SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_MPEG1 },
  { V4L2_PIX_FMT_MPEG2,        SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_MPEG2 },
  { V4L2_PIX_FMT_MPEG4,        SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_MPEG4 },
  { V4L2_PIX_FMT_XVID,         SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_XVID },
  { V4L2_PIX_FMT_VC1_ANNEX_G,  SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_VC1 },
  { V4L2_PIX_FMT_VC1_ANNEX_L,  SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_VC1 },
  { V4L2_PIX_FMT_VP8,          SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_VP8 },

  /*  Vendor-specific formats   */
  { V4L2_PIX_FMT_WNVA,         SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_SN9C10X,      SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_PWC1,         SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  { V4L2_PIX_FMT_PWC2,         SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
};

static const FormatInfo *
fourcc_to_format_info (uint32_t fourcc)
{
  int i;

  for (i = 0; i < SPA_N_ELEMENTS (format_info); i++) {
    if (format_info[i].fourcc == fourcc)
      return &format_info[i];
  }
  return NULL;
}

#if 0
static const FormatInfo *
video_format_to_format_info (SpaVideoFormat format)
{
  int i;

  for (i = 0; i < SPA_N_ELEMENTS (format_info); i++) {
    if (format_info[i].format == format)
      return &format_info[i];
  }
  return NULL;
}
#endif

static const FormatInfo *
find_format_info_by_media_type (SpaMediaType    type,
                                SpaMediaSubType subtype,
                                SpaVideoFormat  format,
                                int             startidx)
{
  int i;

  for (i = startidx; i < SPA_N_ELEMENTS (format_info); i++) {
    if ((format_info[i].media_type == type) &&
        (format_info[i].media_subtype == subtype) &&
        (format == SPA_VIDEO_FORMAT_UNKNOWN || format_info[i].format == format))
      return &format_info[i];
  }
  return NULL;
}

static SpaVideoFormat
enum_filter_format (const SpaFormat *filter, unsigned int index)
{
  SpaVideoFormat video_format = SPA_VIDEO_FORMAT_UNKNOWN;

  if ((filter->body.media_type.value == SPA_MEDIA_TYPE_VIDEO ||
       filter->body.media_type.value == SPA_MEDIA_TYPE_IMAGE)) {
    if (filter->body.media_subtype.value == SPA_MEDIA_SUBTYPE_RAW) {
      SpaPODProp *p;
      unsigned int n_values;
      const uint32_t *values;

      if (!(p = spa_format_find_prop (filter, SPA_PROP_ID_VIDEO_FORMAT)))
        return SPA_VIDEO_FORMAT_UNKNOWN;

      if (p->body.value.type != SPA_POD_TYPE_INT)
        return SPA_VIDEO_FORMAT_UNKNOWN;

      values = SPA_POD_BODY_CONST (&p->body.value);
      n_values = SPA_POD_PROP_N_VALUES (p);

      if (p->body.flags & SPA_POD_PROP_FLAG_UNSET) {
        if (index + 1 < n_values)
          video_format = values[index + 1];
      } else {
        if (index == 0)
          video_format = values[0];
      }
    } else {
      if (index == 0)
        video_format = SPA_VIDEO_FORMAT_ENCODED;
    }
  }
  return video_format;
}

static bool
filter_framesize (struct v4l2_frmsizeenum *frmsize,
                  const SpaRectangle      *min,
                  const SpaRectangle      *max,
                  const SpaRectangle      *step)
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

    frmsize->stepwise.min_width = SPA_MAX (frmsize->stepwise.min_width, min->width);
    frmsize->stepwise.min_height = SPA_MAX (frmsize->stepwise.min_height, min->height);
    frmsize->stepwise.max_width = SPA_MIN (frmsize->stepwise.max_width, max->width);
    frmsize->stepwise.max_height = SPA_MIN (frmsize->stepwise.max_height, max->height);
  } else
    return false;

  return true;
}

static int
compare_fraction (struct v4l2_fract *f1, const SpaFraction *f2)
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
filter_framerate (struct v4l2_frmivalenum *frmival,
                  const SpaFraction       *min,
                  const SpaFraction       *max,
                  const SpaFraction       *step)
{
  if (frmival->type == V4L2_FRMIVAL_TYPE_DISCRETE) {
    if (compare_fraction (&frmival->discrete, min) < 0 ||
        compare_fraction (&frmival->discrete, max) > 0)
      return false;
  } else if (frmival->type == V4L2_FRMIVAL_TYPE_CONTINUOUS ||
             frmival->type == V4L2_FRMIVAL_TYPE_STEPWISE) {
    /* FIXME, use LCM */
    frmival->stepwise.step.denominator *= step->num;
    frmival->stepwise.step.numerator *= step->denom;

    if (compare_fraction (&frmival->stepwise.max, min) < 0 ||
        compare_fraction (&frmival->stepwise.min, max) > 0)
      return false;

    if (compare_fraction (&frmival->stepwise.min, min) < 0) {
      frmival->stepwise.min.denominator = min->num;
      frmival->stepwise.min.numerator = min->denom;
    }
    if (compare_fraction (&frmival->stepwise.max, max) > 0) {
      frmival->stepwise.max.denominator = max->num;
      frmival->stepwise.max.numerator = max->denom;
    }
  } else
    return false;

  return true;
}

#define FOURCC_ARGS(f) (f)&0x7f,((f)>>8)&0x7f,((f)>>16)&0x7f,((f)>>24)&0x7f

static SpaResult
spa_v4l2_enum_format (SpaV4l2Source   *this,
                      SpaFormat      **format,
                      const SpaFormat *filter,
                      unsigned int     index)
{
  SpaV4l2State *state = &this->state[0];
  int res, n_fractions;
  const FormatInfo *info;
  SpaPODFrame f[2];
  SpaPODProp *prop;
  SpaFormat *fmt;
  SpaPODBuilder b = { state->format_buffer, sizeof (state->format_buffer), };

  if (spa_v4l2_open (this) < 0)
    return SPA_RESULT_ERROR;

  *format = NULL;

  if (index == 0) {
    CLEAR (state->fmtdesc);
    state->fmtdesc.index = 0;
    state->fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    state->next_fmtdesc = true;
    CLEAR (state->frmsize);
    state->next_frmsize = true;
    CLEAR (state->frmival);
  }

  if (false) {
next_fmtdesc:
    state->fmtdesc.index++;
    state->next_fmtdesc = true;
  }

  while (state->next_fmtdesc) {
    if (filter) {
      SpaVideoFormat video_format;

      video_format = enum_filter_format (filter, state->fmtdesc.index);
      if (video_format == SPA_VIDEO_FORMAT_UNKNOWN)
        return SPA_RESULT_ENUM_END;

      info = find_format_info_by_media_type (filter->body.media_type.value,
                                             filter->body.media_subtype.value,
                                             video_format,
                                             0);
      if (info == NULL)
        goto next_fmtdesc;

      state->fmtdesc.pixelformat = info->fourcc;
    } else {
      if ((res = xioctl (state->fd, VIDIOC_ENUM_FMT, &state->fmtdesc)) < 0) {
        if (errno != EINVAL)
          perror ("VIDIOC_ENUM_FMT");
        return SPA_RESULT_ENUM_END;
      }
    }
    state->next_fmtdesc = false;
    state->frmsize.index = 0;
    state->frmsize.pixel_format = state->fmtdesc.pixelformat;
    state->next_frmsize = true;
  }
  if (!(info = fourcc_to_format_info (state->fmtdesc.pixelformat)))
    goto next_fmtdesc;

next_frmsize:
  while (state->next_frmsize) {
    if (filter) {
      SpaPODProp *p;

      /* check if we have a fixed frame size */
      if (!(p = spa_format_find_prop (filter, SPA_PROP_ID_VIDEO_SIZE)))
        goto do_frmsize;

      if (p->body.value.type != SPA_POD_TYPE_RECTANGLE)
        return SPA_RESULT_ENUM_END;

      if (!(p->body.flags & SPA_POD_PROP_FLAG_UNSET)) {
        const SpaRectangle *values = SPA_POD_BODY_CONST (&p->body.value);

        if (state->frmsize.index > 0)
          goto next_fmtdesc;

        state->frmsize.type = V4L2_FRMSIZE_TYPE_DISCRETE;
        state->frmsize.discrete.width = values[0].width;
        state->frmsize.discrete.height = values[0].height;
        goto have_size;
      }
    }
do_frmsize:
    if ((res = xioctl (state->fd, VIDIOC_ENUM_FRAMESIZES, &state->frmsize)) < 0) {
      if (errno == EINVAL)
        goto next_fmtdesc;

      perror ("VIDIOC_ENUM_FRAMESIZES");
      return SPA_RESULT_ENUM_END;
    }
    if (filter) {
      SpaPODProp *p;
      const SpaRectangle step = { 1, 1 }, *values;
      uint32_t range;
      unsigned int i, n_values;

      /* check if we have a fixed frame size */
      if (!(p = spa_format_find_prop (filter, SPA_PROP_ID_VIDEO_SIZE)))
        goto have_size;

      range = p->body.flags & SPA_POD_PROP_RANGE_MASK;
      values = SPA_POD_BODY_CONST (&p->body.value);
      n_values = SPA_POD_PROP_N_VALUES (p);

      if (range == SPA_POD_PROP_RANGE_MIN_MAX && n_values > 2) {
        if (filter_framesize (&state->frmsize, &values[1],
                                               &values[2],
                                               &step))
          goto have_size;
      } else if (range == SPA_POD_PROP_RANGE_STEP && n_values > 3) {
        if (filter_framesize (&state->frmsize, &values[1],
                                               &values[2],
                                               &values[3]))
          goto have_size;
      } else if (range == SPA_POD_PROP_RANGE_ENUM) {
        for (i = 1; i < n_values; i++) {
          if (filter_framesize (&state->frmsize, &values[i],
                                                 &values[i],
                                                 &step))
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
    }
    else if (state->frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS ||
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

  fmt = SPA_MEMBER (b.data,
                    spa_pod_builder_push_format (&b, &f[0],
                                                 info->media_type,
                                                 info->media_subtype),
                    SpaFormat);

  if (info->media_subtype == SPA_MEDIA_SUBTYPE_RAW) {
    spa_pod_builder_prop (&b,
        SPA_PROP_ID_VIDEO_FORMAT,  SPA_POD_TYPE_INT,
                                        info->format,
                                   SPA_POD_PROP_RANGE_NONE | SPA_POD_PROP_FLAG_READWRITE,
        0);
  }

  spa_pod_builder_prop (&b,
      SPA_PROP_ID_VIDEO_SIZE,  SPA_POD_TYPE_RECTANGLE,
                                      state->frmsize.discrete.width,
                                      state->frmsize.discrete.height,
                               SPA_POD_PROP_RANGE_NONE | SPA_POD_PROP_FLAG_READWRITE,
      0);

  prop = SPA_MEMBER (b.data,
                     spa_pod_builder_push_prop (&b, &f[1],
                                                SPA_PROP_ID_VIDEO_FRAMERATE,
                                                SPA_POD_PROP_RANGE_NONE |
                                                SPA_POD_PROP_FLAG_UNSET |
                                                SPA_POD_PROP_FLAG_READWRITE),
                     SpaPODProp);
  n_fractions = 0;

  state->frmival.index = 0;

  while (true) {
    if ((res = xioctl (state->fd, VIDIOC_ENUM_FRAMEINTERVALS, &state->frmival)) < 0) {
      if (errno == EINVAL) {
        state->frmsize.index++;
        state->next_frmsize = true;
        if (state->frmival.index == 0)
          goto next_frmsize;
        break;
      }
      perror ("VIDIOC_ENUM_FRAMEINTERVALS");
      return SPA_RESULT_ENUM_END;
    }
    if (filter) {
      SpaPODProp *p;
      uint32_t range;
      unsigned int i, n_values;
      const SpaFraction step = { 1, 1 }, *values;

      if (!(p = spa_format_find_prop (filter, SPA_PROP_ID_VIDEO_FRAMERATE)))
        goto have_framerate;

      if (p->body.value.type != SPA_POD_TYPE_FRACTION)
        return SPA_RESULT_ENUM_END;

      range = p->body.flags & SPA_POD_PROP_RANGE_MASK;
      values = SPA_POD_BODY_CONST (&p->body.value);
      n_values = SPA_POD_PROP_N_VALUES (p);

      if (!(p->body.flags & SPA_POD_PROP_FLAG_UNSET)) {
        if (filter_framerate (&state->frmival, &values[0],
                                               &values[0],
                                               &step))
          goto have_framerate;
      } else if (range == SPA_POD_PROP_RANGE_MIN_MAX && n_values > 2) {
        if (filter_framerate (&state->frmival, &values[1],
                                               &values[2],
                                               &step))
          goto have_framerate;
      } else if (range == SPA_POD_PROP_RANGE_STEP && n_values > 3) {
        if (filter_framerate (&state->frmival, &values[1],
                                               &values[2],
                                               &values[3]))
          goto have_framerate;
      } else if (range == SPA_POD_PROP_RANGE_ENUM) {
        for (i = 1; i < n_values; i++) {
          if (filter_framerate (&state->frmival, &values[i],
                                                 &values[i],
                                                 &step))
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
        spa_pod_builder_fraction (&b,
                                  state->frmival.discrete.denominator,
                                  state->frmival.discrete.numerator);
      spa_pod_builder_fraction (&b,
                                state->frmival.discrete.denominator,
                                state->frmival.discrete.numerator);
      state->frmival.index++;
    } else if (state->frmival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS ||
               state->frmival.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
      if (n_fractions == 0)
        spa_pod_builder_fraction (&b, 25, 1);
      spa_pod_builder_fraction (&b,
                                state->frmival.stepwise.min.denominator,
                                state->frmival.stepwise.min.numerator);
      spa_pod_builder_fraction (&b,
                                state->frmival.stepwise.max.denominator,
                                state->frmival.stepwise.max.numerator);

      if (state->frmival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
        prop->body.flags |= SPA_POD_PROP_RANGE_MIN_MAX;
      } else {
        prop->body.flags |= SPA_POD_PROP_RANGE_STEP;
        spa_pod_builder_fraction (&b,
                                  state->frmival.stepwise.step.denominator,
                                  state->frmival.stepwise.step.numerator);
      }
      break;
    }
    n_fractions++;
  }
  spa_pod_builder_pop (&b, &f[1]);
  spa_pod_builder_pop (&b, &f[0]);

  *format = fmt;

  return SPA_RESULT_OK;
}

static int
spa_v4l2_set_format (SpaV4l2Source *this, SpaVideoInfo *f, bool try_only)
{
  SpaV4l2State *state = &this->state[0];
  int cmd;
  struct v4l2_format reqfmt, fmt;
  struct v4l2_streamparm streamparm;
  const FormatInfo *info = NULL;
  SpaVideoFormat video_format;
  SpaRectangle *size = NULL;
  SpaFraction *framerate = NULL;

  CLEAR (fmt);
  CLEAR (streamparm);
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  switch (f->media_subtype) {
    case SPA_MEDIA_SUBTYPE_RAW:
      video_format = f->info.raw.format;
      size = &f->info.raw.size;
      framerate = &f->info.raw.framerate;
      break;
    case SPA_MEDIA_SUBTYPE_MJPG:
    case SPA_MEDIA_SUBTYPE_JPEG:
      video_format = SPA_VIDEO_FORMAT_ENCODED;
      size = &f->info.mjpg.size;
      framerate = &f->info.mjpg.framerate;
      break;
    case SPA_MEDIA_SUBTYPE_H264:
      video_format = SPA_VIDEO_FORMAT_ENCODED;
      size = &f->info.h264.size;
      framerate = &f->info.h264.framerate;
      break;
    case SPA_MEDIA_SUBTYPE_DV:
    case SPA_MEDIA_SUBTYPE_MPEGTS:
    case SPA_MEDIA_SUBTYPE_MPEG1:
    case SPA_MEDIA_SUBTYPE_MPEG2:
    case SPA_MEDIA_SUBTYPE_MPEG4:
    case SPA_MEDIA_SUBTYPE_XVID:
    case SPA_MEDIA_SUBTYPE_VC1:
    case SPA_MEDIA_SUBTYPE_VP8:
    default:
      video_format = SPA_VIDEO_FORMAT_ENCODED;
      break;
  }

  info = find_format_info_by_media_type (f->media_type,
                                         f->media_subtype,
                                         video_format,
                                         0);
  if (info == NULL || size == NULL || framerate == NULL) {
    spa_log_error (state->log, "v4l2: unknown media type %d %d %d", f->media_type,
        f->media_subtype, video_format);
    return -1;
  }


  fmt.fmt.pix.pixelformat = info->fourcc;
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  fmt.fmt.pix.width = size->width;
  fmt.fmt.pix.height = size->height;
  streamparm.parm.capture.timeperframe.numerator = framerate->denom;
  streamparm.parm.capture.timeperframe.denominator = framerate->num;

  spa_log_info (state->log, "v4l2: set %08x %dx%d %d/%d", fmt.fmt.pix.pixelformat,
      fmt.fmt.pix.width, fmt.fmt.pix.height,
      streamparm.parm.capture.timeperframe.denominator,
      streamparm.parm.capture.timeperframe.numerator);

  reqfmt = fmt;

  if (spa_v4l2_open (this) < 0)
    return -1;

  cmd = try_only ? VIDIOC_TRY_FMT : VIDIOC_S_FMT;
  if (xioctl (state->fd, cmd, &fmt) < 0) {
    perror ("VIDIOC_S_FMT");
    return -1;
  }

  /* some cheap USB cam's won't accept any change */
  if (xioctl (state->fd, VIDIOC_S_PARM, &streamparm) < 0)
    perror ("VIDIOC_S_PARM");

  spa_log_info (state->log, "v4l2: got %08x %dx%d %d/%d", fmt.fmt.pix.pixelformat,
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
  state->info.flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS |
                      SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                      SPA_PORT_INFO_FLAG_LIVE;
  state->info.maxbuffering = -1;
  state->info.latency = (streamparm.parm.capture.timeperframe.numerator * SPA_NSEC_PER_SEC) /
                        streamparm.parm.capture.timeperframe.denominator;

  state->info.n_params = 2;
  state->info.params = state->params;
  state->params[0] = &state->param_buffers.param;
  state->param_buffers.param.type = SPA_ALLOC_PARAM_TYPE_BUFFERS;
  state->param_buffers.param.size = sizeof (state->param_buffers);
  state->param_buffers.minsize = fmt.fmt.pix.sizeimage;
  state->param_buffers.stride = fmt.fmt.pix.bytesperline;
  state->param_buffers.min_buffers = 2;
  state->param_buffers.max_buffers = MAX_BUFFERS;
  state->param_buffers.align = 16;
  state->params[1] = &state->param_meta.param;
  state->param_meta.param.type = SPA_ALLOC_PARAM_TYPE_META_ENABLE;
  state->param_meta.param.size = sizeof (state->param_meta);
  state->param_meta.type = SPA_META_TYPE_HEADER;

  state->info.extra = NULL;

  return 0;
}

static SpaResult
mmap_read (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state[0];
  struct v4l2_buffer buf;
  V4l2Buffer *b;
  SpaData *d;
  int64_t pts;
  SpaPortOutput *output;

  CLEAR(buf);
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = state->memtype;

  if (xioctl (state->fd, VIDIOC_DQBUF, &buf) < 0) {
    switch (errno) {
      case EAGAIN:
        return SPA_RESULT_ERROR;
      case EIO:
      default:
        perror ("VIDIOC_DQBUF");
        return SPA_RESULT_ERROR;
    }
  }

  state->last_ticks = (int64_t)buf.timestamp.tv_sec * SPA_USEC_PER_SEC + (uint64_t)buf.timestamp.tv_usec;
  pts = state->last_ticks * 1000;

  if (buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
    state->last_monotonic = pts;
  else
    state->last_monotonic = SPA_TIME_INVALID;

  b = &state->buffers[buf.index];
  if (b->h) {
    b->h->flags = SPA_BUFFER_FLAG_NONE;
    if (buf.flags & V4L2_BUF_FLAG_ERROR)
      b->h->flags |= SPA_BUFFER_FLAG_CORRUPTED;
    b->h->seq = buf.sequence;
    b->h->pts = pts;
  }

  d = b->outbuf->datas;
  d[0].chunk->offset = 0;
  d[0].chunk->size = buf.bytesused;
  d[0].chunk->stride = state->fmt.fmt.pix.bytesperline;

  if ((output = state->io)) {
    b->outstanding = true;
    output->buffer_id = b->outbuf->id;
    output->status = SPA_RESULT_OK;
  }
  return SPA_RESULT_OK;
}

static void
v4l2_on_fd_events (SpaSource *source)
{
  SpaV4l2Source *this = source->data;
  SpaNodeEvent event;

  if (source->rmask & SPA_IO_ERR)
    return;

  if (!(source->rmask & SPA_IO_IN))
    return;

  if (mmap_read (this) < 0)
    return;

  event.type = SPA_NODE_EVENT_TYPE_HAVE_OUTPUT;
  event.size = sizeof (event);
  this->event_cb (&this->node, &event, this->user_data);
}

static SpaResult
spa_v4l2_use_buffers (SpaV4l2Source *this, SpaBuffer **buffers, uint32_t n_buffers)
{
  SpaV4l2State *state = &this->state[0];
  struct v4l2_requestbuffers reqbuf;
  int i;
  SpaData *d;

  if (n_buffers > 0) {
    switch (buffers[0]->datas[0].type) {
      case SPA_DATA_TYPE_MEMPTR:
      case SPA_DATA_TYPE_MEMFD:
        state->memtype = V4L2_MEMORY_USERPTR;
        break;
      case SPA_DATA_TYPE_DMABUF:
        state->memtype = V4L2_MEMORY_DMABUF;
        break;
      default:
        spa_log_error (state->log, "v4l2: can't use buffers");
        return SPA_RESULT_ERROR;
    }
  }

  CLEAR(reqbuf);
  reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  reqbuf.memory = state->memtype;
  reqbuf.count = n_buffers;

  if (xioctl (state->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
    perror ("VIDIOC_REQBUFS");
    return SPA_RESULT_ERROR;
  }
  spa_log_info (state->log, "v4l2: got %d buffers", reqbuf.count);
  if (reqbuf.count < 2) {
    spa_log_error (state->log, "v4l2: can't allocate enough buffers");
    return SPA_RESULT_ERROR;
  }

  for (i = 0; i < reqbuf.count; i++) {
    V4l2Buffer *b;

    b = &state->buffers[i];
    b->outbuf = buffers[i];
    b->outstanding = true;
    b->allocated = false;
    b->h = spa_buffer_find_meta (b->outbuf, SPA_META_TYPE_HEADER);

    spa_log_info (state->log, "v4l2: import buffer %p", buffers[i]);

    if (buffers[i]->n_datas < 1) {
      spa_log_error (state->log, "v4l2: invalid memory on buffer %p", buffers[i]);
      continue;
    }
    d = buffers[i]->datas;

    CLEAR (b->v4l2_buffer);
    b->v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b->v4l2_buffer.memory = state->memtype;
    b->v4l2_buffer.index = i;
    switch (d[0].type) {
      case SPA_DATA_TYPE_MEMPTR:
      case SPA_DATA_TYPE_MEMFD:
        if (d[0].data == NULL) {
          spa_log_error (state->log, "v4l2: need mmaped memory");
          continue;
        }
        b->v4l2_buffer.m.userptr = (unsigned long) d[0].data;
        b->v4l2_buffer.length = d[0].maxsize;
        break;
      case SPA_DATA_TYPE_DMABUF:
        b->v4l2_buffer.m.fd = d[0].fd;
        break;
      default:
        break;
    }
    spa_v4l2_buffer_recycle (this, buffers[i]->id);
  }
  state->n_buffers = reqbuf.count;

  return SPA_RESULT_OK;
}

static SpaResult
mmap_init (SpaV4l2Source   *this,
           SpaAllocParam  **params,
           unsigned int     n_params,
           SpaBuffer      **buffers,
           unsigned int    *n_buffers)
{
  SpaV4l2State *state = &this->state[0];
  struct v4l2_requestbuffers reqbuf;
  int i;

  state->memtype = V4L2_MEMORY_MMAP;

  CLEAR(reqbuf);
  reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  reqbuf.memory = state->memtype;
  reqbuf.count = *n_buffers;

  if (xioctl (state->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
    perror ("VIDIOC_REQBUFS");
    return SPA_RESULT_ERROR;
  }

  spa_log_info (state->log, "v4l2: got %d buffers", reqbuf.count);
  *n_buffers = reqbuf.count;

  if (reqbuf.count < 2) {
    spa_log_error (state->log, "v4l2: can't allocate enough buffers");
    return SPA_RESULT_ERROR;
  }
  if (state->export_buf)
    spa_log_info (state->log, "v4l2: using EXPBUF");

  for (i = 0; i < reqbuf.count; i++) {
    V4l2Buffer *b;
    SpaData *d;

    if (buffers[i]->n_datas < 1) {
      spa_log_error (state->log, "v4l2: invalid buffer data");
      return SPA_RESULT_ERROR;
    }

    b = &state->buffers[i];
    b->outbuf = buffers[i];
    b->outstanding = true;
    b->allocated = true;
    b->h = spa_buffer_find_meta (b->outbuf, SPA_META_TYPE_HEADER);

    CLEAR (b->v4l2_buffer);
    b->v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b->v4l2_buffer.memory = state->memtype;
    b->v4l2_buffer.index = i;

    if (xioctl (state->fd, VIDIOC_QUERYBUF, &b->v4l2_buffer) < 0) {
      perror ("VIDIOC_QUERYBUF");
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

      CLEAR (expbuf);
      expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      expbuf.index = i;
      expbuf.flags = O_CLOEXEC | O_RDONLY;
      if (xioctl (state->fd, VIDIOC_EXPBUF, &expbuf) < 0) {
        perror("VIDIOC_EXPBUF");
        continue;
      }
      d[0].type = SPA_DATA_TYPE_DMABUF;
      d[0].fd = expbuf.fd;
      d[0].data = NULL;
    } else {
      d[0].type = SPA_DATA_TYPE_MEMPTR;
      d[0].fd = -1;
      d[0].data = mmap (NULL,
                        b->v4l2_buffer.length,
                        PROT_READ,
                        MAP_SHARED,
                        state->fd,
                        b->v4l2_buffer.m.offset);
      if (d[0].data == MAP_FAILED) {
        perror ("mmap");
        continue;
      }
    }
    spa_v4l2_buffer_recycle (this, i);
  }
  state->n_buffers = reqbuf.count;

  return SPA_RESULT_OK;
}

static SpaResult
userptr_init (SpaV4l2Source *this)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
read_init (SpaV4l2Source *this)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_alloc_buffers (SpaV4l2Source   *this,
                        SpaAllocParam  **params,
                        unsigned int     n_params,
                        SpaBuffer      **buffers,
                        unsigned int    *n_buffers)
{
  SpaResult res;
  SpaV4l2State *state = &this->state[0];

  if (state->n_buffers > 0)
    return SPA_RESULT_ERROR;

  if (state->cap.capabilities & V4L2_CAP_STREAMING) {
    if ((res = mmap_init (this, params, n_params, buffers, n_buffers)) < 0)
      if ((res = userptr_init (this)) < 0)
        return res;
  } else if (state->cap.capabilities & V4L2_CAP_READWRITE) {
    if ((res = read_init (this)) < 0)
      return res;
  } else
    return SPA_RESULT_ERROR;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_stream_on (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state[0];
  enum v4l2_buf_type type;

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl (state->fd, VIDIOC_STREAMON, &type) < 0) {
    spa_log_error (this->log, "VIDIOC_STREAMON: %s", strerror (errno));
    return SPA_RESULT_ERROR;
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_port_set_enabled (SpaV4l2Source *this, bool enabled)
{
  SpaV4l2State *state = &this->state[0];
  if (state->source_enabled != enabled) {
    state->source_enabled = enabled;
    if (enabled)
      spa_loop_add_source (state->data_loop, &state->source);
    else
      spa_loop_remove_source (state->data_loop, &state->source);
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_stream_off (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state[0];
  enum v4l2_buf_type type;
  int i;

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl (state->fd, VIDIOC_STREAMOFF, &type) < 0) {
    spa_log_error (this->log, "VIDIOC_STREAMOFF: %s", strerror (errno));
    return SPA_RESULT_ERROR;
  }
  for (i = 0; i < state->n_buffers; i++) {
    V4l2Buffer *b;

    b = &state->buffers[i];
    if (!b->outstanding)
      if (xioctl (state->fd, VIDIOC_QBUF, &b->v4l2_buffer) < 0)
        spa_log_warn (this->log, "VIDIOC_QBUF: %s", strerror (errno));
  }

  return SPA_RESULT_OK;
}
