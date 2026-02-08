/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <unistd.h>

#include <gst/gst.h>

#include <gst/allocators/gstfdmemory.h>
#include <gst/allocators/gstdmabuf.h>
#ifdef HAVE_GSTREAMER_SHM_ALLOCATOR
#include <gst/allocators/gstshmallocator.h>
#endif

#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>

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
  g_free (data);
}

void gst_pipewire_pool_wrap_buffer (GstPipeWirePool *pool, struct pw_buffer *b)
{
  GstBuffer *buf;
  uint32_t i;
  GstPipeWirePoolData *data;
  /* Default to a large enough value */
  gsize plane_0_size = pool->has_rawvideo ?
    pool->video_info.size :
    (gsize) pool->video_info.width * pool->video_info.height;
  gsize plane_sizes[GST_VIDEO_MAX_PLANES] = { plane_0_size, };

  GST_DEBUG_OBJECT (pool, "wrap buffer, datas:%d", b->buffer->n_datas);

  data = g_new0 (GstPipeWirePoolData, 1);

  buf = gst_buffer_new ();

  if (pool->add_metavideo) {
    GstVideoMeta *meta = gst_buffer_add_video_meta_full (buf,
        GST_VIDEO_FRAME_FLAG_NONE,
        GST_VIDEO_INFO_FORMAT (&pool->video_info),
        GST_VIDEO_INFO_WIDTH (&pool->video_info),
        GST_VIDEO_INFO_HEIGHT (&pool->video_info),
        GST_VIDEO_INFO_N_PLANES (&pool->video_info),
        pool->video_info.offset,
        pool->video_info.stride);

    gst_video_meta_set_alignment (meta, pool->video_align);

    if (!gst_video_meta_get_plane_size (meta, plane_sizes)) {
      GST_ERROR_OBJECT (pool, "could not compute plane sizes");
    }

    /*
     * We need to set the video meta as pooled, else gst_buffer_pool_release_buffer
     * will call reset_buffer and the default_reset_buffer implementation for
     * GstBufferPool removes all metadata without the POOLED flag.
     */
    GST_META_FLAG_SET (meta, GST_META_FLAG_POOLED);
  }

  for (i = 0; i < b->buffer->n_datas; i++) {
    struct spa_data *d = &b->buffer->datas[i];
    GstMemory *gmem = NULL;

    GST_DEBUG_OBJECT (pool, "wrap data (%s %d) %d %d",
        spa_debug_type_find_short_name(spa_type_data_type, d->type), d->type,
        d->mapoffset, d->maxsize);

    if (pool->allocate_memory) {
#ifdef HAVE_GSTREAMER_SHM_ALLOCATOR
      gsize block_size = d->maxsize;

      if (pool->has_video) {
        /* For video, we know block sizes from the video info already */
        block_size = plane_sizes[i];
      } else {
        /* For audio, reserve space based on the quantum limit and channel count */
        g_autoptr (GstPipeWireStream) s = g_weak_ref_get (&pool->stream);

        struct pw_context *context =  pw_core_get_context(pw_stream_get_core(s->pwstream));
        const struct pw_properties *props = pw_context_get_properties(context);
        uint32_t quantum_limit = 8192; /* "reasonable" default */

        const char *quantum = spa_dict_lookup(&props->dict, "clock.quantum-limit");
        if (!quantum) {
          quantum = spa_dict_lookup(&props->dict, "default.clock.quantum-limit");
          GST_DEBUG_OBJECT (pool, "using default quantum limit %s", quantum);
        }

        if (quantum)
          spa_atou32(quantum, &quantum_limit, 0);
        GST_DEBUG_OBJECT (pool, "quantum limit %s", quantum);

        block_size = quantum_limit * pool->audio_info.bpf;
      }

      GST_DEBUG_OBJECT (pool, "setting block size %zu", block_size);

      if (!pool->shm_allocator)
        pool->shm_allocator = gst_shm_allocator_get();

      /* use MemFd only. That is the only supported data type when memory is remote i.e. allocated by the client */
      gmem = gst_allocator_alloc (pool->shm_allocator, block_size, NULL);
      d->fd = gst_fd_memory_get_fd (gmem);
      d->mapoffset = 0;
      d->flags = SPA_DATA_FLAG_READWRITE | SPA_DATA_FLAG_MAPPABLE;

      d->type = SPA_DATA_MemFd;
      d->maxsize = block_size;
      d->data = NULL;
#endif
    } else if (d->type == SPA_DATA_MemFd) {
      gmem = gst_fd_allocator_alloc (pool->fd_allocator, dup(d->fd),
                d->mapoffset + d->maxsize, GST_FD_MEMORY_FLAG_NONE);
      gst_memory_resize (gmem, d->mapoffset, d->maxsize);
    }
    else if(d->type == SPA_DATA_DmaBuf) {
      GstMapInfo info = { 0 };
      GstFdMemoryFlags fd_flags = GST_FD_MEMORY_FLAG_NONE;

      if (d->flags & SPA_DATA_FLAG_MAPPABLE && d->flags & SPA_DATA_FLAG_READABLE)
        fd_flags |= GST_FD_MEMORY_FLAG_KEEP_MAPPED;

      gmem = gst_fd_allocator_alloc (pool->dmabuf_allocator, dup(d->fd),
        d->mapoffset + d->maxsize, fd_flags);
      gst_memory_resize (gmem, d->mapoffset, d->maxsize);

      if (fd_flags & GST_FD_MEMORY_FLAG_KEEP_MAPPED) {
        GstMapFlags map_flags = GST_MAP_READ;

        if (d->flags & SPA_DATA_FLAG_WRITABLE)
          map_flags |= GST_MAP_WRITE;

        if (gst_memory_map (gmem, &info, map_flags)) {
          gst_memory_unmap (gmem, &info);
        } else {
          GST_ERROR_OBJECT (pool, "mmaping buffer failed");
        }
      }
    }
    else if (d->type == SPA_DATA_MemPtr) {
      gmem = gst_memory_new_wrapped (0, d->data, d->maxsize, 0,
                                     d->maxsize, NULL, NULL);
    }
    else {
      GST_WARNING_OBJECT (pool, "unknown data type (%s %d)",
        spa_debug_type_find_short_name(spa_type_data_type, d->type), d->type);
    }
    if (gmem)
      gst_buffer_insert_memory (buf, i, gmem);
  }

  if (pool->add_metavideo && !pool->allocate_memory) {
      /* Set memory sizes to expected plane sizes, so we know the valid size,
       * and the offsets in the meta make sense */
      for (i = 0; i < gst_buffer_n_memory (buf); i++) {
        GstMemory *mem = gst_buffer_peek_memory (buf, i);
        gst_memory_resize (mem, 0, plane_sizes[i]);
    }
  }

  data->pool = gst_object_ref (pool);
  data->owner = NULL;
  data->header = spa_buffer_find_meta_data (b->buffer, SPA_META_Header, sizeof(*data->header));
  data->flags = GST_BUFFER_FLAGS (buf);
  data->b = b;
  data->buf = buf;
  data->crop = spa_buffer_find_meta_data (b->buffer, SPA_META_VideoCrop, sizeof(*data->crop));
  data->videotransform =
    spa_buffer_find_meta_data (b->buffer, SPA_META_VideoTransform, sizeof(*data->videotransform));
  data->cursor = spa_buffer_find_meta_data (b->buffer, SPA_META_Cursor, sizeof(*data->cursor));

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

  if (!pool->allocate_memory)
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
  static const gchar *options[] = {
    GST_BUFFER_POOL_OPTION_VIDEO_META,
#ifdef HAVE_GSTREAMER_SHM_ALLOCATOR
    GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT,
#endif
    NULL
  };
  return options;
}

