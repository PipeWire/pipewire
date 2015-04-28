/* GStreamer
 * Copyright (C) <2015> Wim Taymans <wim.taymans@gmail.com>
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

/**
 * SECTION:element-pulsevideosrc
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v pulsevideosrc ! ximagesink
 * ]| Shows pulsevideo output in an X window.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "gstpvsrc.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gio/gunixfdmessage.h>
#include <gst/allocators/gstfdmemory.h>



GST_DEBUG_CATEGORY_STATIC (pulsevideo_src_debug);
#define GST_CAT_DEFAULT pulsevideo_src_debug

enum
{
  PROP_0,
  PROP_LAST
};


#define PVS_VIDEO_CAPS GST_VIDEO_CAPS_MAKE (GST_VIDEO_FORMATS_ALL)

static GstStaticPadTemplate gst_pulsevideo_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (PVS_VIDEO_CAPS)
    );

#define gst_pulsevideo_src_parent_class parent_class
G_DEFINE_TYPE (GstPulsevideoSrc, gst_pulsevideo_src, GST_TYPE_PUSH_SRC);

static void gst_pulsevideo_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_pulsevideo_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstStateChangeReturn
gst_pulsevideo_src_change_state (GstElement * element, GstStateChange transition);

static GstCaps *gst_pulsevideo_src_getcaps (GstBaseSrc * bsrc, GstCaps * filter);
static gboolean gst_pulsevideo_src_setcaps (GstBaseSrc * bsrc, GstCaps * caps);
static GstCaps *gst_pulsevideo_src_src_fixate (GstBaseSrc * bsrc,
    GstCaps * caps);

static gboolean gst_pulsevideo_src_query (GstBaseSrc * bsrc, GstQuery * query);

static gboolean gst_pulsevideo_src_decide_allocation (GstBaseSrc * bsrc,
    GstQuery * query);
static GstFlowReturn gst_pulsevideo_src_create (GstPushSrc * psrc,
    GstBuffer ** buffer);
static gboolean gst_pulsevideo_src_start (GstBaseSrc * basesrc);
static gboolean gst_pulsevideo_src_stop (GstBaseSrc * basesrc);

static void
gst_pulsevideo_src_class_init (GstPulsevideoSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSrcClass *gstbasesrc_class;
  GstPushSrcClass *gstpushsrc_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesrc_class = (GstBaseSrcClass *) klass;
  gstpushsrc_class = (GstPushSrcClass *) klass;

  gobject_class->set_property = gst_pulsevideo_src_set_property;
  gobject_class->get_property = gst_pulsevideo_src_get_property;

  gstelement_class->change_state = gst_pulsevideo_src_change_state;

  gst_element_class_set_static_metadata (gstelement_class,
      "Pulsevideo source", "Source/Video",
      "Uses pulsevideo to create video", "Wim Taymans <wim.taymans@gmail.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pulsevideo_src_template));

  gstbasesrc_class->get_caps = gst_pulsevideo_src_getcaps;
  gstbasesrc_class->set_caps = gst_pulsevideo_src_setcaps;
  gstbasesrc_class->fixate = gst_pulsevideo_src_src_fixate;
  gstbasesrc_class->query = gst_pulsevideo_src_query;
  gstbasesrc_class->start = gst_pulsevideo_src_start;
  gstbasesrc_class->stop = gst_pulsevideo_src_stop;
  gstbasesrc_class->decide_allocation = gst_pulsevideo_src_decide_allocation;

  gstpushsrc_class->create = gst_pulsevideo_src_create;
}

static void
gst_pulsevideo_src_init (GstPulsevideoSrc * src)
{
  /* we operate in time */
  gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);
  gst_base_src_set_live (GST_BASE_SRC (src), TRUE);

  src->fd_allocator = gst_fd_allocator_new ();
}

