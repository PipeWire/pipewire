/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

/**
 * SECTION:element-pipewiresrc
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v pipewiresrc ! videoconvert ! ximagesink
 * ]| Shows pipewire output in an X window.
 * </refsect2>
 */

#define PW_ENABLE_DEPRECATED

#include "gstpipewiresrc.h"
#include "gstpipewireformat.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spa/param/video/format.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <gst/net/gstnetclientclock.h>
#include <gst/allocators/gstfdmemory.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>

#include "gstpipewireclock.h"

static GQuark process_mem_data_quark;

GST_DEBUG_CATEGORY_STATIC (pipewire_src_debug);
#define GST_CAT_DEFAULT pipewire_src_debug

#define DEFAULT_ALWAYS_COPY     false
#define DEFAULT_MIN_BUFFERS     8
#define DEFAULT_MAX_BUFFERS     INT32_MAX
#define DEFAULT_RESEND_LAST     false
#define DEFAULT_KEEPALIVE_TIME  0
#define DEFAULT_AUTOCONNECT     true
#define DEFAULT_USE_BUFFERPOOL USE_BUFFERPOOL_AUTO

enum
{
  PROP_0,
  PROP_PATH,
  PROP_TARGET_OBJECT,
  PROP_CLIENT_NAME,
  PROP_CLIENT_PROPERTIES,
  PROP_STREAM_PROPERTIES,
  PROP_ALWAYS_COPY,
  PROP_MIN_BUFFERS,
  PROP_MAX_BUFFERS,
  PROP_FD,
  PROP_RESEND_LAST,
  PROP_KEEPALIVE_TIME,
  PROP_AUTOCONNECT,
  PROP_USE_BUFFERPOOL,
};


static GstStaticPadTemplate gst_pipewire_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );

#define gst_pipewire_src_parent_class parent_class
G_DEFINE_TYPE (GstPipeWireSrc, gst_pipewire_src, GST_TYPE_PUSH_SRC);

static GstStateChangeReturn
gst_pipewire_src_change_state (GstElement * element, GstStateChange transition);

static gboolean gst_pipewire_src_send_event (GstElement * elem, GstEvent * event);

static gboolean gst_pipewire_src_negotiate (GstBaseSrc * basesrc);

static GstFlowReturn gst_pipewire_src_create (GstPushSrc * psrc,
    GstBuffer ** buffer);
static gboolean gst_pipewire_src_unlock (GstBaseSrc * basesrc);
static gboolean gst_pipewire_src_unlock_stop (GstBaseSrc * basesrc);
static gboolean gst_pipewire_src_start (GstBaseSrc * basesrc);
static gboolean gst_pipewire_src_stop (GstBaseSrc * basesrc);
static gboolean gst_pipewire_src_event (GstBaseSrc * src, GstEvent * event);
static gboolean gst_pipewire_src_query (GstBaseSrc * src, GstQuery * query);
static void gst_pipewire_src_get_times (GstBaseSrc * basesrc, GstBuffer * buffer,
    GstClockTime * start, GstClockTime * end);