static gboolean
set_config (GstBufferPool * pool, GstStructure * config)
{
  GstPipeWirePool *p = GST_PIPEWIRE_POOL (pool);
  GstCaps *caps;
  GstStructure *structure;
  guint size, min_buffers, max_buffers;
  gboolean has_videoalign;

  if (!gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers, &max_buffers)) {
    GST_WARNING_OBJECT (pool, "invalid config");
    return FALSE;
  }

  if (caps == NULL) {
    GST_WARNING_OBJECT (pool, "no caps in config");
    return FALSE;
  }

  /* We don't support unlimited buffers */
  if (max_buffers == 0)
    max_buffers = PIPEWIRE_POOL_MAX_BUFFERS;
  /* Pick a sensible min to avoid starvation */
  if (min_buffers == 0)
    min_buffers = PIPEWIRE_POOL_MIN_BUFFERS;

  if (min_buffers < PIPEWIRE_POOL_MIN_BUFFERS || max_buffers > PIPEWIRE_POOL_MAX_BUFFERS)
    return FALSE;

  structure = gst_caps_get_structure (caps, 0);
  if (g_str_has_prefix (gst_structure_get_name (structure), "video/") ||
      g_str_has_prefix (gst_structure_get_name (structure), "image/")) {
    p->has_video = TRUE;

    gst_video_info_from_caps (&p->video_info, caps);

    if (GST_VIDEO_FORMAT_INFO_IS_VALID_RAW (p->video_info.finfo)
#ifdef HAVE_GSTREAMER_DMA_DRM
        && GST_VIDEO_FORMAT_INFO_FORMAT (p->video_info.finfo) != GST_VIDEO_FORMAT_DMA_DRM
#endif
        )
      p->has_rawvideo = TRUE;
    else
      p->has_rawvideo = FALSE;

#ifdef HAVE_GSTREAMER_SHM_ALLOCATOR
    if (p->has_rawvideo) {
      gst_video_alignment_reset (&p->video_align);
      gst_video_info_align (&p->video_info, &p->video_align);
    }
#endif
  } else if (g_str_has_prefix(gst_structure_get_name(structure), "audio/")) {
    p->has_video = FALSE;
    gst_audio_info_from_caps(&p->audio_info, caps);
  } else {
    g_assert_not_reached ();
  }

  p->add_metavideo = p->has_rawvideo && gst_buffer_pool_config_has_option (config,
      GST_BUFFER_POOL_OPTION_VIDEO_META);

