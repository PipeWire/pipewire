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

static SpaVideoFormat
fourcc_to_video_format (uint32_t fourcc)
{
  SpaVideoFormat format;

  switch (fourcc) {
    case V4L2_PIX_FMT_GREY:    /*  8  Greyscale     */
      format = SPA_VIDEO_FORMAT_GRAY8;
      break;
    case V4L2_PIX_FMT_Y16:
      format = SPA_VIDEO_FORMAT_GRAY16_LE;
      break;
    case V4L2_PIX_FMT_Y16_BE:
      format = SPA_VIDEO_FORMAT_GRAY16_BE;
      break;
    case V4L2_PIX_FMT_XRGB555:
    case V4L2_PIX_FMT_RGB555:
      format = SPA_VIDEO_FORMAT_RGB15;
      break;
    case V4L2_PIX_FMT_XRGB555X:
    case V4L2_PIX_FMT_RGB555X:
      format = SPA_VIDEO_FORMAT_BGR15;
      break;
    case V4L2_PIX_FMT_RGB565:
      format = SPA_VIDEO_FORMAT_RGB16;
      break;
    case V4L2_PIX_FMT_RGB24:
      format = SPA_VIDEO_FORMAT_RGB;
      break;
    case V4L2_PIX_FMT_BGR24:
      format = SPA_VIDEO_FORMAT_BGR;
      break;
    case V4L2_PIX_FMT_XRGB32:
    case V4L2_PIX_FMT_RGB32:
      format = SPA_VIDEO_FORMAT_xRGB;
      break;
    case V4L2_PIX_FMT_XBGR32:
    case V4L2_PIX_FMT_BGR32:
      format = SPA_VIDEO_FORMAT_BGRx;
      break;
    case V4L2_PIX_FMT_ABGR32:
      format = SPA_VIDEO_FORMAT_BGRA;
      break;
    case V4L2_PIX_FMT_ARGB32:
      format = SPA_VIDEO_FORMAT_ARGB;
      break;
    case V4L2_PIX_FMT_NV12:
    case V4L2_PIX_FMT_NV12M:
      format = SPA_VIDEO_FORMAT_NV12;
      break;
    case V4L2_PIX_FMT_NV12MT:
      format = SPA_VIDEO_FORMAT_NV12_64Z32;
      break;
    case V4L2_PIX_FMT_NV21:
    case V4L2_PIX_FMT_NV21M:
      format = SPA_VIDEO_FORMAT_NV21;
      break;
    case V4L2_PIX_FMT_YVU410:
      format = SPA_VIDEO_FORMAT_YVU9;
      break;
    case V4L2_PIX_FMT_YUV410:
      format = SPA_VIDEO_FORMAT_YUV9;
      break;
    case V4L2_PIX_FMT_YUV420:
    case V4L2_PIX_FMT_YUV420M:
      format = SPA_VIDEO_FORMAT_I420;
      break;
    case V4L2_PIX_FMT_YUYV:
      format = SPA_VIDEO_FORMAT_YUY2;
      break;
    case V4L2_PIX_FMT_YVU420:
      format = SPA_VIDEO_FORMAT_YV12;
      break;
    case V4L2_PIX_FMT_UYVY:
      format = SPA_VIDEO_FORMAT_UYVY;
      break;
    case V4L2_PIX_FMT_YUV411P:
      format = SPA_VIDEO_FORMAT_Y41B;
      break;
    case V4L2_PIX_FMT_YUV422P:
      format = SPA_VIDEO_FORMAT_Y42B;
      break;
    case V4L2_PIX_FMT_YVYU:
      format = SPA_VIDEO_FORMAT_YVYU;
      break;
    case V4L2_PIX_FMT_NV16:
    case V4L2_PIX_FMT_NV16M:
      format = SPA_VIDEO_FORMAT_NV16;
      break;
    case V4L2_PIX_FMT_NV61:
    case V4L2_PIX_FMT_NV61M:
      format = SPA_VIDEO_FORMAT_NV61;
      break;
    case V4L2_PIX_FMT_NV24:
      format = SPA_VIDEO_FORMAT_NV24;
      break;
    default:
      format = SPA_VIDEO_FORMAT_UNKNOWN;
      break;
  }
  return format;
}