static void
gst_pipewire_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (object);

  switch (prop_id) {
    case PROP_PATH:
      g_free (pwsrc->stream->path);
      pwsrc->stream->path = g_value_dup_string (value);
      break;

    case PROP_TARGET_OBJECT:
      g_free (pwsrc->stream->target_object);
      pwsrc->stream->target_object = g_value_dup_string (value);
      break;

    case PROP_CLIENT_NAME:
      g_free (pwsrc->stream->client_name);
      pwsrc->stream->client_name = g_value_dup_string (value);
      break;

    case PROP_CLIENT_PROPERTIES:
      if (pwsrc->stream->client_properties)
        gst_structure_free (pwsrc->stream->client_properties);
      pwsrc->stream->client_properties =
          gst_structure_copy (gst_value_get_structure (value));
      break;

    case PROP_STREAM_PROPERTIES:
      if (pwsrc->stream->stream_properties)
        gst_structure_free (pwsrc->stream->stream_properties);
      pwsrc->stream->stream_properties =
          gst_structure_copy (gst_value_get_structure (value));
      break;

    case PROP_ALWAYS_COPY:
      /* don't provide buffer if always copy*/
      if (g_value_get_boolean (value))
        pwsrc->use_bufferpool = USE_BUFFERPOOL_NO;
      else
        pwsrc->use_bufferpool = USE_BUFFERPOOL_YES;
      break;

    case PROP_MIN_BUFFERS:
      pwsrc->min_buffers = g_value_get_int (value);
      break;

    case PROP_MAX_BUFFERS:
      pwsrc->max_buffers = g_value_get_int (value);
      break;

    case PROP_FD:
      pwsrc->stream->fd = g_value_get_int (value);
      break;

    case PROP_RESEND_LAST:
      pwsrc->resend_last = g_value_get_boolean (value);
      break;

    case PROP_KEEPALIVE_TIME:
      pwsrc->keepalive_time = g_value_get_int (value);
      break;

    case PROP_AUTOCONNECT:
      pwsrc->autoconnect = g_value_get_boolean (value);
      break;

    case PROP_USE_BUFFERPOOL:
      if(g_value_get_boolean (value))
        pwsrc->use_bufferpool = USE_BUFFERPOOL_YES;
      else
        pwsrc->use_bufferpool = USE_BUFFERPOOL_NO;
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pipewire_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (object);

  switch (prop_id) {
    case PROP_PATH:
      g_value_set_string (value, pwsrc->stream->path);
      break;

    case PROP_TARGET_OBJECT:
      g_value_set_string (value, pwsrc->stream->target_object);
      break;

    case PROP_CLIENT_NAME:
      g_value_set_string (value, pwsrc->stream->client_name);
      break;

    case PROP_CLIENT_PROPERTIES:
      gst_value_set_structure (value, pwsrc->stream->client_properties);
      break;

    case PROP_STREAM_PROPERTIES:
      gst_value_set_structure (value, pwsrc->stream->stream_properties);
      break;

    case PROP_ALWAYS_COPY:
      g_value_set_boolean (value, !pwsrc->use_bufferpool);
      break;

    case PROP_MIN_BUFFERS:
      g_value_set_int (value, pwsrc->min_buffers);
      break;

    case PROP_MAX_BUFFERS:
      g_value_set_int (value, pwsrc->max_buffers);
      break;

    case PROP_FD:
      g_value_set_int (value, pwsrc->stream->fd);
      break;

    case PROP_RESEND_LAST:
      g_value_set_boolean (value, pwsrc->resend_last);
      break;

    case PROP_KEEPALIVE_TIME:
      g_value_set_int (value, pwsrc->keepalive_time);
      break;

    case PROP_AUTOCONNECT:
      g_value_set_boolean (value, pwsrc->autoconnect);
      break;

    case PROP_USE_BUFFERPOOL:
      g_value_set_boolean (value, !!pwsrc->use_bufferpool);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstClock *
gst_pipewire_src_provide_clock (GstElement * elem)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (elem);
  GstClock *clock;

  GST_OBJECT_LOCK (pwsrc);
  if (!GST_OBJECT_FLAG_IS_SET (pwsrc, GST_ELEMENT_FLAG_PROVIDE_CLOCK))
    goto clock_disabled;

  if (pwsrc->stream->clock && pwsrc->is_live)
    clock = GST_CLOCK_CAST (gst_object_ref (pwsrc->stream->clock));
  else
    clock = NULL;
  GST_OBJECT_UNLOCK (pwsrc);

  return clock;

  /* ERRORS */
clock_disabled:
  {
    GST_DEBUG_OBJECT (pwsrc, "clock provide disabled");
    GST_OBJECT_UNLOCK (pwsrc);
    return NULL;
  }
}

static void
gst_pipewire_src_finalize (GObject * object)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (object);

  gst_clear_object (&pwsrc->stream);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_pipewire_src_class_init (GstPipeWireSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSrcClass *gstbasesrc_class;
  GstPushSrcClass *gstpushsrc_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesrc_class = (GstBaseSrcClass *) klass;
  gstpushsrc_class = (GstPushSrcClass *) klass;

  gobject_class->finalize = gst_pipewire_src_finalize;
  gobject_class->set_property = gst_pipewire_src_set_property;
  gobject_class->get_property = gst_pipewire_src_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        "Path",
                                                        "The source path to connect to (NULL = default)",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS |
                                                        G_PARAM_DEPRECATED));

  g_object_class_install_property (gobject_class,
                                   PROP_TARGET_OBJECT,
                                   g_param_spec_string ("target-object",
                                                        "Target object",
                                                        "The source name/serial to connect to (NULL = default)",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_CLIENT_NAME,
                                   g_param_spec_string ("client-name",
                                                        "Client Name",
                                                        "The client name to use (NULL = default)",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_CLIENT_PROPERTIES,
                                   g_param_spec_boxed ("client-properties",
                                                       "client properties",
                                                       "list of PipeWire client properties",
                                                       GST_TYPE_STRUCTURE,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_STREAM_PROPERTIES,
                                   g_param_spec_boxed ("stream-properties",
                                                       "stream properties",
                                                       "list of PipeWire stream properties",
                                                       GST_TYPE_STRUCTURE,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_ALWAYS_COPY,
                                   g_param_spec_boolean ("always-copy",
                                                         "Always copy",
                                                         "Always copy the buffer and data",
                                                         DEFAULT_ALWAYS_COPY,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_STATIC_STRINGS |
                                                         G_PARAM_DEPRECATED));

  g_object_class_install_property (gobject_class,
                                   PROP_MIN_BUFFERS,
                                   g_param_spec_int ("min-buffers",
                                                     "Min Buffers",
                                                     "Minimum number of buffers to negotiate with PipeWire",
                                                     1, G_MAXINT, DEFAULT_MIN_BUFFERS,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_MAX_BUFFERS,
                                   g_param_spec_int ("max-buffers",
                                                     "Max Buffers",
                                                     "Maximum number of buffers to negotiate with PipeWire",
                                                     1, G_MAXINT, DEFAULT_MAX_BUFFERS,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_FD,
                                   g_param_spec_int ("fd",
                                                     "Fd",
                                                     "The fd to connect with",
                                                     -1, G_MAXINT, -1,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_RESEND_LAST,
                                   g_param_spec_boolean ("resend-last",
                                                         "Resend last",
                                                         "Resend last buffer on EOS",
                                                         DEFAULT_RESEND_LAST,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_KEEPALIVE_TIME,
                                   g_param_spec_int ("keepalive-time",
                                                     "Keepalive Time",
                                                     "Periodically send last buffer (in milliseconds, 0 = disabled)",
                                                     0, G_MAXINT, DEFAULT_KEEPALIVE_TIME,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_AUTOCONNECT,
                                   g_param_spec_boolean ("autoconnect",
                                                         "Connect automatically",
                                                         "Attempt to find a peer to connect to",
                                                         DEFAULT_AUTOCONNECT,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_USE_BUFFERPOOL,
                                   g_param_spec_boolean ("use-bufferpool",
                                                         "Use bufferpool",
                                                         "Use bufferpool (default: true for video, false for audio)",
                                                         DEFAULT_USE_BUFFERPOOL,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_STATIC_STRINGS));

  gstelement_class->provide_clock = gst_pipewire_src_provide_clock;
  gstelement_class->change_state = gst_pipewire_src_change_state;
  gstelement_class->send_event = gst_pipewire_src_send_event;

  gst_element_class_set_static_metadata (gstelement_class,
      "PipeWire source", "Source/Audio/Video",
      "Uses PipeWire to create audio/video", "Wim Taymans <wim.taymans@gmail.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pipewire_src_template));

  gstbasesrc_class->negotiate = gst_pipewire_src_negotiate;
  gstbasesrc_class->unlock = gst_pipewire_src_unlock;
  gstbasesrc_class->unlock_stop = gst_pipewire_src_unlock_stop;
  gstbasesrc_class->start = gst_pipewire_src_start;
  gstbasesrc_class->stop = gst_pipewire_src_stop;
  gstbasesrc_class->event = gst_pipewire_src_event;
  gstbasesrc_class->query = gst_pipewire_src_query;
  gstbasesrc_class->get_times = gst_pipewire_src_get_times;
  gstpushsrc_class->create = gst_pipewire_src_create;

  GST_DEBUG_CATEGORY_INIT (pipewire_src_debug, "pipewiresrc", 0,
      "PipeWire Source");

  process_mem_data_quark = g_quark_from_static_string ("GstPipeWireSrcProcessMemQuark");
}

static void
gst_pipewire_src_init (GstPipeWireSrc * src)
{
  /* we operate in time */
  gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);

  /* we're a live source, unless explicitly requested not to be */
  gst_base_src_set_live (GST_BASE_SRC (src), TRUE);

  GST_OBJECT_FLAG_SET (src, GST_ELEMENT_FLAG_PROVIDE_CLOCK);

  src->stream = gst_pipewire_stream_new (GST_ELEMENT (src));

  src->use_bufferpool = DEFAULT_USE_BUFFERPOOL;
  src->min_buffers = DEFAULT_MIN_BUFFERS;
  src->max_buffers = DEFAULT_MAX_BUFFERS;
  src->resend_last = DEFAULT_RESEND_LAST;
  src->keepalive_time = DEFAULT_KEEPALIVE_TIME;
  src->autoconnect = DEFAULT_AUTOCONNECT;
  src->min_latency = 0;
  src->max_latency = GST_CLOCK_TIME_NONE;

  src->transform_value = UINT32_MAX;
}

static gboolean
buffer_recycle (GstMiniObject *obj)
{
  GstPipeWireSrc *src;
  GstPipeWirePoolData *data;
  int res;

  data = gst_pipewire_pool_get_data (GST_BUFFER_CAST(obj));

  GST_OBJECT_LOCK (data->pool);
  if (!obj->dispose) {
    GST_OBJECT_UNLOCK (data->pool);
    return TRUE;
  }

  GST_BUFFER_FLAGS (obj) = data->flags;
  src = data->owner;

  pw_thread_loop_lock (src->stream->core->loop);
  if (!obj->dispose) {
    pw_thread_loop_unlock (src->stream->core->loop);
    GST_OBJECT_UNLOCK (data->pool);
    return TRUE;
  }

  gst_mini_object_ref (obj);

  data->queued = TRUE;

  if ((res = pw_stream_queue_buffer (src->stream->pwstream, data->b)) < 0)
    GST_WARNING_OBJECT (src, "can't queue recycled buffer %p, %s", obj, spa_strerror(res));
  else
    GST_LOG_OBJECT (src, "recycle buffer %p", obj);

  pw_thread_loop_unlock (src->stream->core->loop);

  GST_OBJECT_UNLOCK (data->pool);

  return FALSE;
}

static void
on_add_buffer (void *_data, struct pw_buffer *b)
{
  GstPipeWireSrc *pwsrc = _data;
  GstPipeWirePoolData *data;

  gst_pipewire_pool_wrap_buffer (pwsrc->stream->pool, b);
  data = b->user_data;
  GST_DEBUG_OBJECT (pwsrc, "add buffer %p", data->buf);
  data->owner = pwsrc;
  data->queued = TRUE;
  GST_MINI_OBJECT_CAST (data->buf)->dispose = buffer_recycle;
}

static void
on_remove_buffer (void *_data, struct pw_buffer *b)
{
  GstPipeWireSrc *pwsrc = _data;
  GstPipeWirePoolData *data = b->user_data;
  GstBuffer *buf = data->buf;
  int res;

  GST_DEBUG_OBJECT (pwsrc, "remove buffer %p", buf);

  GST_MINI_OBJECT_CAST (buf)->dispose = NULL;

  if (data->queued) {
    gst_buffer_unref (buf);
  } else {
    if ((res = pw_stream_queue_buffer (pwsrc->stream->pwstream, b)) < 0)
      GST_WARNING_OBJECT (pwsrc, "can't queue removed buffer %p, %s", buf, spa_strerror(res));
  }
}

static const char * const transform_map[] = {
  [SPA_META_TRANSFORMATION_None] = "rotate-0",
  [SPA_META_TRANSFORMATION_90] = "rotate-90",
  [SPA_META_TRANSFORMATION_180] = "rotate-180",
  [SPA_META_TRANSFORMATION_270] = "rotate-270",
  [SPA_META_TRANSFORMATION_Flipped] = "flip-rotate-0",
  [SPA_META_TRANSFORMATION_Flipped90] = "flip-rotate-270",
  [SPA_META_TRANSFORMATION_Flipped180] = "flip-rotate-180",
  [SPA_META_TRANSFORMATION_Flipped270] = "flip-rotate-90",
};

static const char *spa_transform_value_to_gst_image_orientation(uint32_t transform_value)
{
  if (transform_value >= SPA_N_ELEMENTS(transform_map))
    transform_value = SPA_META_TRANSFORMATION_None;

  return transform_map[transform_value];
}

static GstBuffer *dequeue_buffer(GstPipeWireSrc *pwsrc)
{
  struct pw_buffer *b;
  GstBuffer *buf;
  GstPipeWirePoolData *data;
  struct spa_meta_header *h;
  struct spa_meta_region *crop;
  enum spa_meta_videotransform_value transform_value;
  struct pw_time time;
  guint i;

  b = pw_stream_dequeue_buffer (pwsrc->stream->pwstream);
  if (b == NULL)
          return NULL;

  data = b->user_data;

  if (!GST_IS_BUFFER (data->buf)) {
    GST_ERROR_OBJECT (pwsrc, "stream buffer %p is missing", data->buf);
    return NULL;
  }

  if (!data->queued) {
    GST_ERROR_OBJECT (pwsrc, "buffer %p was not recycled", data->buf);
    return NULL;
  }

  pw_stream_get_time_n(pwsrc->stream->pwstream, &time, sizeof(time));

  if (pwsrc->delay != time.delay && time.rate.denom != 0) {
    pwsrc->min_latency = time.delay * GST_SECOND * time.rate.num / time.rate.denom;
    GST_LOG_OBJECT (pwsrc, "latency changed %"PRIi64" -> %"PRIi64" %"PRIu64,
		    pwsrc->delay, time.delay, pwsrc->min_latency);
    pwsrc->delay = time.delay;
    gst_element_post_message (GST_ELEMENT_CAST (pwsrc),
      gst_message_new_latency (GST_OBJECT_CAST (pwsrc)));
  }

  GST_LOG_OBJECT (pwsrc, "got new buffer %p", data->buf);

  buf = gst_buffer_new ();

  data->queued = FALSE;
  GST_BUFFER_PTS (buf) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DTS (buf) = GST_CLOCK_TIME_NONE;

  h = data->header;
  if (h) {
    GST_LOG_OBJECT (pwsrc, "pts %" G_GUINT64_FORMAT ", dts_offset %" G_GUINT64_FORMAT, h->pts, h->dts_offset);

    if (GST_CLOCK_TIME_IS_VALID (h->pts)) {
      GST_BUFFER_PTS (buf) = h->pts;
      if (GST_BUFFER_PTS (buf) + h->dts_offset > 0)
        GST_BUFFER_DTS (buf) = GST_BUFFER_PTS (buf) + h->dts_offset;
    }
    GST_BUFFER_OFFSET (buf) = h->seq;
  } else {
    GST_BUFFER_PTS (buf) = b->time - pwsrc->delay;
    GST_BUFFER_DTS (buf) = b->time - pwsrc->delay;
  }

  if (pwsrc->is_video) {
    if (pwsrc->video_info.fps_n) {
      GST_BUFFER_DURATION (buf) = gst_util_uint64_scale (GST_SECOND,
          pwsrc->video_info.fps_d, pwsrc->video_info.fps_n);
    }
  } else {
    GST_BUFFER_DURATION (buf) = gst_util_uint64_scale (GST_SECOND,
        time.size * time.rate.num, time.rate.denom);
  }

  crop = data->crop;
  if (crop) {
    GstVideoCropMeta *meta = gst_buffer_get_video_crop_meta(buf);
    if (meta) {
      meta->x = crop->region.position.x;
      meta->y = crop->region.position.y;
      meta->width = crop->region.size.width;
      meta->height = crop->region.size.height;
    }
  }

  transform_value = data->videotransform ? data->videotransform->transform :
                                           SPA_META_TRANSFORMATION_None;
  if (transform_value != pwsrc->transform_value) {
    GstEvent *tag_event;
    const char* tag_string;

    tag_string = spa_transform_value_to_gst_image_orientation(transform_value);

    GST_LOG_OBJECT (pwsrc, "got new videotransform: %u / %s",
        transform_value, tag_string);

    tag_event = gst_event_new_tag(gst_tag_list_new(GST_TAG_IMAGE_ORIENTATION,
        tag_string, NULL));
    gst_pad_push_event (GST_BASE_SRC_PAD (pwsrc), tag_event);

    pwsrc->transform_value = transform_value;
  }

  if (pwsrc->is_video) {
    gsize video_size = 0;
    GstVideoInfo *info = &pwsrc->video_info;
    GstVideoMeta *meta = gst_buffer_add_video_meta_full (buf, GST_VIDEO_FRAME_FLAG_NONE,
                             GST_VIDEO_INFO_FORMAT (info),
                             GST_VIDEO_INFO_WIDTH (info),
                             GST_VIDEO_INFO_HEIGHT (info),
                             GST_VIDEO_INFO_N_PLANES (info),
                             info->offset,
                             info->stride);

    for (i = 0; i < MIN (b->buffer->n_datas, GST_VIDEO_MAX_PLANES); i++) {
      struct spa_data *d = &b->buffer->datas[i];
      meta->offset[i] = video_size;
      meta->stride[i] = d->chunk->stride;

      video_size += d->chunk->size;
    }
  }

  for (i = 0; i < b->buffer->n_datas; i++) {
    struct spa_data *d = &b->buffer->datas[i];

    if (d->chunk->size == 0) {
      // Skip the 0 sized chunk, not adding to the buffer
      GST_DEBUG_OBJECT(pwsrc, "Chunk size is 0, skipping");
      continue;
    }

    GstMemory *pmem = gst_buffer_peek_memory (data->buf, i);
    if (pmem) {
      GstMemory *mem;
      if (pwsrc->use_bufferpool != USE_BUFFERPOOL_NO)
        mem = gst_memory_share (pmem, d->chunk->offset, d->chunk->size);
      else
        mem = gst_memory_copy (pmem, d->chunk->offset, d->chunk->size);
      gst_buffer_insert_memory (buf, i, mem);
    }
    if (d->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) {
      GST_DEBUG_OBJECT(pwsrc, "Buffer corrupted");
      GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_CORRUPTED);
    }
  }
  if (pwsrc->use_bufferpool != USE_BUFFERPOOL_NO)
    gst_buffer_add_parent_buffer_meta (buf, data->buf);
  gst_buffer_unref (data->buf);

  if (gst_buffer_get_size(buf) == 0)
  {
    GST_ERROR_OBJECT(pwsrc, "Buffer is empty, dropping this");
    gst_buffer_unref(buf);
    buf = NULL;
  }

  return buf;
}

static void
on_process (void *_data)
{
  GstPipeWireSrc *pwsrc = _data;
  pw_thread_loop_signal (pwsrc->stream->core->loop, FALSE);
}

static void
on_state_changed (void *data,
                  enum pw_stream_state old,
                  enum pw_stream_state state, const char *error)
{
  GstPipeWireSrc *pwsrc = data;

  GST_DEBUG ("got stream state %s", pw_stream_state_as_string (state));

  switch (state) {
    case PW_STREAM_STATE_UNCONNECTED:
    case PW_STREAM_STATE_CONNECTING:
    case PW_STREAM_STATE_PAUSED:
    case PW_STREAM_STATE_STREAMING:
      break;
    case PW_STREAM_STATE_ERROR:
      /* make the error permanent, if it is not already;
         pw_stream_set_error() will recursively call us again */
      if (pw_stream_get_state (pwsrc->stream->pwstream, NULL) != PW_STREAM_STATE_ERROR)
        pw_stream_set_error (pwsrc->stream->pwstream, -EPIPE, "%s", error);
      else
        GST_ELEMENT_ERROR (pwsrc, RESOURCE, FAILED,
            ("stream error: %s", error), (NULL));
      break;
  }
  pw_thread_loop_signal (pwsrc->stream->core->loop, FALSE);
}

static void
parse_stream_properties (GstPipeWireSrc *pwsrc, const struct pw_properties *props)
{
  const gchar *var;
  gboolean is_live;

  GST_OBJECT_LOCK (pwsrc);
  var = pw_properties_get (props, PW_KEY_STREAM_IS_LIVE);
  is_live = pwsrc->is_live = var ? pw_properties_parse_bool(var) : TRUE;
  GST_OBJECT_UNLOCK (pwsrc);

  GST_DEBUG_OBJECT (pwsrc, "live %d", is_live);

  gst_base_src_set_live (GST_BASE_SRC (pwsrc), is_live);
}

static gboolean
gst_pipewire_src_stream_start (GstPipeWireSrc *pwsrc)
{
  const char *error = NULL;
  struct timespec abstime;

  pw_thread_loop_lock (pwsrc->stream->core->loop);
  GST_DEBUG_OBJECT (pwsrc, "doing stream start");

  pw_thread_loop_get_time (pwsrc->stream->core->loop, &abstime,
                  GST_PIPEWIRE_DEFAULT_TIMEOUT * SPA_NSEC_PER_SEC);

  while (TRUE) {
    enum pw_stream_state state = pw_stream_get_state (pwsrc->stream->pwstream, &error);

    GST_DEBUG_OBJECT (pwsrc, "waiting for STREAMING, now %s", pw_stream_state_as_string (state));
    if (state == PW_STREAM_STATE_STREAMING)
      break;

    if (state == PW_STREAM_STATE_ERROR)
      goto start_error;

    if (pwsrc->flushing) {
      error = "flushing";
      goto start_error;
    }

    if (pw_thread_loop_timed_wait_full (pwsrc->stream->core->loop, &abstime) < 0) {
      error = "timeout";
      goto start_error;
    }
  }

  parse_stream_properties (pwsrc, pw_stream_get_properties (pwsrc->stream->pwstream));
  GST_DEBUG_OBJECT (pwsrc, "signal started");
  pwsrc->started = TRUE;
  pw_thread_loop_signal (pwsrc->stream->core->loop, FALSE);
  pw_thread_loop_unlock (pwsrc->stream->core->loop);

  return TRUE;

start_error:
  {
    GST_DEBUG_OBJECT (pwsrc, "error starting stream: %s", error);
    pwsrc->started = FALSE;
    pw_thread_loop_signal (pwsrc->stream->core->loop, FALSE);
    pw_thread_loop_unlock (pwsrc->stream->core->loop);
    return FALSE;
  }
}

static enum pw_stream_state
wait_started (GstPipeWireSrc *this)
{
  enum pw_stream_state state, prev_state = PW_STREAM_STATE_UNCONNECTED;
  const char *error = NULL;
  struct timespec abstime;
  gboolean restart = FALSE;

  pw_thread_loop_lock (this->stream->core->loop);

  pw_thread_loop_get_time (this->stream->core->loop, &abstime,
                  GST_PIPEWIRE_DEFAULT_TIMEOUT * SPA_NSEC_PER_SEC);

  /* when started already is true then expects a re-start, so allow prev_state
   * degrade until turned around. */
  if (this->started) {
    GST_DEBUG_OBJECT (this, "restart in progress");
    restart = TRUE;
    this->started = FALSE;
  }

  while (TRUE) {
    state = pw_stream_get_state (this->stream->pwstream, &error);

    GST_DEBUG_OBJECT (this, "waiting for started signal, state now %s",
        pw_stream_state_as_string (state));

    if (state == PW_STREAM_STATE_ERROR ||
        (state == PW_STREAM_STATE_UNCONNECTED && prev_state > PW_STREAM_STATE_UNCONNECTED && !restart) ||
        this->flushing) {
      state = PW_STREAM_STATE_ERROR;
      break;
    }

    if (this->started)
      break;

    if (this->autoconnect) {
      if (pw_thread_loop_timed_wait_full (this->stream->core->loop, &abstime) < 0) {
        state = PW_STREAM_STATE_ERROR;
        break;
      }
    } else {
      pw_thread_loop_wait (this->stream->core->loop);
    }

    if (restart)
      restart = state != PW_STREAM_STATE_UNCONNECTED;
    prev_state = state;
  }
  GST_DEBUG_OBJECT (this, "got started signal: %s",
                  pw_stream_state_as_string (state));
  pw_thread_loop_unlock (this->stream->core->loop);

  return state;
}

static gboolean
gst_pipewire_src_negotiate (GstBaseSrc * basesrc)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (basesrc);
  g_autoptr (GstCaps) thiscaps = NULL;
  g_autoptr (GstCaps) possible_caps = NULL;
  g_autoptr (GstCaps) negotiated_caps = NULL;
  g_autoptr (GstCaps) peercaps = NULL;
  g_autoptr (GPtrArray) possible = NULL;
  gboolean result = FALSE;
  const char *error = NULL;
  struct timespec abstime;
  uint32_t target_id;

  /* first see what is possible on our source pad */
  thiscaps = gst_pad_query_caps (GST_BASE_SRC_PAD (basesrc), NULL);
  GST_DEBUG_OBJECT (basesrc, "caps of src: %" GST_PTR_FORMAT, thiscaps);
  /* nothing or anything is allowed, we're done */
  if (thiscaps == NULL)
    goto no_nego_needed;

  if (G_UNLIKELY (gst_caps_is_empty (thiscaps)))
    goto no_caps;

  /* get the peer caps */
  peercaps = gst_pad_peer_query_caps (GST_BASE_SRC_PAD (basesrc), thiscaps);
  GST_DEBUG_OBJECT (basesrc, "caps of peer: %" GST_PTR_FORMAT, peercaps);
  if (peercaps) {
    /* The result is already a subset of our caps */
    possible_caps = g_steal_pointer (&peercaps);
  } else {
    /* no peer, work with our own caps then */
    possible_caps = g_steal_pointer (&thiscaps);
  }

  GST_DEBUG_OBJECT (basesrc, "have common caps: %" GST_PTR_FORMAT, possible_caps);
  gst_caps_sanitize (&possible_caps);

  if (gst_caps_is_empty (possible_caps))
    goto no_common_caps;

  GST_DEBUG_OBJECT (basesrc, "have common caps (sanitized): %" GST_PTR_FORMAT, possible_caps);

  if (pw_stream_get_state(pwsrc->stream->pwstream, NULL) == PW_STREAM_STATE_STREAMING) {
    g_autoptr (GstCaps) current_caps = NULL;
    g_autoptr (GstCaps) preferred_new_caps = NULL;

    current_caps = gst_pad_get_current_caps (GST_BASE_SRC_PAD (pwsrc));
    preferred_new_caps = gst_caps_copy_nth (possible_caps, 0);

    if (current_caps && gst_caps_is_equal (current_caps, preferred_new_caps)) {
      GST_DEBUG_OBJECT (pwsrc,
                        "Stream running and new caps equal current ones. "
                        "Skipping renegotiation.");
      goto no_nego_needed;
    }
  }

  /* open a connection with these caps */
  possible = gst_caps_to_format_all (possible_caps);

  /* first disconnect */
  pw_thread_loop_lock (pwsrc->stream->core->loop);
  if (pw_stream_get_state(pwsrc->stream->pwstream, &error) != PW_STREAM_STATE_UNCONNECTED) {
    GST_DEBUG_OBJECT (basesrc, "disconnect capture");
    pw_stream_disconnect (pwsrc->stream->pwstream);
    while (TRUE) {
      enum pw_stream_state state = pw_stream_get_state (pwsrc->stream->pwstream, &error);

      GST_DEBUG_OBJECT (basesrc, "waiting for UNCONNECTED, now %s", pw_stream_state_as_string (state));
      if (state == PW_STREAM_STATE_UNCONNECTED)
        break;

      if (state == PW_STREAM_STATE_ERROR || pwsrc->flushing)
        goto connect_error;

      pw_thread_loop_wait (pwsrc->stream->core->loop);
    }
  }

  target_id = pwsrc->stream->path ? (uint32_t)atoi(pwsrc->stream->path) : PW_ID_ANY;

  if (pwsrc->stream->target_object) {
      struct spa_dict_item items[2] = {
        SPA_DICT_ITEM_INIT(PW_KEY_TARGET_OBJECT, pwsrc->stream->target_object),
	/* XXX deprecated but the portal and some example apps only
	 * provide the object id */
        SPA_DICT_ITEM_INIT(PW_KEY_NODE_TARGET, NULL),
      };
      struct spa_dict dict = SPA_DICT_INIT_ARRAY(items);
      uint64_t serial;

      /* If target.object is a name, set it also to node.target */
      if (spa_atou64(pwsrc->stream->target_object, &serial, 0)) {
        dict.n_items = 1;
      } else {
        target_id = PW_ID_ANY;
        items[1].value = pwsrc->stream->target_object;
      }

      pw_stream_update_properties (pwsrc->stream->pwstream, &dict);
  }

  GST_DEBUG_OBJECT (basesrc, "connect capture with path %s, target-object %s",
                    pwsrc->stream->path, pwsrc->stream->target_object);

  pwsrc->possible_caps = possible_caps;
  pwsrc->negotiated = FALSE;

  enum pw_stream_flags flags;
  flags = PW_STREAM_FLAG_DONT_RECONNECT |
	  PW_STREAM_FLAG_ASYNC;
  if (pwsrc->autoconnect)
    flags |= PW_STREAM_FLAG_AUTOCONNECT;
  pw_stream_connect (pwsrc->stream->pwstream,
                     PW_DIRECTION_INPUT,
                     target_id,
                     flags,
                     (const struct spa_pod **)possible->pdata,
                     possible->len);

  pw_thread_loop_get_time (pwsrc->stream->core->loop, &abstime,
                  GST_PIPEWIRE_DEFAULT_TIMEOUT * SPA_NSEC_PER_SEC);

  while (TRUE) {
    enum pw_stream_state state = pw_stream_get_state (pwsrc->stream->pwstream, &error);

    GST_DEBUG_OBJECT (basesrc, "waiting for NEGOTIATED, now %s", pw_stream_state_as_string (state));
    if (state == PW_STREAM_STATE_ERROR || pwsrc->flushing)
      goto connect_error;

    if (pwsrc->negotiated)
      break;

    if (pwsrc->autoconnect) {
      if (pw_thread_loop_timed_wait_full (pwsrc->stream->core->loop, &abstime) < 0)
        goto connect_error;
    } else {
      pw_thread_loop_wait (pwsrc->stream->core->loop);
    }
  }

  negotiated_caps = g_steal_pointer (&pwsrc->caps);
  pwsrc->possible_caps = NULL;
  pw_thread_loop_unlock (pwsrc->stream->core->loop);

  if (negotiated_caps == NULL)
    goto no_caps;

  gst_pipewire_clock_reset (GST_PIPEWIRE_CLOCK (pwsrc->stream->clock), 0);

  GST_DEBUG_OBJECT (pwsrc, "set format %" GST_PTR_FORMAT, negotiated_caps);
  result = gst_base_src_set_caps (GST_BASE_SRC (pwsrc), negotiated_caps);
  if (!result)
    goto no_caps;

  result = gst_pipewire_src_stream_start (pwsrc);

  return result;

no_nego_needed:
  {
    GST_DEBUG_OBJECT (basesrc, "no negotiation needed");
    return TRUE;
  }
no_caps:
  {
    const gchar * error_string = "No supported formats found";

    GST_ELEMENT_ERROR (basesrc, STREAM, FORMAT,
        ("%s", error_string),
        ("This element did not produce valid caps"));
    pw_thread_loop_lock (pwsrc->stream->core->loop);
    pw_stream_set_error (pwsrc->stream->pwstream, -EINVAL, "%s", error_string);
    pw_thread_loop_unlock (pwsrc->stream->core->loop);
    return FALSE;
  }
no_common_caps:
  {
    const gchar * error_string = "No supported formats found";

    GST_ELEMENT_ERROR (basesrc, STREAM, FORMAT,
        ("%s", error_string),
        ("This element does not have formats in common with the peer"));
    pw_thread_loop_lock (pwsrc->stream->core->loop);
    pw_stream_set_error (pwsrc->stream->pwstream, -EPIPE, "%s", error_string);
    pw_thread_loop_unlock (pwsrc->stream->core->loop);
    return FALSE;
  }
connect_error:
  {
    g_clear_pointer (&pwsrc->caps, gst_caps_unref);
    pwsrc->possible_caps = NULL;
    GST_DEBUG_OBJECT (basesrc, "connect error");
    pw_thread_loop_unlock (pwsrc->stream->core->loop);
    return FALSE;
  }
}

static void
handle_format_change (GstPipeWireSrc *pwsrc,
                   const struct spa_pod *param)
{
  GstStructure *structure;
  g_autoptr (GstCaps) pw_peer_caps = NULL;

  g_clear_pointer (&pwsrc->caps, gst_caps_unref);
  if (param == NULL) {
    GST_DEBUG_OBJECT (pwsrc, "clear format");
    pwsrc->negotiated = FALSE;
    pwsrc->is_video = FALSE;
    return;
  }

  pw_peer_caps = gst_caps_from_format (param);
  if (pw_peer_caps && pwsrc->possible_caps) {
    pwsrc->caps = gst_caps_intersect_full (pw_peer_caps,
                                           pwsrc->possible_caps,
                                           GST_CAPS_INTERSECT_FIRST);
    gst_caps_maybe_fixate_dma_format (pwsrc->caps);
  }

  if (pwsrc->caps && gst_caps_is_fixed (pwsrc->caps)) {
    pwsrc->negotiated = TRUE;

    structure = gst_caps_get_structure (pwsrc->caps, 0);
    if (g_str_has_prefix (gst_structure_get_name (structure), "video/") ||
        g_str_has_prefix (gst_structure_get_name (structure), "image/")) {
      pwsrc->is_video = TRUE;

#ifdef HAVE_GSTREAMER_DMA_DRM
      if (gst_video_is_dma_drm_caps (pwsrc->caps)) {
        if (!gst_video_info_dma_drm_from_caps (&pwsrc->drm_info, pwsrc->caps)) {
          GST_WARNING_OBJECT (pwsrc, "Can't create drm video info from caps");
          pw_stream_set_error (pwsrc->stream->pwstream, -EINVAL, "internal error");
          return;
        }

        if (!gst_video_info_dma_drm_to_video_info (&pwsrc->drm_info,
                                                   &pwsrc->video_info)) {
          GST_WARNING_OBJECT (pwsrc, "Can't create video info from drm video info");
          pw_stream_set_error (pwsrc->stream->pwstream, -EINVAL, "internal error");
          return;
        }
      } else {
        gst_video_info_dma_drm_init (&pwsrc->drm_info);
#endif
        gst_video_info_from_caps (&pwsrc->video_info, pwsrc->caps);
#ifdef HAVE_GSTREAMER_DMA_DRM
      }
#endif
    } else {
      /* Don't provide bufferpool for audio if not requested by the
       * application/user */
      if (pwsrc->use_bufferpool != USE_BUFFERPOOL_YES)
        pwsrc->use_bufferpool = USE_BUFFERPOOL_NO;
    }
  } else {
    pwsrc->negotiated = FALSE;
    pwsrc->is_video = FALSE;
  }

  if (pwsrc->caps) {
    const struct spa_pod *params[4];
    struct spa_pod_builder b = { NULL };
    uint8_t buffer[512];
    uint32_t buffers = CLAMP (16, pwsrc->min_buffers, pwsrc->max_buffers);
    int buffertypes;

    buffertypes = (1<<SPA_DATA_DmaBuf);
    if (!spa_pod_find_prop (param, NULL, SPA_FORMAT_VIDEO_modifier)) {
      buffertypes |= ((1<<SPA_DATA_MemFd) | (1<<SPA_DATA_MemPtr));
    }

    GST_DEBUG_OBJECT (pwsrc, "we got format %" GST_PTR_FORMAT, pwsrc->caps);

    spa_pod_builder_init (&b, buffer, sizeof (buffer));
    params[0] = spa_pod_builder_add_object (&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(buffers,
                                                            pwsrc->min_buffers,
                                                            pwsrc->max_buffers),
        SPA_PARAM_BUFFERS_blocks,  SPA_POD_CHOICE_RANGE_Int(0, 1, INT32_MAX),
        SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(0, 0, INT32_MAX),
        SPA_PARAM_BUFFERS_stride,  SPA_POD_CHOICE_RANGE_Int(0, 0, INT32_MAX),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(buffertypes));

    params[1] = spa_pod_builder_add_object (&b,
        SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof (struct spa_meta_header)));
    params[2] = spa_pod_builder_add_object (&b,
        SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof (struct spa_meta_region)));
    params[3] = spa_pod_builder_add_object (&b,
        SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoTransform),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof (struct spa_meta_videotransform)));

    GST_DEBUG_OBJECT (pwsrc, "doing finish format");
    pw_stream_update_params (pwsrc->stream->pwstream, params, SPA_N_ELEMENTS(params));
  } else {
    GST_WARNING_OBJECT (pwsrc, "finish format with error");
    pw_stream_set_error (pwsrc->stream->pwstream, -EINVAL, "unhandled format");
  }
  pw_thread_loop_signal (pwsrc->stream->core->loop, FALSE);
}

static void
on_param_changed (void *data, uint32_t id,
                   const struct spa_pod *param)
{
  GstPipeWireSrc *pwsrc = data;
  switch (id) {
    case SPA_PARAM_Format:
      handle_format_change(pwsrc, param);
      break;
  }
}

static gboolean
gst_pipewire_src_unlock (GstBaseSrc * basesrc)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (basesrc);

  pw_thread_loop_lock (pwsrc->stream->core->loop);
  GST_DEBUG_OBJECT (pwsrc, "setting flushing");
  pwsrc->flushing = TRUE;
  pw_thread_loop_signal (pwsrc->stream->core->loop, FALSE);
  pw_thread_loop_unlock (pwsrc->stream->core->loop);

  return TRUE;
}

static gboolean
gst_pipewire_src_unlock_stop (GstBaseSrc * basesrc)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (basesrc);

  pw_thread_loop_lock (pwsrc->stream->core->loop);
  GST_DEBUG_OBJECT (pwsrc, "unsetting flushing");
  pwsrc->flushing = FALSE;
  pw_thread_loop_unlock (pwsrc->stream->core->loop);

  return TRUE;
}

static gboolean
gst_pipewire_src_event (GstBaseSrc * src, GstEvent * event)
{
  gboolean res = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_UPSTREAM:
      if (gst_video_event_is_force_key_unit (event)) {
        GstClockTime running_time;
        gboolean all_headers;
        guint count;

        gst_video_event_parse_upstream_force_key_unit (event,
                &running_time, &all_headers, &count);

        res = TRUE;
      } else {
        res = GST_BASE_SRC_CLASS (parent_class)->event (src, event);
      }
      break;
    default:
      res = GST_BASE_SRC_CLASS (parent_class)->event (src, event);
      break;
  }
  return res;
}

