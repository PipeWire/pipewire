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
 * SECTION:element-pinossrc
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v pinossrc ! videoconvert ! ximagesink
 * ]| Shows pinos output in an X window.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "gstpinossrc.h"
#include "gstpinosformat.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gio/gunixfdmessage.h>
#include <gst/net/gstnetclientclock.h>
#include <gst/allocators/gstfdmemory.h>
#include <gst/video/video.h>

#include <spa/buffer.h>

#include "gstpinosclock.h"

static GQuark process_mem_data_quark;

GST_DEBUG_CATEGORY_STATIC (pinos_src_debug);
#define GST_CAT_DEFAULT pinos_src_debug

enum
{
  PROP_0,
  PROP_PATH,
  PROP_CLIENT_NAME,
  PROP_STREAM_PROPERTIES,
};


#define PINOSS_VIDEO_CAPS GST_VIDEO_CAPS_MAKE (GST_VIDEO_FORMATS_ALL)

static GstStaticPadTemplate gst_pinos_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );

#define gst_pinos_src_parent_class parent_class
G_DEFINE_TYPE (GstPinosSrc, gst_pinos_src, GST_TYPE_PUSH_SRC);

static GstStateChangeReturn
gst_pinos_src_change_state (GstElement * element, GstStateChange transition);

static gboolean gst_pinos_src_negotiate (GstBaseSrc * basesrc);
static GstCaps *gst_pinos_src_src_fixate (GstBaseSrc * bsrc,
    GstCaps * caps);

static GstFlowReturn gst_pinos_src_create (GstPushSrc * psrc,
    GstBuffer ** buffer);
static gboolean gst_pinos_src_unlock (GstBaseSrc * basesrc);
static gboolean gst_pinos_src_unlock_stop (GstBaseSrc * basesrc);
static gboolean gst_pinos_src_start (GstBaseSrc * basesrc);
static gboolean gst_pinos_src_stop (GstBaseSrc * basesrc);
static gboolean gst_pinos_src_event (GstBaseSrc * src, GstEvent * event);
static gboolean gst_pinos_src_query (GstBaseSrc * src, GstQuery * query);