static uint32_t
video_format_to_fourcc (SpaVideoFormat format)
{
  uint32_t fourcc;

  switch (format) {
    case SPA_VIDEO_FORMAT_I420:
      fourcc = V4L2_PIX_FMT_YUV420;
      break;
    case SPA_VIDEO_FORMAT_YUY2:
      fourcc = V4L2_PIX_FMT_YUYV;
      break;
    case SPA_VIDEO_FORMAT_UYVY:
      fourcc = V4L2_PIX_FMT_UYVY;
      break;
    case SPA_VIDEO_FORMAT_YV12:
      fourcc = V4L2_PIX_FMT_YVU420;
      break;
    case SPA_VIDEO_FORMAT_Y41B:
      fourcc = V4L2_PIX_FMT_YUV411P;
      break;
    case SPA_VIDEO_FORMAT_Y42B:
      fourcc = V4L2_PIX_FMT_YUV422P;
      break;
    case SPA_VIDEO_FORMAT_NV12:
      fourcc = V4L2_PIX_FMT_NV12;
      break;
   case SPA_VIDEO_FORMAT_NV12_64Z32:
      fourcc = V4L2_PIX_FMT_NV12MT;
      break;
    case SPA_VIDEO_FORMAT_NV21:
      fourcc = V4L2_PIX_FMT_NV21;
      break;
    case SPA_VIDEO_FORMAT_NV16:
      fourcc = V4L2_PIX_FMT_NV16;
      break;
    case SPA_VIDEO_FORMAT_NV61:
      fourcc = V4L2_PIX_FMT_NV61;
      break;
    case SPA_VIDEO_FORMAT_NV24:
      fourcc = V4L2_PIX_FMT_NV24;
      break;
    case SPA_VIDEO_FORMAT_YVYU:
      fourcc = V4L2_PIX_FMT_YVYU;
      break;
    case SPA_VIDEO_FORMAT_RGB15:
      fourcc = V4L2_PIX_FMT_RGB555;
      break;
    case SPA_VIDEO_FORMAT_RGB16:
      fourcc = V4L2_PIX_FMT_RGB565;
      break;
    case SPA_VIDEO_FORMAT_RGB:
      fourcc = V4L2_PIX_FMT_RGB24;
      break;
    case SPA_VIDEO_FORMAT_BGR:
      fourcc = V4L2_PIX_FMT_BGR24;
      break;
    case SPA_VIDEO_FORMAT_xRGB:
      fourcc = V4L2_PIX_FMT_RGB32;
      break;
    case SPA_VIDEO_FORMAT_ARGB:
      fourcc = V4L2_PIX_FMT_RGB32;
      break;
    case SPA_VIDEO_FORMAT_BGRx:
      fourcc = V4L2_PIX_FMT_BGR32;
      break;
    case SPA_VIDEO_FORMAT_BGRA:
      fourcc = V4L2_PIX_FMT_BGR32;
      break;
    case SPA_VIDEO_FORMAT_GRAY8:
      fourcc = V4L2_PIX_FMT_GREY;
      break;
    case SPA_VIDEO_FORMAT_GRAY16_LE:
      fourcc = V4L2_PIX_FMT_Y16;
      break;
    case SPA_VIDEO_FORMAT_GRAY16_BE:
      fourcc = V4L2_PIX_FMT_Y16_BE;
      break;
    default:
      fourcc = 0;
      break;
  }
  return fourcc;
}

#define FOURCC_ARGS(f) (f)&0x7f,((f)>>8)&0x7f,((f)>>16)&0x7f,((f)>>24)&0x7f

static SpaResult
spa_v4l2_enum_format (SpaV4l2Source *this, SpaFormat **format, void **cookie)
{
  SpaV4l2State *state = &this->state[0];
  int res;

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
    state->next_frmival = true;

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
      state->next_frmival = true;
    }
  }
  if (state->next_frmival) {
    if ((res = xioctl (state->fd, VIDIOC_ENUM_FRAMEINTERVALS, &state->frmival)) < 0) {
      if (errno == EINVAL) {
        state->frmsize.index++;
        state->next_frmsize = true;
        goto again;
      }
      perror ("VIDIOC_ENUM_FRAMEINTERVALS");
      return SPA_RESULT_ENUM_END;
    }
    state->frmival.index++;
  }

  spa_video_raw_format_init (&state->raw_format[0]);
  state->raw_format[0].info.format = fourcc_to_video_format (state->fmtdesc.pixelformat);
  state->raw_format[0].info.size.width = state->frmsize.discrete.width;
  state->raw_format[0].info.size.height = state->frmsize.discrete.height;
  state->raw_format[0].info.framerate.num = state->frmival.discrete.numerator;
  state->raw_format[0].info.framerate.denom = state->frmival.discrete.denominator;
  state->raw_format[0].unset_mask &= ~((1<<0)|(1<<1)|(1<<2));

  *format = &state->raw_format[0].format;

  return SPA_RESULT_OK;
}