static gboolean
gst_pipewire_src_query (GstBaseSrc * src, GstQuery * query)
{
  gboolean res = FALSE;
  GstPipeWireSrc *pwsrc;

  pwsrc = GST_PIPEWIRE_SRC (src);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
      GST_OBJECT_LOCK (pwsrc);
      gst_query_set_latency (query, pwsrc->is_live, pwsrc->min_latency, pwsrc->max_latency);
      GST_OBJECT_UNLOCK (pwsrc);
      res = TRUE;
      break;
    default:
      res = GST_BASE_SRC_CLASS (parent_class)->query (src, query);
      break;
  }
  return res;
}

static void
gst_pipewire_src_get_times (GstBaseSrc * basesrc, GstBuffer * buffer,
    GstClockTime * start, GstClockTime * end)
{
  GstPipeWireSrc *pwsrc = GST_PIPEWIRE_SRC (basesrc);

  /* for live sources, sync on the timestamp of the buffer */
  if (gst_base_src_is_live (basesrc)) {
    GstClockTime timestamp = GST_BUFFER_PTS (buffer);

    if (GST_CLOCK_TIME_IS_VALID (timestamp)) {
      /* get duration to calculate end time */
      GstClockTime duration = GST_BUFFER_DURATION (buffer);

      if (GST_CLOCK_TIME_IS_VALID (duration)) {
        *end = timestamp + duration;
      }
      *start = timestamp;
    }
  } else {
    *start = GST_CLOCK_TIME_NONE;
    *end = GST_CLOCK_TIME_NONE;
  }

  GST_LOG_OBJECT (pwsrc, "start %" GST_TIME_FORMAT " (%" G_GUINT64_FORMAT
      "), end %" GST_TIME_FORMAT " (%" G_GUINT64_FORMAT ")",
      GST_TIME_ARGS (*start), *start, GST_TIME_ARGS (*end), *end);
}

