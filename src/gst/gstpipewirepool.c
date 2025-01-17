/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <unistd.h>

#include <gst/gst.h>

#include <gst/allocators/gstfdmemory.h>
#include <gst/allocators/gstdmabuf.h>

#include <gst/video/gstvideometa.h>

#include "gstpipewirepool.h"

#include <spa/debug/types.h>
#include <spa/utils/result.h>


GST_DEBUG_CATEGORY_STATIC (gst_pipewire_pool_debug_category);
#define GST_CAT_DEFAULT gst_pipewire_pool_debug_category

G_DEFINE_TYPE (GstPipeWirePool, gst_pipewire_pool, GST_TYPE_BUFFER_POOL);

enum
{
  ACTIVATED,
  /* FILL ME */
  LAST_SIGNAL
};


static guint pool_signals[LAST_SIGNAL] = { 0 };

static GQuark pool_data_quark;

GstPipeWirePool *
gst_pipewire_pool_new (GstPipeWireStream *stream)
{
  GstPipeWirePool *pool;

  pool = g_object_new (GST_TYPE_PIPEWIRE_POOL, NULL);
  g_weak_ref_set (&pool->stream, stream);

  return pool;
}

static void
pool_data_destroy (gpointer user_data)
{
  GstPipeWirePoolData *data = user_data;

  gst_object_unref (data->pool);
  g_slice_free (GstPipeWirePoolData, data);
}

void gst_pipewire_pool_wrap_buffer (GstPipeWirePool *pool, struct pw_buffer *b)
{
  GstBuffer *buf;
  uint32_t i;
  GstPipeWirePoolData *data;

  GST_DEBUG_OBJECT (pool, "wrap buffer");

  data = g_slice_new (GstPipeWirePoolData);

  buf = gst_buffer_new ();

  for (i = 0; i < b->buffer->n_datas; i++) {
    struct spa_data *d = &b->buffer->datas[i];
    GstMemory *gmem = NULL;

    GST_DEBUG_OBJECT (pool, "wrap data (%s %d) %d %d",
        spa_debug_type_find_short_name(spa_type_data_type, d->type), d->type,
        d->mapoffset, d->maxsize);
    if (d->type == SPA_DATA_MemFd) {
      gmem = gst_fd_allocator_alloc (pool->fd_allocator, dup(d->fd),
                d->mapoffset + d->maxsize, GST_FD_MEMORY_FLAG_NONE);
      gst_memory_resize (gmem, d->mapoffset, d->maxsize);
    }
    else if(d->type == SPA_DATA_DmaBuf) {
      gmem = gst_fd_allocator_alloc (pool->dmabuf_allocator, dup(d->fd),
                d->mapoffset + d->maxsize, GST_FD_MEMORY_FLAG_NONE);
      gst_memory_resize (gmem, d->mapoffset, d->maxsize);
    }
    else if (d->type == SPA_DATA_MemPtr) {
      gmem = gst_memory_new_wrapped (0, d->data, d->maxsize, 0,
                                     d->maxsize, NULL, NULL);
    }
    if (gmem)
      gst_buffer_insert_memory (buf, i, gmem);
  }

  if (pool->add_metavideo) {
    gst_buffer_add_video_meta_full (buf, GST_VIDEO_FRAME_FLAG_NONE,
        GST_VIDEO_INFO_FORMAT (&pool->video_info),
        GST_VIDEO_INFO_WIDTH (&pool->video_info),
        GST_VIDEO_INFO_HEIGHT (&pool->video_info),
        GST_VIDEO_INFO_N_PLANES (&pool->video_info),
        pool->video_info.offset,
        pool->video_info.stride);
  }

  data->pool = gst_object_ref (pool);
  data->owner = NULL;
  data->header = spa_buffer_find_meta_data (b->buffer, SPA_META_Header, sizeof(*data->header));
  data->flags = GST_BUFFER_FLAGS (buf);
  data->b = b;
  data->buf = buf;
  data->crop = spa_buffer_find_meta_data (b->buffer, SPA_META_VideoCrop, sizeof(*data->crop));
  if (data->crop)
	  gst_buffer_add_video_crop_meta(buf);
  data->videotransform =
    spa_buffer_find_meta_data (b->buffer, SPA_META_VideoTransform, sizeof(*data->videotransform));

  gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (buf),
                             pool_data_quark,
                             data,
                             pool_data_destroy);
  b->user_data = data;

  pool->n_buffers++;
}