static int
spa_v4l2_set_format (SpaV4l2Source *this, SpaFormat *format, bool try_only)
{
  SpaV4l2State *state = &this->state[0];
  int cmd = try_only ? VIDIOC_TRY_FMT : VIDIOC_S_FMT;
  struct v4l2_format reqfmt, fmt;
  struct v4l2_streamparm streamparm;

  CLEAR (fmt);
  CLEAR (streamparm);
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  if (format->media_type == SPA_MEDIA_TYPE_VIDEO) {
    if (format->media_subtype == SPA_MEDIA_SUBTYPE_RAW) {
      SpaVideoRawFormat *f = (SpaVideoRawFormat *) format;

      fmt.fmt.pix.pixelformat = video_format_to_fourcc (f->info.format);
      fmt.fmt.pix.width = f->info.size.width;
      fmt.fmt.pix.height = f->info.size.height;
      fmt.fmt.pix.field = V4L2_FIELD_ANY;
      streamparm.parm.capture.timeperframe.numerator = f->info.framerate.denom;
      streamparm.parm.capture.timeperframe.denominator = f->info.framerate.num;

      fprintf (stderr, "set %08x %dx%d %d/%d\n", fmt.fmt.pix.pixelformat,
          fmt.fmt.pix.width, fmt.fmt.pix.height, f->info.framerate.denom,
          f->info.framerate.num);

    } else
      return -1;
  } else
    return -1;

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
  state->param_buffers.param.size = sizeof (&state->buffers);
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

static int
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
        return -1;
    }
  }

  b = &state->buffers[buf.index];
  b->next = state->ready;
  state->ready = b;
  state->ready_count++;

  return 0;
}

static int
v4l2_on_fd_events (SpaPollNotifyData *data)
{
  SpaV4l2Source *this = data->user_data;
  SpaEvent event;

  mmap_read (this);

  event.refcount = 1;
  event.notify = NULL;
  event.type = SPA_EVENT_TYPE_CAN_PULL_OUTPUT;
  event.port_id = 0;
  event.size = 0;
  event.data = NULL;
  this->event_cb (&this->handle, &event, this->user_data);

  return 0;
}

static void
v4l2_buffer_free (void *data)
{
  V4l2Buffer *b = (V4l2Buffer *) data;
  SpaV4l2Source *this = b->source;
  SpaV4l2State *state = &this->state[0];

  b->buffer.refcount = 1;
  b->outstanding = false;

  if (xioctl (state->fd, VIDIOC_QBUF, &b->v4l2_buffer) < 0) {
    perror ("VIDIOC_QBUF");
  }
}

static int
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
    return -1;
  }
  fprintf (stderr, "got %d buffers\n", reqbuf.count);
  if (reqbuf.count < 2) {
    fprintf (stderr, "can't allocate enough buffers\n");
    return -1;
  }
  state->reqbuf = reqbuf;

  for (i = 0; i < reqbuf.count; i++) {
    V4l2Buffer *b;

    b = &state->buffers[i];

    b->source = this;
    b->buffer.refcount = 0;
    b->buffer.notify = v4l2_buffer_free;
    b->buffer.size = buffers[i]->size;
    b->buffer.n_metas = buffers[i]->n_metas;
    b->buffer.metas = buffers[i]->metas;
    b->buffer.n_datas = buffers[i]->n_datas;
    b->buffer.datas = buffers[i]->datas;
    b->imported = buffers[i];
    b->outstanding = true;

    CLEAR (b->v4l2_buffer);
    b->v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b->v4l2_buffer.memory = state->memtype;
    b->v4l2_buffer.index = i;
    b->v4l2_buffer.m.userptr = (unsigned long) b->buffer.datas[0].ptr;
    b->v4l2_buffer.length = b->buffer.datas[0].size;

    v4l2_buffer_free (b);
  }
  state->have_buffers = true;

  return 0;
}

