#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))

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
  SpaV4l2SourceProps *props = &this->props[1];

  if (state->opened)
    return 0;

  fprintf (stderr, "Playback device is '%s'\n", props->device);

  if (stat (props->device, &st) < 0) {
    fprintf(stderr, "Cannot identify '%s': %d, %s\n",
            props->device, errno, strerror (errno));
    return -1;
  }

  if (!S_ISCHR (st.st_mode)) {
    fprintf(stderr, "%s is no device\n", props->device);
    return -1;
  }

  state->fd = open (props->device, O_RDWR | O_NONBLOCK, 0);

  if (state->fd == -1) {
    fprintf (stderr, "Cannot open '%s': %d, %s\n",
            props->device, errno, strerror (errno));
    return -1;
  }

  if (xioctl (state->fd, VIDIOC_QUERYCAP, &state->cap) < 0) {
    perror ("QUERYCAP");
    return -1;
  }

  if ((state->cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
    fprintf (stderr, "%s is no video capture device\n", props->device);
    return -1;
  }
  state->opened = true;

  return 0;
}

static SpaResult
spa_v4l2_buffer_recycle (SpaV4l2Source *this, uint32_t buffer_id)
{
  SpaV4l2State *state = &this->state[0];
  V4l2Buffer *b = &state->alloc_buffers[buffer_id];

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
  int i;

  if (!state->have_buffers)
    return SPA_RESULT_OK;

  for (i = 0; i < state->reqbuf.count; i++) {
    V4l2Buffer *b;

    b = &state->alloc_buffers[i];
    if (b->outstanding) {
      fprintf (stderr, "queueing outstanding buffer %p\n", b);
      spa_v4l2_buffer_recycle (this, i);
    }
    if (b->buffer.n_datas > 0)
      spa_memory_unref (&b->datas[0].mem.mem);
  }
  if (state->alloc_mem)
    spa_memory_unref (&state->alloc_mem->mem);

  state->have_buffers = false;

  return SPA_RESULT_OK;
}