static GstFlowReturn
gst_pipewire_src_create (GstPushSrc * psrc, GstBuffer ** buffer)
{
  GstPipeWireSrc *pwsrc;
  const char *error = NULL;
  GstBuffer *buf;
  gboolean update_time = FALSE, timeout = FALSE;
  GstCaps *caps = NULL;

  pwsrc = GST_PIPEWIRE_SRC (psrc);

  pw_thread_loop_lock (pwsrc->stream->core->loop);
  if (!pwsrc->negotiated)
    goto not_negotiated;

  while (TRUE) {
    enum pw_stream_state state;

    if (pwsrc->flushing)
      goto streaming_stopped;

    if (pwsrc->stream == NULL)
      goto streaming_error;

    state = pw_stream_get_state (pwsrc->stream->pwstream, &error);
    if (state == PW_STREAM_STATE_ERROR)
      goto streaming_error;

    if (state == PW_STREAM_STATE_UNCONNECTED)
      goto streaming_stopped;

    if ((caps = pwsrc->caps) != NULL) {
      pwsrc->caps = NULL;
      pw_thread_loop_unlock (pwsrc->stream->core->loop);

      GST_DEBUG_OBJECT (pwsrc, "set format %" GST_PTR_FORMAT, caps);
      gst_base_src_set_caps (GST_BASE_SRC (pwsrc), caps);
      gst_caps_unref (caps);

      pw_thread_loop_lock (pwsrc->stream->core->loop);
      continue;
    }

    if (pwsrc->eos) {
      if (pwsrc->last_buffer == NULL)
        goto streaming_eos;
      buf = pwsrc->last_buffer;
      pwsrc->last_buffer = NULL;
      update_time = TRUE;
      GST_LOG_OBJECT (pwsrc, "EOS, send last buffer");
      break;
    } else if (timeout) {
      if (pwsrc->last_buffer != NULL) {
        update_time = TRUE;
        buf = gst_buffer_ref(pwsrc->last_buffer);
        GST_LOG_OBJECT (pwsrc, "timeout, send keepalive buffer");
        break;
      }
    } else {
      buf = dequeue_buffer (pwsrc);
      GST_LOG_OBJECT (pwsrc, "popped buffer %p", buf);
      if (buf != NULL) {
        if (pwsrc->resend_last || pwsrc->keepalive_time > 0)
          gst_buffer_replace (&pwsrc->last_buffer, buf);
        break;
      }
    }
    timeout = FALSE;
    if (pwsrc->keepalive_time > 0) {
      struct timespec abstime;
      pw_thread_loop_get_time(pwsrc->stream->core->loop, &abstime,
                      pwsrc->keepalive_time * SPA_NSEC_PER_MSEC);
      if (pw_thread_loop_timed_wait_full (pwsrc->stream->core->loop, &abstime) == -ETIMEDOUT)
        timeout = TRUE;
    } else {
      pw_thread_loop_wait (pwsrc->stream->core->loop);
    }
  }
  pw_thread_loop_unlock (pwsrc->stream->core->loop);

  *buffer = buf;

  if (update_time) {
    GstClock *clock;
    GstClockTime pts, dts;

    clock = gst_element_get_clock (GST_ELEMENT_CAST (pwsrc));
    if (clock != NULL) {
      pts = dts = gst_clock_get_time (clock);
      gst_object_unref (clock);
    } else {
      pts = dts = GST_CLOCK_TIME_NONE;
    }

    GST_BUFFER_PTS (*buffer) = pts;
    GST_BUFFER_DTS (*buffer) = dts;

    GST_LOG_OBJECT (pwsrc, "Sending keepalive buffer pts/dts: %" GST_TIME_FORMAT
      " (%" G_GUINT64_FORMAT ")", GST_TIME_ARGS (pts), pts);
  }

  return GST_FLOW_OK;

not_negotiated:
  {
    pw_thread_loop_unlock (pwsrc->stream->core->loop);
    return GST_FLOW_NOT_NEGOTIATED;
  }
streaming_eos:
  {
    pw_thread_loop_unlock (pwsrc->stream->core->loop);
    return GST_FLOW_EOS;
  }
streaming_error:
  {
    pw_thread_loop_unlock (pwsrc->stream->core->loop);
    return GST_FLOW_ERROR;
  }
streaming_stopped:
  {
    pw_thread_loop_unlock (pwsrc->stream->core->loop);
    return GST_FLOW_FLUSHING;
  }
}