static int
mmap_init (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state[0];
  struct v4l2_requestbuffers reqbuf;
  int i;

  state->memtype = V4L2_MEMORY_MMAP;

  CLEAR(reqbuf);
  reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  reqbuf.memory = state->memtype;
  reqbuf.count = MAX_BUFFERS;

  if (xioctl (state->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
    perror ("VIDIOC_REQBUFS");
    return -1;
  }

  fprintf (stderr, "got %d buffers\n", reqbuf.count);
  if (reqbuf.count < 2) {
    fprintf (stderr, "can't allocate enough buffers\n");
    return -1;
  }
  if (state->export_buf)
    fprintf (stderr, "using EXPBUF\n");

  state->reqbuf = reqbuf;

  for (i = 0; i < reqbuf.count; i++) {
    struct v4l2_buffer buf;
    V4l2Buffer *b;

    CLEAR (buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = state->memtype;
    buf.index = i;

    if (xioctl (state->fd, VIDIOC_QUERYBUF, &buf) < 0) {
      perror ("VIDIOC_QUERYBUF");
      return -1;
    }

    b = &state->buffers[i];
    b->source = this;
    b->buffer.refcount = 0;
    b->buffer.notify = v4l2_buffer_free;
    b->buffer.size = buf.length;
    b->buffer.n_metas = 1;
    b->buffer.metas = b->metas;
    b->buffer.n_datas = 1;
    b->buffer.datas = b->datas;

    b->header.flags = 0;
    b->header.seq = 0;
    b->header.pts = 0;
    b->header.dts_offset = 0;

    b->metas[0].type = SPA_META_TYPE_HEADER;
    b->metas[0].data = &b->header;
    b->metas[0].size = sizeof (b->header);

    if (state->export_buf) {
      struct v4l2_exportbuffer expbuf;

      CLEAR (expbuf);
      expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      expbuf.index = i;
      if (xioctl (state->fd, VIDIOC_EXPBUF, &expbuf) < 0) {
        perror("VIDIOC_EXPBUF");
        continue;
      }

      b->dmafd = expbuf.fd;
      b->datas[0].type = SPA_DATA_TYPE_FD;
      b->datas[0].ptr = &b->dmafd;
      b->datas[0].ptr_type = "dmabuf";
      b->datas[0].offset = 0;
      b->datas[0].size = buf.length;
      b->datas[0].stride = state->fmt.fmt.pix.bytesperline;
    } else {
      b->datas[0].type = SPA_DATA_TYPE_MEMPTR;
      b->datas[0].ptr_type = "sysmem";
      b->datas[0].ptr = mmap (NULL,
                              buf.length,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED,
                              state->fd,
                              buf.m.offset);
      b->datas[0].offset = 0;
      b->datas[0].size = buf.length;
      b->datas[0].stride = state->fmt.fmt.pix.bytesperline;
      if (b->datas[0].ptr == MAP_FAILED) {
        perror ("mmap");
        continue;
      }
    }
    b->outstanding = true;

    CLEAR (b->v4l2_buffer);
    b->v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b->v4l2_buffer.memory = state->memtype;
    b->v4l2_buffer.index = i;

    v4l2_buffer_free (b);
  }
  state->have_buffers = true;

  return 0;
}

static int
userptr_init (SpaV4l2Source *this)
{
  return -1;
}

static int
read_init (SpaV4l2Source *this)
{
  return -1;
}

static int
spa_v4l2_start (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state[0];
  enum v4l2_buf_type type;
  SpaEvent event;

  if (spa_v4l2_open (this) < 0)
    return -1;

  if (!state->have_buffers) {
    if (state->cap.capabilities & V4L2_CAP_STREAMING) {
      if (mmap_init (this) < 0)
        if (userptr_init (this) < 0)
          return -1;
    } else if (state->cap.capabilities & V4L2_CAP_READWRITE) {
      if (read_init (this) < 0)
        return -1;
    } else
      return -1;
  }

  event.refcount = 1;
  event.notify = NULL;
  event.type = SPA_EVENT_TYPE_ADD_POLL;
  event.port_id = 0;
  event.data = &state->poll;
  event.size = sizeof (state->poll);

  state->fds[0].fd = state->fd;
  state->fds[0].events = POLLIN | POLLPRI | POLLERR;
  state->fds[0].revents = 0;

  state->poll.fds = state->fds;
  state->poll.n_fds = 1;
  state->poll.idle_cb = NULL;
  state->poll.before_cb = NULL;
  state->poll.after_cb = v4l2_on_fd_events;
  state->poll.user_data = this;
  this->event_cb (&this->handle, &event, this->user_data);

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl (state->fd, VIDIOC_STREAMON, &type) < 0) {
    perror ("VIDIOC_STREAMON");
    return -1;
  }
  return 0;
}

static int
spa_v4l2_stop (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state[0];
  enum v4l2_buf_type type;
  SpaEvent event;
  int i;

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl (state->fd, VIDIOC_STREAMOFF, &type) < 0) {
    perror ("VIDIOC_STREAMOFF");
    return -1;
  }

  for (i = 0; i < state->reqbuf.count; i++) {
    V4l2Buffer *b;

    b = &state->buffers[i];
    if (b->outstanding) {
      fprintf (stderr, "queueing outstanding buffer %p\n", b);
      v4l2_buffer_free (b);
    }
    if (state->export_buf) {
      close (b->dmafd);
    } else {
      munmap (b->datas[0].ptr, b->datas[0].size);
    }
  }
  state->have_buffers = false;

  event.refcount = 1;
  event.notify = NULL;
  event.type = SPA_EVENT_TYPE_REMOVE_POLL;
  event.port_id = 0;
  event.data = &state->poll;
  event.size = sizeof (state->poll);
  this->event_cb (&this->handle, &event, this->user_data);

  spa_v4l2_close (this);

  return 0;
}
