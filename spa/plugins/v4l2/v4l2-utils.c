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
  {V4L2_PIX_FMT_MJPEG,         SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_MJPG },
  {V4L2_PIX_FMT_JPEG,          SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_IMAGE, SPA_MEDIA_SUBTYPE_JPEG },
  {V4L2_PIX_FMT_PJPG,          SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  {V4L2_PIX_FMT_DV,            SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_DV },
  {V4L2_PIX_FMT_MPEG,          SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_MPEGTS },
  {V4L2_PIX_FMT_H264,          SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_H264 },
  {V4L2_PIX_FMT_H264_NO_SC,    SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_H264 },
  {V4L2_PIX_FMT_H264_MVC,      SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_H264 },
  {V4L2_PIX_FMT_H263,          SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_H263 },
  {V4L2_PIX_FMT_MPEG1,         SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_MPEG1 },
  {V4L2_PIX_FMT_MPEG2,         SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_MPEG2 },
  {V4L2_PIX_FMT_MPEG4,         SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_MPEG4 },
  {V4L2_PIX_FMT_XVID,          SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_XVID },
  {V4L2_PIX_FMT_VC1_ANNEX_G,   SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_VC1 },
  {V4L2_PIX_FMT_VC1_ANNEX_L,   SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_VC1 },
  {V4L2_PIX_FMT_VP8,           SPA_VIDEO_FORMAT_ENCODED, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_VP8 },

  /*  Vendor-specific formats   */
  {V4L2_PIX_FMT_WNVA,          SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  {V4L2_PIX_FMT_SN9C10X,       SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  {V4L2_PIX_FMT_PWC1,          SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
  {V4L2_PIX_FMT_PWC2,          SPA_VIDEO_FORMAT_UNKNOWN, SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW },
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

#define FOURCC_ARGS(f) (f)&0x7f,((f)>>8)&0x7f,((f)>>16)&0x7f,((f)>>24)&0x7f

static SpaResult
spa_v4l2_enum_format (SpaV4l2Source *this, SpaFormat **format, void **cookie)
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

again:
  if (state->next_fmtdesc) {
    if ((res = xioctl (state->fd, VIDIOC_ENUM_FMT, &state->fmtdesc)) < 0) {
      if (errno != EINVAL)
        perror ("VIDIOC_ENUM_FMT");
      return SPA_RESULT_ENUM_END;
    }
    state->next_fmtdesc = false;

    state->frmsize.index = 0;
    state->frmsize.pixel_format = state->fmtdesc.pixelformat;
    state->next_frmsize = true;
  }

  if (!(info = fourcc_to_format_info (state->fmtdesc.pixelformat))) {
    state->fmtdesc.index++;
    state->next_fmtdesc = true;
    goto again;
  }

  if (state->next_frmsize) {
    if ((res = xioctl (state->fd, VIDIOC_ENUM_FRAMESIZES, &state->frmsize)) < 0) {
      if (errno == EINVAL) {
        state->fmtdesc.index++;
        state->next_fmtdesc = true;
        goto again;
      }
      perror ("VIDIOC_ENUM_FRAMESIZES");
      return SPA_RESULT_ENUM_END;
    }
    state->next_frmsize = false;

    if (state->frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
      state->frmival.index = 0;
      state->frmival.pixel_format = state->frmsize.pixel_format;
      state->frmival.width = state->frmsize.discrete.width;
      state->frmival.height = state->frmsize.discrete.height;
    }
  }

  fmt = &state->format[0];
  fmt->fmt.media_type = info->media_type;
  fmt->fmt.media_subtype = info->media_subtype;
  fmt->fmt.props.prop_info = fmt->infos;
  fmt->fmt.props.n_prop_info = pi = 0;
  fmt->fmt.props.set_prop = spa_props_generic_set_prop;
  fmt->fmt.props.get_prop = spa_props_generic_get_prop;
  fmt->unset_mask = 0;

  if (info->media_subtype == SPA_MEDIA_SUBTYPE_RAW) {
    spa_video_raw_fill_prop_info (&fmt->infos[pi],
                                  SPA_PROP_ID_VIDEO_FORMAT,
                                  offsetof (V4l2Format, format));
    fmt->infos[pi].mask_offset = offsetof (V4l2Format, unset_mask);
    fmt->format = info->format;
    pi = ++fmt->fmt.props.n_prop_info;
  }

  spa_video_raw_fill_prop_info (&fmt->infos[pi],
                                SPA_PROP_ID_VIDEO_SIZE,
                                offsetof (V4l2Format, size));
  fmt->infos[pi].mask_offset = offsetof (V4l2Format, unset_mask);
  fmt->size.width = state->frmsize.discrete.width;
  fmt->size.height = state->frmsize.discrete.height;
  pi = ++fmt->fmt.props.n_prop_info;

  spa_video_raw_fill_prop_info (&fmt->infos[pi],
                                SPA_PROP_ID_VIDEO_FRAMERATE,
                                offsetof (V4l2Format, framerate));
  fmt->infos[pi].mask_offset = offsetof (V4l2Format, unset_mask);
  fmt->infos[pi].range_type = SPA_PROP_RANGE_TYPE_ENUM;
  fmt->infos[pi].range_values = fmt->ranges;
  fmt->infos[pi].n_range_values = 0;
  i = state->frmival.index = 0;

  while (true) {
    if ((res = xioctl (state->fd, VIDIOC_ENUM_FRAMEINTERVALS, &state->frmival)) < 0) {
      if (errno == EINVAL) {
        state->frmsize.index++;
        state->next_frmsize = true;
        break;
      }
      perror ("VIDIOC_ENUM_FRAMEINTERVALS");
      return SPA_RESULT_ENUM_END;
    }

    fmt->ranges[i].name = NULL;
    fmt->ranges[i].description = NULL;
    fmt->ranges[i].size = sizeof (SpaFraction);
    fmt->framerates[i].num = state->frmival.discrete.numerator;
    fmt->framerates[i].denom = state->frmival.discrete.denominator;
    fmt->ranges[i].value = &fmt->framerates[i];

    i = ++state->frmival.index;
  }
  fmt->infos[pi].n_range_values = i;
  fmt->infos[pi].unset_mask = 1 << i;
  fmt->unset_mask |= fmt->infos[pi].unset_mask;
  pi = ++fmt->fmt.props.n_prop_info;

  *format = &state->format[0].fmt;

  return SPA_RESULT_OK;
}

static int
spa_v4l2_set_format (SpaV4l2Source *this, V4l2Format *f, bool try_only)
{
  SpaV4l2State *state = &this->state[0];
  int cmd = try_only ? VIDIOC_TRY_FMT : VIDIOC_S_FMT;
  struct v4l2_format reqfmt, fmt;
  struct v4l2_streamparm streamparm;
  const FormatInfo *info = NULL;
  int i;

  CLEAR (fmt);
  CLEAR (streamparm);
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  for (i = 0; i < SPA_N_ELEMENTS (format_info); i++) {
    if (format_info[i].media_type == f->fmt.media_type &&
        format_info[i].media_subtype == f->fmt.media_subtype &&
        format_info[i].format == f->format) {
      info = &format_info[i];
      break;
    }
  }
  if (info == NULL)
    return -1;

  fmt.fmt.pix.pixelformat = info->fourcc;
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  fmt.fmt.pix.width = f->size.width;
  fmt.fmt.pix.height = f->size.height;
  streamparm.parm.capture.timeperframe.numerator = f->framerate.denom;
  streamparm.parm.capture.timeperframe.denominator = f->framerate.num;

  fprintf (stderr, "set %08x %dx%d %d/%d\n", fmt.fmt.pix.pixelformat,
      fmt.fmt.pix.width, fmt.fmt.pix.height,
      streamparm.parm.capture.timeperframe.denominator,
      streamparm.parm.capture.timeperframe.numerator);

  reqfmt = fmt;

  if (spa_v4l2_open (this) < 0)
    return -1;

  if (xioctl (state->fd, cmd, &fmt) < 0) {
    perror ("VIDIOC_S_FMT");
    return -1;
  }

  /* some cheap USB cam's won't accept any change */
  if (xioctl (state->fd, VIDIOC_S_PARM, &streamparm) < 0)
    perror ("VIDIOC_S_PARM");

  if (reqfmt.fmt.pix.pixelformat != fmt.fmt.pix.pixelformat ||
      reqfmt.fmt.pix.width != fmt.fmt.pix.width ||
      reqfmt.fmt.pix.height != fmt.fmt.pix.height)
    return -1;

  if (try_only)
    return 0;

  state->fmt = fmt;
  state->info.flags = SPA_PORT_INFO_FLAG_CAN_GIVE_BUFFER;
  state->info.maxbuffering = -1;
  state->info.latency = -1;

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

static int
spa_v4l2_close (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state[0];

  if (!state->opened)
    return 0;

  fprintf (stderr, "close\n");
  if (close(state->fd))
    perror ("close");

  state->fd = -1;
  state->opened = false;

  return 0;
}

static SpaResult
mmap_read (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state[0];
  struct v4l2_buffer buf;
  V4l2Buffer *b;

  CLEAR(buf);
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = state->memtype;

  if (xioctl (state->fd, VIDIOC_DQBUF, &buf) < 0) {
    switch (errno) {
      case EAGAIN:
        return 0;
      case EIO:
      default:
        perror ("VIDIOC_DQBUF");
        return SPA_RESULT_ERROR;
    }
  }

  b = &state->alloc_buffers[buf.index];
  b->header.seq = buf.sequence;
  b->header.pts = (uint64_t)buf.timestamp.tv_sec * 1000000000lu + (uint64_t)buf.timestamp.tv_usec * 1000lu;
  b->next = state->ready;
  state->ready = b;
  state->ready_count++;

  return SPA_RESULT_OK;
}

static int
v4l2_on_fd_events (SpaPollNotifyData *data)
{
  SpaV4l2Source *this = data->user_data;
  SpaEvent event;

  if (mmap_read (this) < 0)
    return 0;

  event.type = SPA_EVENT_TYPE_CAN_PULL_OUTPUT;
  event.port_id = 0;
  event.size = 0;
  event.data = NULL;
  this->event_cb (&this->node, &event, this->user_data);

  return 0;
}

static void
spa_v4l2_buffer_recycle (SpaV4l2Source *this, uint32_t buffer_id)
{
  SpaV4l2State *state = &this->state[0];
  V4l2Buffer *b = &state->alloc_buffers[buffer_id];

  b->outstanding = false;

  if (xioctl (state->fd, VIDIOC_QBUF, &b->v4l2_buffer) < 0) {
    perror ("VIDIOC_QBUF");
  }
}

static SpaResult
spa_v4l2_import_buffers (SpaV4l2Source *this, SpaBuffer **buffers, uint32_t n_buffers)
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
    spa_memory_free (state->alloc_mem->pool_id, state->alloc_mem->id);
  state->alloc_mem = spa_memory_alloc_with_fd (0, NULL, sizeof (V4l2Buffer) * reqbuf.count);
  state->alloc_buffers = spa_memory_ensure_ptr (state->alloc_mem);

  for (i = 0; i < reqbuf.count; i++) {
    V4l2Buffer *b;
    uint32_t mem_id;
    SpaMemory *mem;
    SpaData *d = SPA_BUFFER_DATAS (buffers[i]);

    b = &state->alloc_buffers[i];
    b->buffer.mem_id = state->alloc_mem->id;
    b->buffer.offset = sizeof (V4l2Buffer) * i;
    b->buffer.size = sizeof (V4l2Buffer);
    b->buffer.id = SPA_ID_INVALID;
    b->outbuf = buffers[i];
    b->outstanding = true;

    fprintf (stderr, "import buffer %p\n", buffers[i]);

    mem_id = SPA_BUFFER_DATAS (buffers[i])[0].mem_id;
    if (!(mem = spa_memory_find (0, mem_id))) {
      fprintf (stderr, "invalid memory on buffer %p\n", buffers[i]);
      continue;
    }

    CLEAR (b->v4l2_buffer);
    b->v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b->v4l2_buffer.memory = state->memtype;
    b->v4l2_buffer.index = i;
    b->v4l2_buffer.m.userptr = (unsigned long) ((uint8_t*)mem->ptr + d[0].offset);
    b->v4l2_buffer.length = d[0].size;

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
    spa_memory_free (state->alloc_mem->pool_id, state->alloc_mem->id);
  state->alloc_mem = spa_memory_alloc_with_fd (0, NULL, sizeof (V4l2Buffer) * reqbuf.count);
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
    b->buffer.mem_id = state->alloc_mem->id;
    b->buffer.offset = sizeof (V4l2Buffer) * i;
    b->buffer.size = sizeof (V4l2Buffer);

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

    mem = spa_memory_alloc (0);
    mem->flags = SPA_MEMORY_FLAG_READABLE;
    mem->size = buf.length;
    b->datas[0].mem_id = mem->id;
    b->datas[0].offset = 0;
    b->datas[0].size = buf.length;
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

    spa_debug_buffer (&b->buffer);

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
  SpaEvent event;

  if (spa_v4l2_open (this) < 0)
    return SPA_RESULT_ERROR;

  if (!state->have_buffers)
    return SPA_RESULT_NO_BUFFERS;

  event.type = SPA_EVENT_TYPE_ADD_POLL;
  event.port_id = 0;
  event.data = &state->poll;
  event.size = sizeof (state->poll);

  state->fds[0].fd = state->fd;
  state->fds[0].events = POLLIN | POLLPRI | POLLERR;
  state->fds[0].revents = 0;

  state->poll.id = 0;
  state->poll.fds = state->fds;
  state->poll.n_fds = 1;
  state->poll.idle_cb = NULL;
  state->poll.before_cb = NULL;
  state->poll.after_cb = v4l2_on_fd_events;
  state->poll.user_data = this;
  this->event_cb (&this->node, &event, this->user_data);

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl (state->fd, VIDIOC_STREAMON, &type) < 0) {
    perror ("VIDIOC_STREAMON");
    return SPA_RESULT_ERROR;
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_stop (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state[0];
  enum v4l2_buf_type type;
  SpaEvent event;
  int i;

  if (!state->opened)
    return SPA_RESULT_OK;

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl (state->fd, VIDIOC_STREAMOFF, &type) < 0) {
    perror ("VIDIOC_STREAMOFF");
    return SPA_RESULT_ERROR;
  }

  for (i = 0; i < state->reqbuf.count; i++) {
    V4l2Buffer *b;
    SpaMemory *mem;

    b = &state->alloc_buffers[i];
    if (b->outstanding) {
      fprintf (stderr, "queueing outstanding buffer %p\n", b);
      spa_v4l2_buffer_recycle (this, i);
    }
    mem = spa_memory_find (0, b->datas[0].mem_id);
    if (state->export_buf) {
      close (mem->fd);
    } else {
      munmap (mem->ptr, mem->size);
    }
    spa_memory_free (0, mem->id);
  }
  state->have_buffers = false;

  event.type = SPA_EVENT_TYPE_REMOVE_POLL;
  event.port_id = 0;
  event.data = &state->poll;
  event.size = sizeof (state->poll);
  this->event_cb (&this->node, &event, this->user_data);

  spa_v4l2_close (this);

  return SPA_RESULT_OK;
}
