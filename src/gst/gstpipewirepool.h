/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef __GST_PIPEWIRE_POOL_H__
#define __GST_PIPEWIRE_POOL_H__

#include "gstpipewirestream.h"

#include <gst/gst.h>

#include <gst/audio/audio.h>
#include <gst/video/video.h>

#include <pipewire/pipewire.h>

G_BEGIN_DECLS

#define GST_TYPE_PIPEWIRE_POOL (gst_pipewire_pool_get_type())
G_DECLARE_FINAL_TYPE (GstPipeWirePool, gst_pipewire_pool, GST, PIPEWIRE_POOL, GstBufferPool)

#define PIPEWIRE_POOL_MIN_BUFFERS 2u
#define PIPEWIRE_POOL_MAX_BUFFERS 16u

/* Only available in GStreamer 1.22+ */
#ifndef GST_VIDEO_FORMAT_INFO_IS_VALID_RAW
#define GST_VIDEO_FORMAT_INFO_IS_VALID_RAW(info)              \
  (info != NULL && (info)->format > GST_VIDEO_FORMAT_ENCODED)
#endif

typedef struct _GstPipeWirePoolData GstPipeWirePoolData;
struct _GstPipeWirePoolData {
  GstPipeWirePool *pool;
  void *owner;
  struct spa_meta_header *header;
  guint flags;
  struct pw_buffer *b;
  GstBuffer *buf;
  gboolean queued;
  struct spa_meta_region *crop;
  struct spa_meta_videotransform *videotransform;
  struct spa_meta_cursor *cursor;
};

struct _GstPipeWirePool {
  GstBufferPool parent;

  GWeakRef stream;
  guint n_buffers;

  gboolean has_video;
  gboolean has_rawvideo;
  gboolean add_metavideo;
  GstAudioInfo audio_info;
  GstVideoInfo video_info;
  GstVideoAlignment video_align;

  GstAllocator *fd_allocator;
  GstAllocator *dmabuf_allocator;
  GstAllocator *shm_allocator;

  GCond cond;
  gboolean paused;
  gboolean allocate_memory;
};

enum GstPipeWirePoolMode {
    USE_BUFFERPOOL_NO = 0,
    USE_BUFFERPOOL_AUTO,
    USE_BUFFERPOOL_YES
};

GstPipeWirePool *  gst_pipewire_pool_new (GstPipeWireStream *stream);

void gst_pipewire_pool_wrap_buffer (GstPipeWirePool *pool, struct pw_buffer *buffer);
void gst_pipewire_pool_remove_buffer (GstPipeWirePool *pool, struct pw_buffer *buffer);

static inline gboolean
gst_pipewire_pool_has_buffers (GstPipeWirePool *pool)
{
  return pool->n_buffers > 0;
}

GstPipeWirePoolData *gst_pipewire_pool_get_data (GstBuffer *buffer);

void gst_pipewire_pool_set_paused (GstPipeWirePool *pool, gboolean paused);

G_END_DECLS

#endif /* __GST_PIPEWIRE_POOL_H__ */