static gboolean
gst_pipewire_src_start (GstBaseSrc * basesrc)
{
  return TRUE;
}

static gboolean
gst_pipewire_src_stop (GstBaseSrc * basesrc)
{
  GstPipeWireSrc *pwsrc;

  pwsrc = GST_PIPEWIRE_SRC (basesrc);

  pw_thread_loop_lock (pwsrc->stream->core->loop);
  pwsrc->eos = false;
  gst_buffer_replace (&pwsrc->last_buffer, NULL);
  gst_caps_replace(&pwsrc->caps, NULL);
  pwsrc->transform_value = UINT32_MAX;
  pw_thread_loop_unlock (pwsrc->stream->core->loop);

  return TRUE;
}

static const struct pw_stream_events stream_events = {
        PW_VERSION_STREAM_EVENTS,
        .state_changed = on_state_changed,
        .param_changed = on_param_changed,
        .add_buffer = on_add_buffer,
        .remove_buffer = on_remove_buffer,
        .process = on_process,
};

static gboolean
gst_pipewire_src_send_event (GstElement * elem, GstEvent * event)
{
  GstPipeWireSrc *this = GST_PIPEWIRE_SRC_CAST (elem);
  gboolean ret;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
      GST_DEBUG_OBJECT (this, "got EOS");
      pw_thread_loop_lock (this->stream->core->loop);
      this->eos = true;
      pw_thread_loop_signal (this->stream->core->loop, FALSE);
      pw_thread_loop_unlock (this->stream->core->loop);
      ret = TRUE;
      break;
    default:
      ret = GST_ELEMENT_CLASS (parent_class)->send_event (elem, event);
      break;
  }
  return ret;
}