#ifdef HAVE_GSTREAMER_SHM_ALLOCATOR
  has_videoalign = p->has_rawvideo && gst_buffer_pool_config_has_option (config,
      GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);

  if (has_videoalign) {
    gst_buffer_pool_config_get_video_alignment (config, &p->video_align);
    gst_video_info_align (&p->video_info, &p->video_align);
    gst_buffer_pool_config_set_video_alignment (config, &p->video_align);

    GST_LOG_OBJECT (pool, "Set alignment: %u-%ux%u-%u",
        p->video_align.padding_left, p->video_align.padding_right,
        p->video_align.padding_top, p->video_align.padding_bottom);
  }
#endif

  if (p->video_info.size != 0)
    size = p->video_info.size;

  gst_buffer_pool_config_set_params (config, caps, size, min_buffers, max_buffers);

  return GST_BUFFER_POOL_CLASS (gst_pipewire_pool_parent_class)->set_config (pool, config);
}


void gst_pipewire_pool_set_paused (GstPipeWirePool *pool, gboolean paused)
{
  GST_DEBUG_OBJECT (pool, "pause: %u", paused);
  GST_OBJECT_LOCK (pool);
  pool->paused = paused;
  g_cond_signal (&pool->cond);
  GST_OBJECT_UNLOCK (pool);
}

static void
flush_start (GstBufferPool * pool)
{
  GstPipeWirePool *p = GST_PIPEWIRE_POOL (pool);

  GST_DEBUG_OBJECT (pool, "flush start");
  GST_OBJECT_LOCK (pool);
  g_cond_signal (&p->cond);
  GST_OBJECT_UNLOCK (pool);
}

static void
release_buffer (GstBufferPool * pool, GstBuffer *buffer)
{
  GST_LOG_OBJECT (pool, "release buffer %p", buffer);

  GstPipeWirePoolData *data = gst_pipewire_pool_get_data(buffer);
  GstPipeWirePool *p = GST_PIPEWIRE_POOL (pool);
  g_autoptr (GstPipeWireStream) s = g_weak_ref_get (&p->stream);

  GST_OBJECT_LOCK (pool);
  pw_thread_loop_lock (s->core->loop);

  if (!data->queued && data->b != NULL)
  {
    int res;

    if ((res = pw_stream_return_buffer (s->pwstream, data->b)) < 0) {
      GST_ERROR_OBJECT (pool,"can't return buffer %p; gstbuffer : %p, %s",data->b, buffer, spa_strerror(res));
    } else {
      data->queued = TRUE;
      GST_DEBUG_OBJECT (pool, "returned buffer %p; gstbuffer:%p", data->b, buffer);
    }
  }

  pw_thread_loop_unlock (s->core->loop);
  GST_OBJECT_UNLOCK (pool);
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
#ifdef HAVE_GSTREAMER_SHM_ALLOCATOR
  if (pool->shm_allocator)
    g_object_unref (pool->shm_allocator);
#endif

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
#ifdef HAVE_GSTREAMER_SHM_ALLOCATOR
  gst_shm_allocator_init_once();
#endif
  g_cond_init (&pool->cond);
}