static void
gst_pinos_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPinosSrc *pinossrc = GST_PINOS_SRC (object);

  switch (prop_id) {
    case PROP_PATH:
      g_free (pinossrc->path);
      pinossrc->path = g_value_dup_string (value);
      break;

    case PROP_CLIENT_NAME:
      g_free (pinossrc->client_name);
      pinossrc->client_name = g_value_dup_string (value);
      break;

    case PROP_STREAM_PROPERTIES:
      if (pinossrc->properties)
        gst_structure_free (pinossrc->properties);
      pinossrc->properties =
          gst_structure_copy (gst_value_get_structure (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pinos_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPinosSrc *pinossrc = GST_PINOS_SRC (object);

  switch (prop_id) {
    case PROP_PATH:
      g_value_set_string (value, pinossrc->path);
      break;

    case PROP_CLIENT_NAME:
      g_value_set_string (value, pinossrc->client_name);
      break;

    case PROP_STREAM_PROPERTIES:
      gst_value_set_structure (value, pinossrc->properties);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstClock *
gst_pinos_src_provide_clock (GstElement * elem)
{
  GstPinosSrc *pinossrc = GST_PINOS_SRC (elem);
  GstClock *clock;

  GST_OBJECT_LOCK (pinossrc);
  if (!GST_OBJECT_FLAG_IS_SET (pinossrc, GST_ELEMENT_FLAG_PROVIDE_CLOCK))
    goto clock_disabled;

  if (pinossrc->clock && pinossrc->is_live)
    clock = GST_CLOCK_CAST (gst_object_ref (pinossrc->clock));
  else
    clock = NULL;
  GST_OBJECT_UNLOCK (pinossrc);

  return clock;

  /* ERRORS */
clock_disabled:
  {
    GST_DEBUG_OBJECT (pinossrc, "clock provide disabled");
    GST_OBJECT_UNLOCK (pinossrc);
    return NULL;
  }
}

static void
clear_queue (GstPinosSrc *pinossrc)
{
  g_queue_foreach (&pinossrc->queue, (GFunc) gst_mini_object_unref, NULL);
  g_queue_clear (&pinossrc->queue);
}

static void
gst_pinos_src_finalize (GObject * object)
{
  GstPinosSrc *pinossrc = GST_PINOS_SRC (object);

  clear_queue (pinossrc);

  pinos_thread_main_loop_destroy (pinossrc->main_loop);
  pinossrc->main_loop = NULL;
  pinos_loop_destroy (pinossrc->loop);
  pinossrc->loop = NULL;

  if (pinossrc->properties)
    gst_structure_free (pinossrc->properties);
  g_object_unref (pinossrc->fd_allocator);
  if (pinossrc->clock)
    gst_object_unref (pinossrc->clock);
  g_free (pinossrc->path);
  g_free (pinossrc->client_name);
  g_hash_table_unref (pinossrc->buf_ids);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_pinos_src_class_init (GstPinosSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSrcClass *gstbasesrc_class;
  GstPushSrcClass *gstpushsrc_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesrc_class = (GstBaseSrcClass *) klass;
  gstpushsrc_class = (GstPushSrcClass *) klass;

  gobject_class->finalize = gst_pinos_src_finalize;
  gobject_class->set_property = gst_pinos_src_set_property;
  gobject_class->get_property = gst_pinos_src_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        "Path",
                                                        "The source path to connect to (NULL = default)",
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
                                   PROP_STREAM_PROPERTIES,
                                   g_param_spec_boxed ("stream-properties",
                                                       "stream properties",
                                                       "list of pinos stream properties",
                                                       GST_TYPE_STRUCTURE,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_STRINGS));

  gstelement_class->provide_clock = gst_pinos_src_provide_clock;
  gstelement_class->change_state = gst_pinos_src_change_state;

  gst_element_class_set_static_metadata (gstelement_class,
      "Pinos source", "Source/Video",
      "Uses pinos to create video", "Wim Taymans <wim.taymans@gmail.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pinos_src_template));

  gstbasesrc_class->negotiate = gst_pinos_src_negotiate;
  gstbasesrc_class->fixate = gst_pinos_src_src_fixate;
  gstbasesrc_class->unlock = gst_pinos_src_unlock;
  gstbasesrc_class->unlock_stop = gst_pinos_src_unlock_stop;
  gstbasesrc_class->start = gst_pinos_src_start;
  gstbasesrc_class->stop = gst_pinos_src_stop;
  gstbasesrc_class->event = gst_pinos_src_event;
  gstbasesrc_class->query = gst_pinos_src_query;
  gstpushsrc_class->create = gst_pinos_src_create;

  GST_DEBUG_CATEGORY_INIT (pinos_src_debug, "pinossrc", 0,
      "Pinos Source");

  process_mem_data_quark = g_quark_from_static_string ("GstPinosSrcProcessMemQuark");
}

static void
gst_pinos_src_init (GstPinosSrc * src)
{
  /* we operate in time */
  gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);

  GST_OBJECT_FLAG_SET (src, GST_ELEMENT_FLAG_PROVIDE_CLOCK);

  g_queue_init (&src->queue);

  src->fd_allocator = gst_fd_allocator_new ();
  src->client_name = pinos_client_name ();
  src->buf_ids = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) gst_buffer_unref);

  src->loop = pinos_loop_new ();
  src->main_loop = pinos_thread_main_loop_new (src->loop, "pinos-main-loop");
  GST_DEBUG ("loop %p, mainloop %p", src->loop, src->main_loop);
}

static GstCaps *
gst_pinos_src_src_fixate (GstBaseSrc * bsrc, GstCaps * caps)
{
  GstStructure *structure;
  const gchar *name;

  caps = gst_caps_make_writable (caps);

  structure = gst_caps_get_structure (caps, 0);
  name = gst_structure_get_name (structure);

  if (g_str_has_prefix (name, "video/") || g_str_has_prefix (name, "image/")) {
    gst_structure_fixate_field_nearest_int (structure, "width", 320);
    gst_structure_fixate_field_nearest_int (structure, "height", 240);
    gst_structure_fixate_field_nearest_fraction (structure, "framerate", 30, 1);


    if (strcmp (name, "video/x-raw") == 0) {
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
    }
  } else if (gst_structure_has_name (structure, "audio/x-raw")) {
    gst_structure_fixate_field_string (structure, "format", "S16LE");
    gst_structure_fixate_field_nearest_int (structure, "channels", 2);
    gst_structure_fixate_field_nearest_int (structure, "rate", 44100);
  }

  caps = GST_BASE_SRC_CLASS (parent_class)->fixate (bsrc, caps);

  return caps;
}

typedef struct {
  GstPinosSrc *src;
  guint id;
  SpaBuffer *buf;
  SpaMetaHeader *header;
  guint flags;
  goffset offset;
} ProcessMemData;

static void
process_mem_data_destroy (gpointer user_data)
{
  ProcessMemData *data = user_data;

  gst_object_unref (data->src);
  g_slice_free (ProcessMemData, data);
}

static gboolean
buffer_recycle (GstMiniObject *obj)
{
  ProcessMemData *data;
  GstPinosSrc *src;

  gst_mini_object_ref (obj);
  data = gst_mini_object_get_qdata (obj,
                                    process_mem_data_quark);
  GST_BUFFER_FLAGS (obj) = data->flags;
  src = data->src;

  GST_LOG_OBJECT (obj, "recycle buffer");
  pinos_thread_main_loop_lock (src->main_loop);
  pinos_stream_recycle_buffer (src->stream, data->id);
  pinos_thread_main_loop_unlock (src->main_loop);

  return FALSE;
}

static void
on_add_buffer (PinosListener *listener,
               PinosStream   *stream,
               guint          id)
{
  GstPinosSrc *pinossrc = SPA_CONTAINER_OF (listener, GstPinosSrc, stream_add_buffer);
  SpaBuffer *b;
  GstBuffer *buf;
  uint32_t i;
  ProcessMemData data;
  PinosContext *ctx = pinossrc->stream->context;

  GST_LOG_OBJECT (pinossrc, "add buffer");

  if (!(b = pinos_stream_peek_buffer (pinossrc->stream, id))) {
    g_warning ("failed to peek buffer");
    return;
  }

  buf = gst_buffer_new ();
  GST_MINI_OBJECT_CAST (buf)->dispose = buffer_recycle;

  data.src = gst_object_ref (pinossrc);
  data.id = id;
  data.buf = b;
  data.header = spa_buffer_find_meta (b, ctx->type.meta.Header);

  for (i = 0; i < b->n_datas; i++) {
    SpaData *d = &b->datas[i];
    GstMemory *gmem = NULL;

    if (d->type == ctx->type.data.MemFd || d->type == ctx->type.data.DmaBuf) {
      gmem = gst_fd_allocator_alloc (pinossrc->fd_allocator, dup (d->fd),
                d->mapoffset + d->maxsize, GST_FD_MEMORY_FLAG_NONE);
      gst_memory_resize (gmem, d->chunk->offset + d->mapoffset, d->chunk->size);
      data.offset = d->mapoffset;
    }
    else if (d->type == ctx->type.data.MemPtr) {
      gmem = gst_memory_new_wrapped (0, d->data, d->maxsize, d->chunk->offset + d->mapoffset,
                d->chunk->size, NULL, NULL);
      data.offset = 0;
    }
    if (gmem)
      gst_buffer_append_memory (buf, gmem);
  }
  data.flags = GST_BUFFER_FLAGS (buf);
  gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (buf),
                             process_mem_data_quark,
                             g_slice_dup (ProcessMemData, &data),
                             process_mem_data_destroy);

  g_hash_table_insert (pinossrc->buf_ids, GINT_TO_POINTER (id), buf);
}

static void
on_remove_buffer (PinosListener *listener,
                  PinosStream   *stream,
                  guint          id)
{
  GstPinosSrc *pinossrc = SPA_CONTAINER_OF (listener, GstPinosSrc, stream_remove_buffer);
  GstBuffer *buf;

  GST_LOG_OBJECT (pinossrc, "remove buffer");
  buf = g_hash_table_lookup (pinossrc->buf_ids, GINT_TO_POINTER (id));
  if (buf) {
    GList *walk;

    GST_MINI_OBJECT_CAST (buf)->dispose = NULL;

    walk = pinossrc->queue.head;
    while (walk) {
      GList *next = walk->next;

      if (walk->data == buf) {
        gst_buffer_unref (buf);
        g_queue_delete_link (&pinossrc->queue, walk);
      }
      walk = next;
    }
    g_hash_table_remove (pinossrc->buf_ids, GINT_TO_POINTER (id));
  }
}

static void
on_new_buffer (PinosListener *listener,
               PinosStream   *stream,
               guint          id)
{
  GstPinosSrc *pinossrc = SPA_CONTAINER_OF (listener, GstPinosSrc, stream_new_buffer);
  GstBuffer *buf;
  ProcessMemData *data;
  SpaMetaHeader *h;
  guint i;

  buf = g_hash_table_lookup (pinossrc->buf_ids, GINT_TO_POINTER (id));
  if (buf == NULL) {
    g_warning ("unknown buffer %d", id);
    return;
  }
  GST_LOG_OBJECT (pinossrc, "got new buffer %p", buf);

  data = gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (buf),
                                  process_mem_data_quark);
  h = data->header;
  if (h) {
    GST_INFO ("pts %" G_GUINT64_FORMAT ", dts_offset %"G_GUINT64_FORMAT, h->pts, h->dts_offset);

    if (GST_CLOCK_TIME_IS_VALID (h->pts)) {
      GST_BUFFER_PTS (buf) = h->pts;
      if (GST_BUFFER_PTS (buf) + h->dts_offset > 0)
        GST_BUFFER_DTS (buf) = GST_BUFFER_PTS (buf) + h->dts_offset;
    }
    GST_BUFFER_OFFSET (buf) = h->seq;
  }
  for (i = 0; i < data->buf->n_datas; i++) {
    SpaData *d = &data->buf->datas[i];
    GstMemory *mem = gst_buffer_peek_memory (buf, i);
    mem->offset = d->chunk->offset + data->offset;
    mem->size = d->chunk->size;
  }
  gst_buffer_ref (buf);
  g_queue_push_tail (&pinossrc->queue, buf);

  pinos_thread_main_loop_signal (pinossrc->main_loop, FALSE);
  return;
}