void gst_pipewire_pool_remove_buffer (GstPipeWirePool *pool, struct pw_buffer *b)
{
  GstPipeWirePoolData *data = b->user_data;

  data->b = NULL;
  data->header = NULL;
  data->crop = NULL;
  data->videotransform = NULL;

  gst_buffer_remove_all_memory (data->buf);

  /* this will also destroy the pool data, if this is the last reference */
  gst_clear_buffer (&data->buf);

  pool->n_buffers--;
}

GstPipeWirePoolData *gst_pipewire_pool_get_data (GstBuffer *buffer)
{
  return gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (buffer), pool_data_quark);
}

static GstFlowReturn
acquire_buffer (GstBufferPool * pool, GstBuffer ** buffer,
        GstBufferPoolAcquireParams * params)
{
  GstPipeWirePool *p = GST_PIPEWIRE_POOL (pool);
  g_autoptr (GstPipeWireStream) s = g_weak_ref_get (&p->stream);
  GstPipeWirePoolData *data;
  struct pw_buffer *b;

  if (G_UNLIKELY (!s))
    return GST_FLOW_ERROR;

  GST_OBJECT_LOCK (pool);
  while (TRUE) {
    if (G_UNLIKELY (GST_BUFFER_POOL_IS_FLUSHING (pool)))
      goto flushing;

    if ((b = pw_stream_dequeue_buffer(s->pwstream))) {
      GST_LOG_OBJECT (pool, "dequeued buffer %p", b);
      break;
    }

    if (params) {
      if (params->flags & GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT)
        goto no_more_buffers;

      if ((params->flags & GST_BUFFER_POOL_ACQUIRE_FLAG_LAST) &&
	      p->paused)
        goto paused;
    }

    GST_WARNING_OBJECT (pool, "failed to dequeue buffer: %s", strerror(errno));
    g_cond_wait (&p->cond, GST_OBJECT_GET_LOCK (pool));
  }

  data = b->user_data;
  data->queued = FALSE;

  *buffer = data->buf;

  GST_OBJECT_UNLOCK (pool);
  GST_LOG_OBJECT (pool, "acquired gstbuffer %p", *buffer);

  return GST_FLOW_OK;

flushing:
  {
    GST_OBJECT_UNLOCK (pool);
    return GST_FLOW_FLUSHING;
  }
paused:
  {
    GST_OBJECT_UNLOCK (pool);
    return GST_FLOW_CUSTOM_ERROR_1;
  }
no_more_buffers:
  {
    GST_LOG_OBJECT (pool, "no more buffers");
    GST_OBJECT_UNLOCK (pool);
    return GST_FLOW_EOS;
  }
}

static const gchar **
get_options (GstBufferPool * pool)
{
  static const gchar *options[] = { GST_BUFFER_POOL_OPTION_VIDEO_META, NULL };
  return options;
}

static gboolean
set_config (GstBufferPool * pool, GstStructure * config)
{
  GstPipeWirePool *p = GST_PIPEWIRE_POOL (pool);
  GstCaps *caps;
  GstStructure *structure;
  guint size, min_buffers, max_buffers;
  gboolean has_video;

  if (!gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers, &max_buffers)) {
    GST_WARNING_OBJECT (pool, "invalid config");
    return FALSE;
  }

  if (caps == NULL) {
    GST_WARNING_OBJECT (pool, "no caps in config");
    return FALSE;
  }

  structure = gst_caps_get_structure (caps, 0);
  if (g_str_has_prefix (gst_structure_get_name (structure), "video/") ||
      g_str_has_prefix (gst_structure_get_name (structure), "image/")) {
    has_video = TRUE;
    gst_video_info_from_caps (&p->video_info, caps);
  } else {
    has_video = FALSE;
  }

  p->add_metavideo = has_video && gst_buffer_pool_config_has_option (config,
      GST_BUFFER_POOL_OPTION_VIDEO_META);

  if (p->video_info.size != 0)
    size = p->video_info.size;

  gst_buffer_pool_config_set_params (config, caps, size, min_buffers, max_buffers);

  return GST_BUFFER_POOL_CLASS (gst_pipewire_pool_parent_class)->set_config (pool, config);
}