static GstStateChangeReturn
gst_pipewire_src_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstPipeWireSrc *this = GST_PIPEWIRE_SRC_CAST (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_pipewire_stream_open (this->stream, &stream_events))
        goto open_failed;
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      /* uncork and start recording */
      pw_thread_loop_lock (this->stream->core->loop);
      pw_stream_set_active (this->stream->pwstream, true);
      pw_thread_loop_unlock (this->stream->core->loop);
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      /* stop recording ASAP by corking */
      pw_thread_loop_lock (this->stream->core->loop);
      pw_stream_set_active (this->stream->pwstream, false);
      pw_thread_loop_unlock (this->stream->core->loop);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      if (wait_started (this) == PW_STREAM_STATE_ERROR)
        goto open_failed;

      pw_thread_loop_lock (this->stream->core->loop);
      /* the initial stream state is active, which is needed for linking and
       * negotiation to happen and the bufferpool to be set up. We don't know
       * if we'll go to playing, so we deactivate the stream until that
       * transition happens. This is janky, but because of how bins propagate
       * state changes one transition at a time, there may not be a better way
       * to do this. */
      pw_stream_set_active (this->stream->pwstream, false);
      pw_thread_loop_unlock (this->stream->core->loop);

      if (gst_base_src_is_live (GST_BASE_SRC (element)))
        ret = GST_STATE_CHANGE_NO_PREROLL;
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      pw_thread_loop_lock (this->stream->core->loop);
      this->negotiated = FALSE;
      pw_thread_loop_unlock (this->stream->core->loop);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      gst_pipewire_stream_close (this->stream);
      break;
    default:
      break;
  }
  return ret;

  /* ERRORS */
open_failed:
  {
    return GST_STATE_CHANGE_FAILURE;
  }
}