static void
on_state_changed (PinosListener *listener,
                  PinosStream   *stream)
{
  GstPinosSrc *pinossrc = SPA_CONTAINER_OF (listener, GstPinosSrc, stream_state_changed);
  PinosStreamState state = stream->state;

  GST_DEBUG ("got stream state %s", pinos_stream_state_as_string (state));

  switch (state) {
    case PINOS_STREAM_STATE_UNCONNECTED:
    case PINOS_STREAM_STATE_CONNECTING:
    case PINOS_STREAM_STATE_CONFIGURE:
    case PINOS_STREAM_STATE_READY:
    case PINOS_STREAM_STATE_PAUSED:
    case PINOS_STREAM_STATE_STREAMING:
      break;
    case PINOS_STREAM_STATE_ERROR:
      GST_ELEMENT_ERROR (pinossrc, RESOURCE, FAILED,
          ("stream error: %s", stream->error), (NULL));
      break;
  }
  pinos_thread_main_loop_signal (pinossrc->main_loop, FALSE);
}

static void
parse_stream_properties (GstPinosSrc *pinossrc, PinosProperties *props)
{
  const gchar *var;
  gboolean is_live;

  GST_OBJECT_LOCK (pinossrc);
  var = pinos_properties_get (props, "pinos.latency.is-live");
  is_live = pinossrc->is_live = var ? (atoi (var) == 1) : FALSE;

  var = pinos_properties_get (props, "pinos.latency.min");
  pinossrc->min_latency = var ? (GstClockTime) atoi (var) : 0;

  var = pinos_properties_get (props, "pinos.latency.max");
  pinossrc->max_latency = var ? (GstClockTime) atoi (var) : GST_CLOCK_TIME_NONE;
  GST_OBJECT_UNLOCK (pinossrc);

  GST_DEBUG_OBJECT (pinossrc, "live %d", is_live);

  gst_base_src_set_live (GST_BASE_SRC (pinossrc), is_live);
}