static int
spa_v4l2_close (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state[0];

  if (!state->opened)
    return 0;

  if (state->have_buffers)
    return 0;

  fprintf (stderr, "close\n");
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

  if ((filter->media_type == SPA_MEDIA_TYPE_VIDEO || filter->media_type == SPA_MEDIA_TYPE_IMAGE)) {
    if (filter->media_subtype == SPA_MEDIA_SUBTYPE_RAW) {
      SpaPropValue val;
      SpaResult res;
      unsigned int idx;
      const SpaPropInfo *pi;

      idx = spa_props_index_for_id (&filter->props, SPA_PROP_ID_VIDEO_FORMAT);
      if (idx == SPA_IDX_INVALID)
        return SPA_VIDEO_FORMAT_UNKNOWN;

      pi = &filter->props.prop_info[idx];
      if (pi->type != SPA_PROP_TYPE_UINT32)
        return SPA_VIDEO_FORMAT_UNKNOWN;

      res = spa_props_get_prop (&filter->props, idx, &val);
      if (res >= 0) {
        if (index == 0)
          video_format = *((SpaVideoFormat *)val.value);
      } else if (res == SPA_RESULT_PROPERTY_UNSET) {

        if (index < pi->n_range_values)
          video_format = *((SpaVideoFormat *)pi->range_values[index].val.value);
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
spa_v4l2_enum_format (SpaV4l2Source *this, SpaFormat **format, const SpaFormat *filter, void **cookie)
{
  SpaV4l2State *state = &this->state[0];
  int res, i, pi;
  V4l2Format *fmt;
  const FormatInfo *info;

  if (spa_v4l2_open (this) < 0)
    return SPA_RESULT_ERROR;

  *format = NULL;

  if (*cookie == NULL) {
    CLEAR (state->fmtdesc);
    state->fmtdesc.index = 0;
    state->fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    state->next_fmtdesc = true;
    CLEAR (state->frmsize);
    state->next_frmsize = true;
    CLEAR (state->frmival);
    *cookie = state;
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

      info = find_format_info_by_media_type (filter->media_type,
                                             filter->media_subtype,
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
      const SpaPropInfo *pi;
      unsigned int idx;
      SpaPropValue val;
      SpaResult res;

      /* check if we have a fixed frame size */
      idx = spa_props_index_for_id (&filter->props, SPA_PROP_ID_VIDEO_SIZE);
      if (idx == SPA_IDX_INVALID)
        goto do_frmsize;

      pi = &filter->props.prop_info[idx];
      if (pi->type != SPA_PROP_TYPE_RECTANGLE)
        return SPA_RESULT_ENUM_END;

      res = spa_props_get_prop (&filter->props, idx, &val);
      if (res >= 0) {
        const SpaRectangle *size = val.value;

        if (state->frmsize.index > 0)
          goto next_fmtdesc;

        state->frmsize.type = V4L2_FRMSIZE_TYPE_DISCRETE;
        state->frmsize.discrete.width = size->width;
        state->frmsize.discrete.height = size->height;
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
      const SpaPropInfo *pi;
      unsigned int idx;
      const SpaRectangle step = { 1, 1 };

      /* check if we have a fixed frame size */
      idx = spa_props_index_for_id (&filter->props, SPA_PROP_ID_VIDEO_SIZE);
      if (idx == SPA_IDX_INVALID)
        goto have_size;

      /* checked above */
      pi = &filter->props.prop_info[idx];

      if (pi->range_type == SPA_PROP_RANGE_TYPE_MIN_MAX) {
        if (filter_framesize (&state->frmsize, pi->range_values[0].val.value,
                                               pi->range_values[1].val.value,
                                               &step))
          goto have_size;
      } else if (pi->range_type == SPA_PROP_RANGE_TYPE_STEP) {
        if (filter_framesize (&state->frmsize, pi->range_values[0].val.value,
                                               pi->range_values[1].val.value,
                                               pi->range_values[2].val.value))
          goto have_size;
      } else if (pi->range_type == SPA_PROP_RANGE_TYPE_ENUM) {
        unsigned int i;
        for (i = 0; i < pi->n_range_values; i++) {
          if (filter_framesize (&state->frmsize, pi->range_values[i].val.value,
                                                 pi->range_values[i].val.value,
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

  fmt = &state->format[0];
  fmt->fmt.media_type = info->media_type;
  fmt->fmt.media_subtype = info->media_subtype;
  fmt->fmt.props.prop_info = fmt->infos;
  fmt->fmt.props.n_prop_info = pi = 0;
  fmt->fmt.props.unset_mask = 0;
  fmt->fmt.mem.mem.pool_id = SPA_ID_INVALID;

  if (info->media_subtype == SPA_MEDIA_SUBTYPE_RAW) {
    spa_prop_info_fill_video (&fmt->infos[pi],
                              SPA_PROP_ID_VIDEO_FORMAT,
                              offsetof (V4l2Format, format));
    fmt->format = info->format;
    pi = ++fmt->fmt.props.n_prop_info;
  } else {
    fmt->format = info->format;
  }

  spa_prop_info_fill_video (&fmt->infos[pi],
                            SPA_PROP_ID_VIDEO_SIZE,
                            offsetof (V4l2Format, size));
  fmt->size.width = state->frmsize.discrete.width;
  fmt->size.height = state->frmsize.discrete.height;
  pi = ++fmt->fmt.props.n_prop_info;

  spa_prop_info_fill_video (&fmt->infos[pi],
                            SPA_PROP_ID_VIDEO_FRAMERATE,
                            offsetof (V4l2Format, framerate));
  fmt->infos[pi].range_values = fmt->ranges;
  fmt->infos[pi].n_range_values = 0;
  i = state->frmival.index = 0;

  while (true) {
    if ((res = xioctl (state->fd, VIDIOC_ENUM_FRAMEINTERVALS, &state->frmival)) < 0) {
      if (errno == EINVAL) {
        state->frmsize.index++;
        state->next_frmsize = true;
        if (i == 0)
          goto next_frmsize;
        break;
      }
      perror ("VIDIOC_ENUM_FRAMEINTERVALS");
      return SPA_RESULT_ENUM_END;
    }
    if (filter) {
      SpaPropValue val;
      const SpaPropInfo *pi;
      unsigned int idx;
      SpaResult res;
      const SpaFraction step = { 1, 1 };

      /* check against filter */
      idx = spa_props_index_for_id (&filter->props, SPA_PROP_ID_VIDEO_FRAMERATE);
      if (idx == SPA_IDX_INVALID)
        goto have_framerate;

      pi = &filter->props.prop_info[idx];
      if (pi->type != SPA_PROP_TYPE_FRACTION)
        return SPA_RESULT_ENUM_END;

      res = spa_props_get_prop (&filter->props, idx, &val);
      if (res == 0) {
        if (filter_framerate (&state->frmival, val.value,
                                               val.value,
                                               &step))
          goto have_framerate;
      } else if (pi->range_type == SPA_PROP_RANGE_TYPE_MIN_MAX) {
        if (filter_framerate (&state->frmival, pi->range_values[0].val.value,
                                               pi->range_values[1].val.value,
                                               &step))
          goto have_framerate;
      } else if (pi->range_type == SPA_PROP_RANGE_TYPE_STEP) {
        if (filter_framerate (&state->frmival, pi->range_values[0].val.value,
                                               pi->range_values[1].val.value,
                                               pi->range_values[2].val.value))
          goto have_framerate;
      } else if (pi->range_type == SPA_PROP_RANGE_TYPE_ENUM) {
        unsigned int i;
        for (i = 0; i < pi->n_range_values; i++) {
          if (filter_framerate (&state->frmival, pi->range_values[i].val.value,
                                                 pi->range_values[i].val.value,
                                                 &step))
            goto have_framerate;
        }
      }
      state->frmival.index++;
      continue;
    }

have_framerate:
    fmt->ranges[i].name = NULL;
    fmt->ranges[i].description = NULL;
    if (state->frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
      fmt->infos[pi].range_type = SPA_PROP_RANGE_TYPE_ENUM;
      fmt->framerates[i].num = state->frmival.discrete.denominator;
      fmt->framerates[i].denom = state->frmival.discrete.numerator;
      fmt->ranges[i].val.size = sizeof (SpaFraction);
      fmt->ranges[i].val.value = &fmt->framerates[i];
      i++;
      state->frmival.index++;
      if (i == 16)
        break;
    } else if (state->frmival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS ||
               state->frmival.type == V4L2_FRMIVAL_TYPE_STEPWISE) {
      fmt->framerates[0].num = state->frmival.stepwise.min.denominator;
      fmt->framerates[0].denom = state->frmival.stepwise.min.numerator;
      fmt->ranges[0].val.size = sizeof (SpaFraction);
      fmt->ranges[0].val.value = &fmt->framerates[0];
      fmt->framerates[1].num = state->frmival.stepwise.max.denominator;
      fmt->framerates[1].denom = state->frmival.stepwise.max.numerator;
      fmt->ranges[1].val.size = sizeof (SpaFraction);
      fmt->ranges[1].val.value = &fmt->framerates[1];
      if (state->frmival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
        fmt->infos[pi].range_type = SPA_PROP_RANGE_TYPE_MIN_MAX;
        i = 2;
      } else {
        fmt->infos[pi].range_type = SPA_PROP_RANGE_TYPE_STEP;
        fmt->framerates[2].num = state->frmival.stepwise.step.denominator;
        fmt->framerates[2].denom = state->frmival.stepwise.step.numerator;
        fmt->ranges[2].val.size = sizeof (SpaFraction);
        fmt->ranges[2].val.value = &fmt->framerates[2];
        i = 3;
      }
      break;
    }
  }
  fmt->infos[pi].n_range_values = i;
  fmt->framerate = fmt->framerates[0];
  if (i > 1) {
    SPA_PROPS_INDEX_UNSET (&fmt->fmt.props, pi);
  }
  pi = ++fmt->fmt.props.n_prop_info;

  *format = &state->format[0].fmt;

  return SPA_RESULT_OK;
}

static int
spa_v4l2_set_format (SpaV4l2Source *this, V4l2Format *f, bool try_only)
{
  SpaV4l2State *state = &this->state[0];
  int cmd;
  struct v4l2_format reqfmt, fmt;
  struct v4l2_streamparm streamparm;
  const FormatInfo *info = NULL;

  CLEAR (fmt);
  CLEAR (streamparm);
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  info = find_format_info_by_media_type (f->fmt.media_type,
                                         f->fmt.media_subtype,
                                         f->format,
                                         0);
  if (info == NULL) {
    fprintf (stderr, "unknown media type %d %d %d\n", f->fmt.media_type,
        f->fmt.media_subtype, f->format);
    return -1;
  }

  fmt.fmt.pix.pixelformat = info->fourcc;
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  fmt.fmt.pix.width = f->size.width;
  fmt.fmt.pix.height = f->size.height;
  streamparm.parm.capture.timeperframe.numerator = f->framerate.denom;
  streamparm.parm.capture.timeperframe.denominator = f->framerate.num;

  fprintf (stderr, "set %08x %dx%d %d/%d\n", fmt.fmt.pix.pixelformat,
      fmt.fmt.pix.width, fmt.fmt.pix.height,
      streamparm.parm.capture.timeperframe.numerator,
      streamparm.parm.capture.timeperframe.denominator);

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

  fprintf (stderr, "got %08x %dx%d %d/%d\n", fmt.fmt.pix.pixelformat,
      fmt.fmt.pix.width, fmt.fmt.pix.height,
      streamparm.parm.capture.timeperframe.numerator,
      streamparm.parm.capture.timeperframe.denominator);

  if (reqfmt.fmt.pix.pixelformat != fmt.fmt.pix.pixelformat ||
      reqfmt.fmt.pix.width != fmt.fmt.pix.width ||
      reqfmt.fmt.pix.height != fmt.fmt.pix.height)
    return -1;

  if (try_only)
    return 0;

  state->fmt = fmt;
  state->info.flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS |
                      SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
  state->info.maxbuffering = -1;
  state->info.latency = (streamparm.parm.capture.timeperframe.numerator * 1000000000LL) /
                        streamparm.parm.capture.timeperframe.denominator;

  state->info.n_params = 1;
  state->info.params = state->params;
  state->params[0] = &state->param_buffers.param;
  state->param_buffers.param.type = SPA_ALLOC_PARAM_TYPE_BUFFERS;
  state->param_buffers.param.size = sizeof (state->param_buffers);
  state->param_buffers.minsize = fmt.fmt.pix.sizeimage;
  state->param_buffers.stride = fmt.fmt.pix.bytesperline;
  state->param_buffers.min_buffers = 2;
  state->param_buffers.max_buffers = MAX_BUFFERS;
  state->param_buffers.align = 16;
  state->info.features = NULL;

  return 0;
}

static SpaResult
mmap_read (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state[0];
  struct v4l2_buffer buf;
  V4l2Buffer *b;
  SpaData *d;

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

  b = &state->alloc_buffers[buf.index];
  b->header.flags = SPA_BUFFER_FLAG_NONE;
  if (buf.flags & V4L2_BUF_FLAG_ERROR)
    b->header.flags |= SPA_BUFFER_FLAG_CORRUPTED;

  state->last_ticks = (int64_t)buf.timestamp.tv_sec * 1000000 + (uint64_t)buf.timestamp.tv_usec;

  b->header.seq = buf.sequence;
  b->header.pts = state->last_ticks * 1000;

  if (buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
    state->last_monotonic = b->header.pts;
  else
    state->last_monotonic = SPA_TIME_INVALID;

  d = SPA_BUFFER_DATAS (b->outbuf);
  d[0].mem.size = buf.bytesused;

  b->next = state->ready;
  state->ready = b;
  state->ready_count++;

  return SPA_RESULT_OK;
}

static int
v4l2_on_fd_events (SpaPollNotifyData *data)
{
  SpaV4l2Source *this = data->user_data;
  SpaNodeEvent event;
  SpaNodeEventHaveOutput ho;

  if (data->fds[0].revents & POLLERR)
    return -1;

  if (mmap_read (this) < 0)
    return 0;

  event.type = SPA_NODE_EVENT_TYPE_HAVE_OUTPUT;
  event.size = sizeof (ho);
  event.data = &ho;
  ho.port_id = 0;
  this->event_cb (&this->node, &event, this->user_data);

  return 0;
}

static SpaResult
spa_v4l2_use_buffers (SpaV4l2Source *this, SpaBuffer **buffers, uint32_t n_buffers)
{
  SpaV4l2State *state = &this->state[0];
  struct v4l2_requestbuffers reqbuf;
  int i;

  state->memtype = V4L2_MEMORY_USERPTR;

  CLEAR(reqbuf);
  reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  reqbuf.memory = state->memtype;
  reqbuf.count = n_buffers;

  if (xioctl (state->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
    perror ("VIDIOC_REQBUFS");
    return SPA_RESULT_ERROR;
  }
  fprintf (stderr, "got %d buffers\n", reqbuf.count);
  if (reqbuf.count < 2) {
    fprintf (stderr, "can't allocate enough buffers\n");
    return SPA_RESULT_ERROR;
  }
  state->reqbuf = reqbuf;

  if (state->alloc_mem)
    spa_memory_unref (&state->alloc_mem->mem);
  state->alloc_mem = spa_memory_alloc_size (SPA_MEMORY_POOL_LOCAL,
                                            NULL,
                                            sizeof (V4l2Buffer) * reqbuf.count);
  state->alloc_buffers = spa_memory_ensure_ptr (state->alloc_mem);

  for (i = 0; i < reqbuf.count; i++) {
    V4l2Buffer *b;
    SpaMemoryRef *mem_ref;
    SpaMemory *mem;
    SpaData *d;

    b = &state->alloc_buffers[i];
    b->buffer.mem.mem = state->alloc_mem->mem;
    b->buffer.mem.offset = sizeof (V4l2Buffer) * i;
    b->buffer.mem.size = sizeof (V4l2Buffer);
    b->buffer.id = SPA_ID_INVALID;
    b->buffer.n_metas = 0;
    b->buffer.n_datas = 0;
    b->outbuf = buffers[i];
    b->outstanding = true;

    fprintf (stderr, "import buffer %p\n", buffers[i]);

    d = SPA_BUFFER_DATAS (buffers[i]);
    mem_ref = &d[0].mem.mem;
    if (!(mem = spa_memory_find (mem_ref))) {
      fprintf (stderr, "invalid memory on buffer %p\n", buffers[i]);
      continue;
    }

    if (buffers[i]->n_datas < 1) {
      fprintf (stderr, "invalid memory on buffer %p\n", buffers[i]);
      continue;
    }

    CLEAR (b->v4l2_buffer);
    b->v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b->v4l2_buffer.memory = state->memtype;
    b->v4l2_buffer.index = i;
    b->v4l2_buffer.m.userptr = (unsigned long) SPA_MEMBER (mem->ptr, d[0].mem.offset, void *);
    b->v4l2_buffer.length = d[0].mem.size;

    spa_v4l2_buffer_recycle (this, buffers[i]->id);
  }
  state->have_buffers = true;

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

  fprintf (stderr, "got %d buffers\n", reqbuf.count);
  *n_buffers = reqbuf.count;

  if (reqbuf.count < 2) {
    fprintf (stderr, "can't allocate enough buffers\n");
    return SPA_RESULT_ERROR;
  }
  if (state->export_buf)
    fprintf (stderr, "using EXPBUF\n");

  state->reqbuf = reqbuf;

  if (state->alloc_mem)
    spa_memory_unref (&state->alloc_mem->mem);
  state->alloc_mem = spa_memory_alloc_with_fd (SPA_MEMORY_POOL_SHARED,
                                               NULL,
                                               sizeof (V4l2Buffer) * reqbuf.count);
  state->alloc_buffers = spa_memory_ensure_ptr (state->alloc_mem);

  for (i = 0; i < reqbuf.count; i++) {
    struct v4l2_buffer buf;
    V4l2Buffer *b;
    SpaMemory *mem;

    CLEAR (buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = state->memtype;
    buf.index = i;

    if (xioctl (state->fd, VIDIOC_QUERYBUF, &buf) < 0) {
      perror ("VIDIOC_QUERYBUF");
      return SPA_RESULT_ERROR;
    }

    b = &state->alloc_buffers[i];
    b->buffer.id = i;
    b->buffer.mem.mem = state->alloc_mem->mem;
    b->buffer.mem.offset = sizeof (V4l2Buffer) * i;
    b->buffer.mem.size = sizeof (V4l2Buffer);

    buffers[i] = &b->buffer;

    b->buffer.n_metas = 1;
    b->buffer.metas = offsetof (V4l2Buffer, metas);
    b->buffer.n_datas = 1;
    b->buffer.datas = offsetof (V4l2Buffer, datas);

    b->header.flags = 0;
    b->header.seq = 0;
    b->header.pts = 0;
    b->header.dts_offset = 0;

    b->metas[0].type = SPA_META_TYPE_HEADER;
    b->metas[0].offset = offsetof (V4l2Buffer, header);
    b->metas[0].size = sizeof (b->header);

    mem = spa_memory_alloc (SPA_MEMORY_POOL_SHARED);
    mem->flags = SPA_MEMORY_FLAG_READABLE;
    mem->size = buf.length;
    b->datas[0].mem.mem = mem->mem;
    b->datas[0].mem.offset = 0;
    b->datas[0].mem.size = buf.length;
    b->datas[0].stride = state->fmt.fmt.pix.bytesperline;

    if (state->export_buf) {
      struct v4l2_exportbuffer expbuf;

      CLEAR (expbuf);
      expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      expbuf.index = i;
      if (xioctl (state->fd, VIDIOC_EXPBUF, &expbuf) < 0) {
        perror("VIDIOC_EXPBUF");
        continue;
      }

      mem->fd = expbuf.fd;
      mem->type = "dmabuf";
      mem->ptr = NULL;
      b->dmafd = expbuf.fd;
    } else {
      mem->fd = -1;
      mem->type = "sysmem";
      mem->ptr = mmap (NULL,
                       buf.length,
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED,
                       state->fd,
                       buf.m.offset);
      if (mem->ptr == MAP_FAILED) {
        perror ("mmap");
        continue;
      }
    }
    b->outbuf = &b->buffer;
    b->outstanding = true;

    CLEAR (b->v4l2_buffer);
    b->v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b->v4l2_buffer.memory = state->memtype;
    b->v4l2_buffer.index = i;

    spa_v4l2_buffer_recycle (this, i);
  }
  state->have_buffers = true;

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
  unsigned int i;

  if (state->have_buffers) {
    if (*n_buffers < state->reqbuf.count)
      return SPA_RESULT_NO_BUFFERS;

    *n_buffers = state->reqbuf.count;
    for (i = 0; i < state->reqbuf.count; i++) {
      buffers[i] = &state->alloc_buffers[i].buffer;
    }
    return SPA_RESULT_OK;
  }

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
spa_v4l2_start (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state[0];
  enum v4l2_buf_type type;
  SpaNodeEvent event;

  if (state->started)
    return SPA_RESULT_OK;

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl (state->fd, VIDIOC_STREAMON, &type) < 0) {
    perror ("VIDIOC_STREAMON");
    return SPA_RESULT_ERROR;
  }
  state->started = true;
  update_state (this, SPA_NODE_STATE_STREAMING);

  event.type = SPA_NODE_EVENT_TYPE_ADD_POLL;
  event.data = &state->poll;
  event.size = sizeof (state->poll);

  state->fds[0].fd = state->fd;
  state->fds[0].events = POLLIN | POLLPRI | POLLERR;
  state->fds[0].revents = 0;

  state->poll.id = 0;
  state->poll.enabled = true;
  state->poll.fds = state->fds;
  state->poll.n_fds = 1;
  state->poll.idle_cb = NULL;
  state->poll.before_cb = NULL;
  state->poll.after_cb = v4l2_on_fd_events;
  state->poll.user_data = this;
  this->event_cb (&this->node, &event, this->user_data);

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_pause (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state[0];
  enum v4l2_buf_type type;
  SpaNodeEvent event;
  int i;

  if (!state->started)
    return SPA_RESULT_OK;

  state->started = false;

  event.type = SPA_NODE_EVENT_TYPE_REMOVE_POLL;
  event.data = &state->poll;
  event.size = sizeof (state->poll);
  this->event_cb (&this->node, &event, this->user_data);

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl (state->fd, VIDIOC_STREAMOFF, &type) < 0) {
    perror ("VIDIOC_STREAMOFF");
    return SPA_RESULT_ERROR;
  }
  for (i = 0; i < state->reqbuf.count; i++) {
    V4l2Buffer *b;

    b = &state->alloc_buffers[i];
    if (!b->outstanding)
      if (xioctl (state->fd, VIDIOC_QBUF, &b->v4l2_buffer) < 0)
        perror ("VIDIOC_QBUF");
  }
  update_state (this, SPA_NODE_STATE_PAUSED);

  return SPA_RESULT_OK;
}