static GstCaps *
gst_pulsevideo_src_src_fixate (GstBaseSrc * bsrc, GstCaps * caps)
{
  GstStructure *structure;

  caps = gst_caps_make_writable (caps);

  structure = gst_caps_get_structure (caps, 0);

  gst_structure_fixate_field_nearest_int (structure, "width", 320);
  gst_structure_fixate_field_nearest_int (structure, "height", 240);
  gst_structure_fixate_field_nearest_fraction (structure, "framerate", 30, 1);

  if (gst_structure_has_field (structure, "pixel-aspect-ratio"))
    gst_structure_fixate_field_nearest_fraction (structure,
        "pixel-aspect-ratio", 1, 1);
  else
    gst_structure_set (structure, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
        NULL);

  if (gst_structure_has_field (structure, "colorimetry"))
    gst_structure_fixate_field_string (structure, "colorimetry", "bt601");
  if (gst_structure_has_field (structure, "chroma-site"))
    gst_structure_fixate_field_string (structure, "chroma-site", "mpeg2");

  if (gst_structure_has_field (structure, "interlace-mode"))
    gst_structure_fixate_field_string (structure, "interlace-mode",
        "progressive");
  else
    gst_structure_set (structure, "interlace-mode", G_TYPE_STRING,
        "progressive", NULL);

  caps = GST_BASE_SRC_CLASS (parent_class)->fixate (bsrc, caps);

  return caps;
}

static void
gst_pulsevideo_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pulsevideo_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_pulsevideo_src_decide_allocation (GstBaseSrc * bsrc, GstQuery * query)
{
  GstPulsevideoSrc *pvsrc;
  GstBufferPool *pool;
  gboolean update;
  guint size, min, max;
  GstStructure *config;
  GstCaps *caps = NULL;

  pvsrc = GST_PULSEVIDEO_SRC (bsrc);

  if (gst_query_get_n_allocation_pools (query) > 0) {
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);

    /* adjust size */
    size = MAX (size, pvsrc->info.size);
    update = TRUE;
  } else {
    pool = NULL;
    size = pvsrc->info.size;
    min = max = 0;
    update = FALSE;
  }

  /* no downstream pool, make our own */
  if (pool == NULL) {
    pool = gst_video_buffer_pool_new ();
  }

  config = gst_buffer_pool_get_config (pool);

  gst_query_parse_allocation (query, &caps, NULL);
  if (caps)
    gst_buffer_pool_config_set_params (config, caps, size, min, max);

  if (gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL)) {
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
  }
  gst_buffer_pool_set_config (pool, config);

  if (update)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
  else
    gst_query_add_allocation_pool (query, pool, size, min, max);

  if (pool)
    gst_object_unref (pool);

  return GST_BASE_SRC_CLASS (parent_class)->decide_allocation (bsrc, query);
}

static void
on_new_buffer (GObject    *gobject,
               gpointer    user_data)
{
  GstPulsevideoSrc *pvsrc = user_data;

  g_main_loop_quit (pvsrc->loop);
}

static void
on_socket_notify (GObject    *gobject,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  GSocket *socket;

  g_object_get (gobject, "socket", &socket, NULL);
  g_print ("got socket %p\n", socket);
}

static void
on_stream_notify (GObject    *gobject,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  PvStreamState state;
  GstPulsevideoSrc *pvsrc = user_data;

  g_object_get (gobject, "state", &state, NULL);
  g_print ("got stream state %d\n", state);

  switch (state) {
    case PV_STREAM_STATE_ERROR:
      g_main_loop_quit (pvsrc->loop);
      break;
    case PV_STREAM_STATE_READY:
      g_main_loop_quit (pvsrc->loop);
      g_main_context_push_thread_default (pvsrc->context);
      pv_stream_start (pvsrc->stream, PV_STREAM_MODE_BUFFER);
      g_main_context_pop_thread_default (pvsrc->context);
      break;
    case PV_STREAM_STATE_STREAMING:
      break;
    default:
      break;
  }
}

static GstCaps *
gst_pulsevideo_src_getcaps (GstBaseSrc * bsrc, GstCaps * filter)
{
  return GST_BASE_SRC_CLASS (parent_class)->get_caps (bsrc, filter);
}