static gboolean
gst_pinos_src_stream_start (GstPinosSrc *pinossrc)
{
  pinos_thread_main_loop_lock (pinossrc->main_loop);
  GST_DEBUG_OBJECT (pinossrc, "doing stream start");
  while (TRUE) {
    PinosStreamState state = pinossrc->stream->state;

    GST_DEBUG_OBJECT (pinossrc, "waiting for STREAMING, now %s", pinos_stream_state_as_string (state));
    if (state == PINOS_STREAM_STATE_STREAMING)
      break;

    if (state == PINOS_STREAM_STATE_ERROR)
      goto start_error;

    if (pinossrc->ctx->state == PINOS_CONTEXT_STATE_ERROR)
      goto start_error;

    pinos_thread_main_loop_wait (pinossrc->main_loop);
  }

  parse_stream_properties (pinossrc, pinossrc->stream->properties);
  pinos_thread_main_loop_unlock (pinossrc->main_loop);

  pinos_thread_main_loop_lock (pinossrc->main_loop);
  GST_DEBUG_OBJECT (pinossrc, "signal started");
  pinossrc->started = TRUE;
  pinos_thread_main_loop_signal (pinossrc->main_loop, FALSE);
  pinos_thread_main_loop_unlock (pinossrc->main_loop);

  return TRUE;

start_error:
  {
    GST_DEBUG_OBJECT (pinossrc, "error starting stream");
    pinos_thread_main_loop_unlock (pinossrc->main_loop);
    return FALSE;
  }
}

