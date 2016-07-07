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
  SpaV4l2State *state = &this->state;
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

#if 0
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
#endif

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


static int
spa_v4l2_set_format (SpaV4l2Source *this, SpaFormat *format, bool try_only)
{
  SpaV4l2State *state = &this->state;
  int cmd = try_only ? VIDIOC_TRY_FMT : VIDIOC_S_FMT;
  struct v4l2_format reqfmt, fmt;

  CLEAR (fmt);
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  if (format->media_type == SPA_MEDIA_TYPE_VIDEO) {
    if (format->media_subtype == SPA_MEDIA_SUBTYPE_RAW) {
      SpaVideoRawFormat *f = (SpaVideoRawFormat *) format;

      fmt.fmt.pix.pixelformat = video_format_to_fourcc (f->info.format);
      fmt.fmt.pix.width = f->info.width;
      fmt.fmt.pix.height = f->info.height;
      fmt.fmt.pix.field = V4L2_FIELD_ANY;
      fprintf (stderr, "set %08x %dx%d\n", fmt.fmt.pix.pixelformat,
          fmt.fmt.pix.width, fmt.fmt.pix.height);
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

  if (reqfmt.fmt.pix.pixelformat != fmt.fmt.pix.pixelformat ||
      reqfmt.fmt.pix.width != fmt.fmt.pix.width ||
      reqfmt.fmt.pix.height != fmt.fmt.pix.height)
    return -1;

  if (try_only)
    return 0;

  state->fmt = fmt;

  return 0;
}

static int
spa_v4l2_close (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state;

  if (!state->opened)
    return 0;

  if (close(state->fd))
    perror ("close");

  state->fd = -1;
  state->opened = false;

  return 0;
}

static int
mmap_read (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state;
  struct v4l2_buffer buf;
  V4l2Buffer *b;

  CLEAR(buf);
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;

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
  fprintf (stderr, "captured buffer %d\n", buf.index);

  b = &state->buffers[buf.index];
  b->next = state->ready;
  state->ready = b;
  state->ready_count++;

  return 0;
}

static void
v4l2_on_fd_events (void *user_data)
{
  SpaV4l2Source *this = user_data;
  SpaEvent event;

  mmap_read (this);

  event.refcount = 1;
  event.notify = NULL;
  event.type = SPA_EVENT_TYPE_CAN_PULL_OUTPUT;
  event.port_id = 0;
  event.size = 0;
  event.data = NULL;
  this->event_cb (&this->handle, &event, this->user_data);
}

static void
v4l2_buffer_free (void *data)
{
  V4l2Buffer *b = (V4l2Buffer *) data;
  SpaV4l2Source *this = b->source;
  SpaV4l2State *state = &this->state;
  struct v4l2_buffer buf;

  CLEAR (buf);
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = b->index;

  b->buffer.refcount = 1;
  b->outstanding = false;

  fprintf (stderr, "queue buffer %d\n", buf.index);

  if (xioctl (state->fd, VIDIOC_QBUF, &buf) < 0) {
    perror ("VIDIOC_QBUF");
  }
}

static int
mmap_init (SpaV4l2Source *this)
{
  SpaV4l2State *state = &this->state;
  struct v4l2_requestbuffers reqbuf;
  int i;

  CLEAR(reqbuf);
  reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  reqbuf.memory = V4L2_MEMORY_MMAP;
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

  state->reqbuf = reqbuf;

  for (i = 0; i < reqbuf.count; i++) {
    struct v4l2_buffer buf;
    V4l2Buffer *b;

    CLEAR (buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;

    if (xioctl (state->fd, VIDIOC_QUERYBUF, &buf) < 0) {
      perror ("VIDIOC_QUERYBUF");
      return -1;
    }

    b = &state->buffers[i];
    b->index = i;
    b->source = this;
    b->buffer.refcount = 1;
    b->buffer.notify = v4l2_buffer_free;
    b->buffer.size = buf.length;
    b->buffer.n_metas = 1;
    b->buffer.metas = b->meta;
    b->buffer.n_datas = 1;
    b->buffer.datas = b->data;

    b->header.flags = 0;
    b->header.seq = 0;
    b->header.pts = 0;
    b->header.dts_offset = 0;

    b->meta[0].type = SPA_META_TYPE_HEADER;
    b->meta[0].data = &b->header;
    b->meta[0].size = sizeof (b->header);

    b->data[0].type = SPA_DATA_TYPE_MEMPTR;
    b->data[0].data = mmap (NULL,
                            buf.length,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED,
                            state->fd,
                            buf.m.offset);
    b->data[0].offset = 0;
    b->data[0].size = buf.length;
    b->data[0].stride = state->fmt.fmt.pix.bytesperline;

    if (b->data[0].data == MAP_FAILED) {
      perror ("mmap");
      return -1;
    }
  }
  for (i = 0; i < state->reqbuf.count; ++i) {
    struct v4l2_buffer buf;

    CLEAR (buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;

    if (xioctl (state->fd, VIDIOC_QBUF, &buf) < 0) {
      perror ("VIDIOC_QBUF");
      return -1;
    }
  }
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
  SpaV4l2State *state = &this->state;
  enum v4l2_buf_type type;
  SpaEvent event;

  if (spa_v4l2_open (this) < 0)
    return -1;

  if (state->cap.capabilities & V4L2_CAP_STREAMING) {
    if (mmap_init (this) < 0)
      if (userptr_init (this) < 0)
        return -1;
  } else if (state->cap.capabilities & V4L2_CAP_READWRITE) {
    if (read_init (this) < 0)
      return -1;
  } else
    return -1;

  event.refcount = 1;
  event.notify = NULL;
  event.type = SPA_EVENT_TYPE_ADD_POLL;
  event.port_id = 0;
  event.data = &state->poll;
  event.size = sizeof (state->poll);

  state->poll.fd = state->fd;
  state->poll.events = POLLIN | POLLPRI | POLLERR;
  state->poll.revents = 0;
  state->poll.callback = v4l2_on_fd_events;
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
  SpaV4l2State *state = &this->state;
  enum v4l2_buf_type type;
  SpaEvent event;

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl (state->fd, VIDIOC_STREAMOFF, &type) < 0) {
    perror ("VIDIOC_STREAMOFF");
    return -1;
  }

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