static gboolean
gst_pulsevideo_src_setcaps (GstBaseSrc * bsrc, GstCaps * caps)
{
  const GstStructure *structure;
  GstPulsevideoSrc *pvsrc;
  GstVideoInfo info;
  GVariantBuilder builder;

  pvsrc = GST_PULSEVIDEO_SRC (bsrc);

  structure = gst_caps_get_structure (caps, 0);

  if (gst_structure_has_name (structure, "video/x-raw")) {
    /* we can use the parsing code */
    if (!gst_video_info_from_caps (&info, caps))
      goto parse_failed;

  } else {
    goto unsupported_caps;
  }

  /* looks ok here */
  pvsrc->info = info;

  g_main_context_push_thread_default (pvsrc->context);
  pvsrc->stream = pv_stream_new (pvsrc->ctx, "test", NULL);
  g_signal_connect (pvsrc->stream, "notify::state", (GCallback) on_stream_notify, pvsrc);
  g_signal_connect (pvsrc->stream, "notify::socket", (GCallback) on_socket_notify, pvsrc);
  g_signal_connect (pvsrc->stream, "new-buffer", (GCallback) on_new_buffer, pvsrc);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&builder, "{sv}", "format.encoding", g_variant_new_string ("video/x-raw"));
  g_variant_builder_add (&builder, "{sv}", "format.format",
      g_variant_new_string (gst_video_format_to_string (info.finfo->format)));
  g_variant_builder_add (&builder, "{sv}", "format.width", g_variant_new_int32 (info.width));
  g_variant_builder_add (&builder, "{sv}", "format.height", g_variant_new_int32 (info.height));
  g_variant_builder_add (&builder, "{sv}", "format.views", g_variant_new_int32 (info.views));
//  g_variant_builder_add (&builder, "{sv}", "format.chroma-site",
//      g_variant_new_string (gst_video_chroma_to_string (info.chroma_site)));
//  g_variant_builder_add (&builder, "{sv}", "format.colorimetry",
//      g_variant_new_take_string (gst_video_colorimetry_to_string (&info.colorimetry)));
//  g_variant_builder_add (&builder, "{sv}", "format.interlace-mode",
//      g_variant_new_string (gst_video_interlace_mode_to_string (info.interlace_mode)));

  pv_stream_connect_capture (pvsrc->stream, NULL, 0, g_variant_builder_end (&builder));
  g_main_context_pop_thread_default (pvsrc->context);

  g_main_loop_run (pvsrc->loop);

  GST_DEBUG_OBJECT (pvsrc, "size %dx%d, %d/%d fps",
      info.width, info.height, info.fps_n, info.fps_d);

  return TRUE;

  /* ERRORS */
parse_failed:
  {
    GST_DEBUG_OBJECT (bsrc, "failed to parse caps");
    return FALSE;
  }
unsupported_caps:
  {
    GST_DEBUG_OBJECT (bsrc, "unsupported caps: %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
}

static gboolean
gst_pulsevideo_src_query (GstBaseSrc * bsrc, GstQuery * query)
{
  gboolean res = FALSE;
  GstPulsevideoSrc *src;

  src = GST_PULSEVIDEO_SRC (bsrc);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONVERT:
    {
      GstFormat src_fmt, dest_fmt;
      gint64 src_val, dest_val;

      gst_query_parse_convert (query, &src_fmt, &src_val, &dest_fmt, &dest_val);
      res =
          gst_video_info_convert (&src->info, src_fmt, src_val, dest_fmt,
          &dest_val);
      gst_query_set_convert (query, src_fmt, src_val, dest_fmt, dest_val);
      break;
    }
    case GST_QUERY_LATENCY:
    {
      if (src->info.fps_n > 0) {
        GstClockTime latency;

        latency =
            gst_util_uint64_scale (GST_SECOND, src->info.fps_d,
            src->info.fps_n);
        gst_query_set_latency (query,
            gst_base_src_is_live (GST_BASE_SRC_CAST (src)), latency,
            GST_CLOCK_TIME_NONE);
        GST_DEBUG_OBJECT (src, "Reporting latency of %" GST_TIME_FORMAT,
            GST_TIME_ARGS (latency));
        res = TRUE;
      }
      break;
    }
    case GST_QUERY_DURATION:{
      if (bsrc->num_buffers != -1) {
        GstFormat format;

        gst_query_parse_duration (query, &format, NULL);
        switch (format) {
          case GST_FORMAT_TIME:{
            gint64 dur = gst_util_uint64_scale_int_round (bsrc->num_buffers
                * GST_SECOND, src->info.fps_d, src->info.fps_n);
            res = TRUE;
            gst_query_set_duration (query, GST_FORMAT_TIME, dur);
            goto done;
          }
          case GST_FORMAT_BYTES:
            res = TRUE;
            gst_query_set_duration (query, GST_FORMAT_BYTES,
                bsrc->num_buffers * src->info.size);
            goto done;
          default:
            break;
        }
      }
      /* fall through */
    }
    default:
      res = GST_BASE_SRC_CLASS (parent_class)->query (bsrc, query);
      break;
  }
done:
  return res;
}