static PinosStreamState
wait_negotiated (GstPinosSrc *this)
{
  PinosStreamState state;

  pinos_thread_main_loop_lock (this->main_loop);
  while (TRUE) {
    state = this->stream->state;

    GST_DEBUG_OBJECT (this, "waiting for started signal, state now %s",
        pinos_stream_state_as_string (state));

    if (state == PINOS_STREAM_STATE_ERROR)
      break;

    if (this->ctx->state == PINOS_CONTEXT_STATE_ERROR)
      break;

    if (this->started)
      break;

    pinos_thread_main_loop_wait (this->main_loop);
  }
  GST_DEBUG_OBJECT (this, "got started signal");
  pinos_thread_main_loop_unlock (this->main_loop);

  return state;
}

static gboolean
gst_pinos_src_negotiate (GstBaseSrc * basesrc)
{
  GstPinosSrc *pinossrc = GST_PINOS_SRC (basesrc);
  GstCaps *thiscaps;
  GstCaps *caps = NULL;
  GstCaps *peercaps = NULL;
  gboolean result = FALSE;
  GPtrArray *possible;

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
    caps = peercaps;
    gst_caps_unref (thiscaps);
  } else {
    /* no peer, work with our own caps then */
    caps = thiscaps;
  }
  if (caps == NULL || gst_caps_is_empty (caps))
    goto no_common_caps;

  GST_DEBUG_OBJECT (basesrc, "have common caps: %" GST_PTR_FORMAT, caps);

  /* open a connection with these caps */
  possible = gst_caps_to_format_all (caps);

  /* first disconnect */
  pinos_thread_main_loop_lock (pinossrc->main_loop);
  if (pinossrc->stream->state != PINOS_STREAM_STATE_UNCONNECTED) {
    GST_DEBUG_OBJECT (basesrc, "disconnect capture");
    pinos_stream_disconnect (pinossrc->stream);
    while (TRUE) {
      PinosStreamState state = pinossrc->stream->state;

      GST_DEBUG_OBJECT (basesrc, "waiting for UNCONNECTED, now %s", pinos_stream_state_as_string (state));
      if (state == PINOS_STREAM_STATE_UNCONNECTED)
        break;

      if (state == PINOS_STREAM_STATE_ERROR) {
        g_ptr_array_unref (possible);
        goto connect_error;
      }

      pinos_thread_main_loop_wait (pinossrc->main_loop);
    }
  }

  GST_DEBUG_OBJECT (basesrc, "connect capture with path %s", pinossrc->path);
  pinos_stream_connect (pinossrc->stream,
                        PINOS_DIRECTION_INPUT,
                        PINOS_STREAM_MODE_BUFFER,
                        pinossrc->path,
                        PINOS_STREAM_FLAG_AUTOCONNECT,
                        possible->len,
                        (SpaFormat **)possible->pdata);

  while (TRUE) {
    PinosStreamState state = pinossrc->stream->state;

    GST_DEBUG_OBJECT (basesrc, "waiting for PAUSED, now %s", pinos_stream_state_as_string (state));
    if (state == PINOS_STREAM_STATE_PAUSED ||
        state == PINOS_STREAM_STATE_STREAMING)
      break;

    if (state == PINOS_STREAM_STATE_ERROR)
      goto connect_error;

    if (pinossrc->ctx->state == PINOS_CONTEXT_STATE_ERROR)
      goto connect_error;

    pinos_thread_main_loop_wait (pinossrc->main_loop);
  }
  pinos_thread_main_loop_unlock (pinossrc->main_loop);

  result = gst_pinos_src_stream_start (pinossrc);

  pinossrc->negotiated = result;

  return result;