void gst_pipewire_pool_set_paused (GstPipeWirePool *pool, gboolean paused)
{
  GST_DEBUG ("flush start");
  GST_OBJECT_LOCK (pool);
  pool->paused = paused;
  g_cond_signal (&pool->cond);
  GST_OBJECT_UNLOCK (pool);
}

static void
flush_start (GstBufferPool * pool)
{
  GstPipeWirePool *p = GST_PIPEWIRE_POOL (pool);

  GST_DEBUG ("flush start");
  GST_OBJECT_LOCK (pool);
  g_cond_signal (&p->cond);
  GST_OBJECT_UNLOCK (pool);
}

static void
release_buffer (GstBufferPool * pool, GstBuffer *buffer)
{
  GST_LOG_OBJECT (pool, "release buffer %p", buffer);

  GstPipeWirePoolData *data = gst_pipewire_pool_get_data(buffer);

  if (!data->queued && data->b != NULL)
  {
    GstPipeWirePool *p = GST_PIPEWIRE_POOL (pool);
    GST_OBJECT_LOCK (pool);

    g_autoptr (GstPipeWireStream) s = g_weak_ref_get (&p->stream);
    int res;

    pw_thread_loop_lock (s->core->loop);
    if ((res = pw_stream_return_buffer (s->pwstream, data->b)) < 0) {
      GST_ERROR_OBJECT (pool,"can't return buffer %p; gstbuffer : %p, %s",data->b, buffer, spa_strerror(res));
    } else {
      data->queued = TRUE;
      GST_DEBUG_OBJECT (pool, "returned buffer %p; gstbuffer:%p", data->b, buffer);
    }
    pw_thread_loop_unlock (s->core->loop);
    GST_OBJECT_UNLOCK (pool);

  }
}

static gboolean
do_start (GstBufferPool * pool)
{
  g_signal_emit (pool, pool_signals[ACTIVATED], 0, NULL);
  return TRUE;
}

static void
gst_pipewire_pool_finalize (GObject * object)
{
  GstPipeWirePool *pool = GST_PIPEWIRE_POOL (object);

  GST_DEBUG_OBJECT (pool, "finalize");
  g_weak_ref_set (&pool->stream, NULL);
  g_object_unref (pool->fd_allocator);
  g_object_unref (pool->dmabuf_allocator);

  G_OBJECT_CLASS (gst_pipewire_pool_parent_class)->finalize (object);
}

static void
gst_pipewire_pool_class_init (GstPipeWirePoolClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBufferPoolClass *bufferpool_class = GST_BUFFER_POOL_CLASS (klass);

  gobject_class->finalize = gst_pipewire_pool_finalize;

  bufferpool_class->get_options = get_options;
  bufferpool_class->set_config = set_config;
  bufferpool_class->start = do_start;
  bufferpool_class->flush_start = flush_start;
  bufferpool_class->acquire_buffer = acquire_buffer;
  bufferpool_class->release_buffer = release_buffer;

  pool_signals[ACTIVATED] =
      g_signal_new ("activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      0, NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 0, G_TYPE_NONE);

  GST_DEBUG_CATEGORY_INIT (gst_pipewire_pool_debug_category, "pipewirepool", 0,
      "debug category for pipewirepool object");

  pool_data_quark = g_quark_from_static_string ("GstPipeWirePoolDataQuark");
}

static void
gst_pipewire_pool_init (GstPipeWirePool * pool)
{
  pool->fd_allocator = gst_fd_allocator_new ();
  pool->dmabuf_allocator = gst_dmabuf_allocator_new ();
  g_cond_init (&pool->cond);
}