static GstFlowReturn
gst_pulsevideo_src_create (GstPushSrc * psrc, GstBuffer ** buffer)
{
  GstPulsevideoSrc *pvsrc;
  PvBufferInfo info;

  pvsrc = GST_PULSEVIDEO_SRC (psrc);

  if (G_UNLIKELY (GST_VIDEO_INFO_FORMAT (&pvsrc->info) ==
          GST_VIDEO_FORMAT_UNKNOWN))
    goto not_negotiated;

  g_main_loop_run (pvsrc->loop);
  pv_stream_capture_buffer (pvsrc->stream, &info);

  *buffer = gst_buffer_new ();

  if (g_socket_control_message_get_msg_type (info.message) == SCM_RIGHTS) {
    gint *fds, n_fds;
    GstMemory *fdmem = NULL;

    fds = g_unix_fd_message_steal_fds (G_UNIX_FD_MESSAGE (info.message), &n_fds);

    fdmem = gst_fd_allocator_alloc (pvsrc->fd_allocator, fds[0],
              info.offset + info.size, GST_FD_MEMORY_FLAG_NONE);
    gst_memory_resize (fdmem, info.offset, info.size);

    gst_buffer_append_memory (*buffer, fdmem);
  }

  return GST_FLOW_OK;

not_negotiated:
  {
    return GST_FLOW_NOT_NEGOTIATED;
  }
}

static gboolean
gst_pulsevideo_src_start (GstBaseSrc * basesrc)
{
  GstPulsevideoSrc *src = GST_PULSEVIDEO_SRC (basesrc);

  gst_video_info_init (&src->info);

  return TRUE;
}

static gboolean
gst_pulsevideo_src_stop (GstBaseSrc * basesrc)
{
  return TRUE;
}

static gpointer
handle_mainloop (GstPulsevideoSrc *this)
{

  return NULL;
}

static void
on_state_notify (GObject    *gobject,
                 GParamSpec *pspec,
                 gpointer    user_data)
{
  GstPulsevideoSrc *pvsrc = user_data;
  PvContextState state;

  g_object_get (gobject, "state", &state, NULL);
  g_print ("got context state %d\n", state);

  switch (state) {
    case PV_CONTEXT_STATE_ERROR:
      g_main_loop_quit (pvsrc->loop);
      GST_ELEMENT_ERROR (pvsrc, RESOURCE, FAILED,
          ("Failed to connect stream: %s",
            pv_context_error (pvsrc->ctx)->message), (NULL));
      break;
    case PV_CONTEXT_STATE_READY:
      g_main_loop_quit (pvsrc->loop);
      break;
    default:
      break;
  }
}

static gboolean
gst_pulsevideo_src_open (GstPulsevideoSrc * pvsrc)
{
  g_main_context_push_thread_default (pvsrc->context);
  pvsrc->ctx = pv_context_new ("test-client", NULL);
  g_signal_connect (pvsrc->ctx, "notify::state", (GCallback) on_state_notify, pvsrc);
  pv_context_connect(pvsrc->ctx, PV_CONTEXT_FLAGS_NONE);
  g_main_context_pop_thread_default (pvsrc->context);

  g_main_loop_run (pvsrc->loop);

  return TRUE;
}

static GstStateChangeReturn
gst_pulsevideo_src_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstPulsevideoSrc *this = GST_PULSEVIDEO_SRC_CAST (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      this->context = g_main_context_new ();
      g_print ("context %p\n", this->context);
      this->loop = g_main_loop_new (this->context, FALSE);
      this->thread = g_thread_new ("pulsevideo", (GThreadFunc) handle_mainloop, this);
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_pulsevideo_src_open (this);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      /* uncork and start recording */
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      /* stop recording ASAP by corking */
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      g_main_loop_quit (this->loop);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      g_main_loop_quit (this->loop);
      g_thread_join (this->thread);
      g_main_loop_unref (this->loop);
      g_main_context_unref (this->context);
      break;
    default:
      break;
  }

  return ret;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (pulsevideo_src_debug, "pulsevideosrc", 0,
      "Pulsevideo Source");

  return gst_element_register (plugin, "pulsevideosrc", GST_RANK_NONE,
      GST_TYPE_PULSEVIDEO_SRC);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    pulsevideo,
    "Uses pulsevideo to create a video stream",
    plugin_init, VERSION, "LGPL", "pulsevideo", "pulsevideo.org")