no_nego_needed:
  {
    GST_DEBUG_OBJECT (basesrc, "no negotiation needed");
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return TRUE;
  }
no_caps:
  {
    GST_ELEMENT_ERROR (basesrc, STREAM, FORMAT,
        ("No supported formats found"),
        ("This element did not produce valid caps"));
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return FALSE;
  }
no_common_caps:
  {
    GST_ELEMENT_ERROR (basesrc, STREAM, FORMAT,
        ("No supported formats found"),
        ("This element does not have formats in common with the peer"));
    if (caps)
      gst_caps_unref (caps);
    return FALSE;
  }
connect_error:
  {
    pinos_thread_main_loop_unlock (pinossrc->main_loop);
    return FALSE;
  }
}

#define PROP(f,key,type,...)                                                    \
          SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
static void
on_format_changed (PinosListener *listener,
                   PinosStream   *stream,
                   SpaFormat     *format)
{
  GstPinosSrc *pinossrc = SPA_CONTAINER_OF (listener, GstPinosSrc, stream_format_changed);
  GstCaps *caps;
  gboolean res;
  PinosContext *ctx = stream->context;

  caps = gst_caps_from_format (format);
  GST_DEBUG_OBJECT (pinossrc, "we got format %" GST_PTR_FORMAT, caps);
  res = gst_base_src_set_caps (GST_BASE_SRC (pinossrc), caps);
  gst_caps_unref (caps);

  if (res) {
    SpaAllocParam *params[1];
    SpaPODBuilder b = { NULL };
    uint8_t buffer[128];
    SpaPODFrame f[2];

    spa_pod_builder_init (&b, buffer, sizeof (buffer));
    spa_pod_builder_object (&b, &f[0], 0, ctx->type.alloc_param_meta_enable.MetaEnable,
        PROP    (&f[1], ctx->type.alloc_param_meta_enable.type, SPA_POD_TYPE_ID, ctx->type.meta.Header),
        PROP    (&f[1], ctx->type.alloc_param_meta_enable.size, SPA_POD_TYPE_INT, sizeof (SpaMetaHeader)));
    params[0] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

    GST_DEBUG_OBJECT (pinossrc, "doing finish format");
    pinos_stream_finish_format (pinossrc->stream, SPA_RESULT_OK, params, 1);
  } else {
    GST_WARNING_OBJECT (pinossrc, "finish format with error");
    pinos_stream_finish_format (pinossrc->stream, SPA_RESULT_INVALID_MEDIA_TYPE, NULL, 0);
  }
}

static gboolean
gst_pinos_src_unlock (GstBaseSrc * basesrc)
{
  GstPinosSrc *pinossrc = GST_PINOS_SRC (basesrc);

  pinos_thread_main_loop_lock (pinossrc->main_loop);
  GST_DEBUG_OBJECT (pinossrc, "setting flushing");
  pinossrc->flushing = TRUE;
  pinos_thread_main_loop_signal (pinossrc->main_loop, FALSE);
  pinos_thread_main_loop_unlock (pinossrc->main_loop);

  return TRUE;
}

static gboolean
gst_pinos_src_unlock_stop (GstBaseSrc * basesrc)
{
  GstPinosSrc *pinossrc = GST_PINOS_SRC (basesrc);

  pinos_thread_main_loop_lock (pinossrc->main_loop);
  GST_DEBUG_OBJECT (pinossrc, "unsetting flushing");
  pinossrc->flushing = FALSE;
  pinos_thread_main_loop_unlock (pinossrc->main_loop);

  return TRUE;
}

static gboolean
gst_pinos_src_event (GstBaseSrc * src, GstEvent * event)
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

#if 0
        pinos_buffer_builder_init (&b);

        refresh.last_id = 0;
        refresh.request_type = all_headers ? 1 : 0;
        refresh.pts = running_time;
        pinos_buffer_builder_add_refresh_request (&b, &refresh);

        pinos_buffer_builder_end (&b, &pbuf);

        GST_OBJECT_LOCK (pinossrc);
        if (pinossrc->stream_state == PINOS_STREAM_STATE_STREAMING) {
          GST_DEBUG_OBJECT (pinossrc, "send refresh request");
          pinos_stream_send_buffer (pinossrc->stream, &pbuf);
        }
        GST_OBJECT_UNLOCK (pinossrc);

        pinos_buffer_unref (&pbuf);
#endif
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
gst_pinos_src_query (GstBaseSrc * src, GstQuery * query)
{
  gboolean res = FALSE;
  GstPinosSrc *pinossrc;

  pinossrc = GST_PINOS_SRC (src);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
      GST_OBJECT_LOCK (pinossrc);
      pinossrc->min_latency = 10000000;
      pinossrc->max_latency = GST_CLOCK_TIME_NONE;
      gst_query_set_latency (query, pinossrc->is_live, pinossrc->min_latency, pinossrc->max_latency);
      GST_OBJECT_UNLOCK (pinossrc);
      res = TRUE;
      break;
    default:
      res = GST_BASE_SRC_CLASS (parent_class)->query (src, query);
      break;
  }
  return res;
}

static GstFlowReturn
gst_pinos_src_create (GstPushSrc * psrc, GstBuffer ** buffer)
{
  GstPinosSrc *pinossrc;
  GstClockTime pts, dts, base_time;

  pinossrc = GST_PINOS_SRC (psrc);

  if (!pinossrc->negotiated)
    goto not_negotiated;

  pinos_thread_main_loop_lock (pinossrc->main_loop);
  while (TRUE) {
    PinosStreamState state;

    if (pinossrc->flushing)
      goto streaming_stopped;

    state = pinossrc->stream->state;
    if (state == PINOS_STREAM_STATE_ERROR)
      goto streaming_error;

    if (state != PINOS_STREAM_STATE_STREAMING)
      goto streaming_stopped;

    *buffer = g_queue_pop_head (&pinossrc->queue);
    GST_DEBUG ("popped buffer %p", *buffer);
    if (*buffer != NULL)
      break;

    pinos_thread_main_loop_wait (pinossrc->main_loop);
  }
  pinos_thread_main_loop_unlock (pinossrc->main_loop);

  if (pinossrc->is_live)
    base_time = GST_ELEMENT_CAST (psrc)->base_time;
  else
    base_time = 0;

  pts = GST_BUFFER_PTS (*buffer);
  dts = GST_BUFFER_DTS (*buffer);

  if (GST_CLOCK_TIME_IS_VALID (pts))
    pts = (pts >= base_time ? pts - base_time : 0);
  if (GST_CLOCK_TIME_IS_VALID (dts))
    dts = (dts >= base_time ? dts - base_time : 0);

  GST_INFO ("pts %" G_GUINT64_FORMAT ", dts %"G_GUINT64_FORMAT
      ", base-time %"GST_TIME_FORMAT" -> %"GST_TIME_FORMAT", %"GST_TIME_FORMAT,
      GST_BUFFER_PTS (*buffer), GST_BUFFER_DTS (*buffer), GST_TIME_ARGS (base_time),
      GST_TIME_ARGS (pts), GST_TIME_ARGS (dts));

  GST_BUFFER_PTS (*buffer) = pts;
  GST_BUFFER_DTS (*buffer) = dts;

  return GST_FLOW_OK;

not_negotiated:
  {
    return GST_FLOW_NOT_NEGOTIATED;
  }
streaming_error:
  {
    pinos_thread_main_loop_unlock (pinossrc->main_loop);
    return GST_FLOW_ERROR;
  }
streaming_stopped:
  {
    pinos_thread_main_loop_unlock (pinossrc->main_loop);
    return GST_FLOW_FLUSHING;
  }
}

static gboolean
gst_pinos_src_start (GstBaseSrc * basesrc)
{
  return TRUE;
}

static gboolean
gst_pinos_src_stop (GstBaseSrc * basesrc)
{
  GstPinosSrc *pinossrc;

  pinossrc = GST_PINOS_SRC (basesrc);

  pinos_thread_main_loop_lock (pinossrc->main_loop);
  clear_queue (pinossrc);
  pinos_thread_main_loop_unlock (pinossrc->main_loop);

  return TRUE;
}

static void
on_ctx_state_changed (PinosListener *listener,
                      PinosContext  *ctx)
{
  GstPinosSrc *pinossrc = SPA_CONTAINER_OF (listener, GstPinosSrc, ctx_state_changed);
  PinosContextState state = ctx->state;

  GST_DEBUG ("got context state %s", pinos_context_state_as_string (state));

  switch (state) {
    case PINOS_CONTEXT_STATE_UNCONNECTED:
    case PINOS_CONTEXT_STATE_CONNECTING:
    case PINOS_CONTEXT_STATE_CONNECTED:
      break;
    case PINOS_CONTEXT_STATE_ERROR:
      GST_ELEMENT_ERROR (pinossrc, RESOURCE, FAILED,
          ("context error: %s", ctx->error), (NULL));
      break;
  }
  pinos_thread_main_loop_signal (pinossrc->main_loop, FALSE);
}

static gboolean
copy_properties (GQuark field_id,
                 const GValue *value,
                 gpointer user_data)
{
  PinosProperties *properties = user_data;

  if (G_VALUE_HOLDS_STRING (value))
    pinos_properties_set (properties,
                          g_quark_to_string (field_id),
                          g_value_get_string (value));
  return TRUE;
}

static gboolean
gst_pinos_src_open (GstPinosSrc * pinossrc)
{
  PinosProperties *props;

  if (pinos_thread_main_loop_start (pinossrc->main_loop) != SPA_RESULT_OK)
    goto mainloop_failed;

  pinos_thread_main_loop_lock (pinossrc->main_loop);
  pinossrc->ctx = pinos_context_new (pinossrc->loop, g_get_application_name (), NULL);

  pinos_signal_add (&pinossrc->ctx->state_changed, &pinossrc->ctx_state_changed, on_ctx_state_changed);

  pinos_context_connect (pinossrc->ctx, PINOS_CONTEXT_FLAG_NO_REGISTRY);

  while (TRUE) {
    PinosContextState state = pinossrc->ctx->state;

    GST_DEBUG ("waiting for CONNECTED, now %s", pinos_context_state_as_string (state));
    if (state == PINOS_CONTEXT_STATE_CONNECTED)
      break;

    if (state == PINOS_CONTEXT_STATE_ERROR)
      goto connect_error;

    pinos_thread_main_loop_wait (pinossrc->main_loop);
  }

  if (pinossrc->properties) {
    props = pinos_properties_new (NULL, NULL);
    gst_structure_foreach (pinossrc->properties, copy_properties, props);
  } else {
    props = NULL;
  }

  pinossrc->stream = pinos_stream_new (pinossrc->ctx, pinossrc->client_name, props);

  pinos_signal_add (&pinossrc->stream->state_changed, &pinossrc->stream_state_changed, on_state_changed);
  pinos_signal_add (&pinossrc->stream->format_changed, &pinossrc->stream_format_changed, on_format_changed);
  pinos_signal_add (&pinossrc->stream->add_buffer, &pinossrc->stream_add_buffer, on_add_buffer);
  pinos_signal_add (&pinossrc->stream->remove_buffer, &pinossrc->stream_remove_buffer, on_remove_buffer);
  pinos_signal_add (&pinossrc->stream->new_buffer, &pinossrc->stream_new_buffer, on_new_buffer);

  pinossrc->clock = gst_pinos_clock_new (pinossrc->stream);
  pinos_thread_main_loop_unlock (pinossrc->main_loop);

  return TRUE;

  /* ERRORS */
mainloop_failed:
  {
    GST_ELEMENT_ERROR (pinossrc, RESOURCE, FAILED, ("error starting mainloop"), (NULL));
    return FALSE;
  }
connect_error:
  {
    pinos_thread_main_loop_unlock (pinossrc->main_loop);
    return FALSE;
  }
}

static void
gst_pinos_src_close (GstPinosSrc * pinossrc)
{
  clear_queue (pinossrc);

  pinos_thread_main_loop_stop (pinossrc->main_loop);

  g_hash_table_remove_all (pinossrc->buf_ids);

  pinos_stream_destroy (pinossrc->stream);
  pinossrc->stream = NULL;

  pinos_context_destroy (pinossrc->ctx);
  pinossrc->ctx = NULL;

  GST_OBJECT_LOCK (pinossrc);
  g_clear_object (&pinossrc->clock);
  GST_OBJECT_UNLOCK (pinossrc);
}

static GstStateChangeReturn
gst_pinos_src_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstPinosSrc *this = GST_PINOS_SRC_CAST (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_pinos_src_open (this))
        goto open_failed;
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
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
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      if (wait_negotiated (this) == PINOS_STREAM_STATE_ERROR)
        goto open_failed;

      if (gst_base_src_is_live (GST_BASE_SRC (element)))
        ret = GST_STATE_CHANGE_NO_PREROLL;
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      this->negotiated = FALSE;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      gst_pinos_src_close (this);
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
