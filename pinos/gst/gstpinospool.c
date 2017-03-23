/* GStreamer
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include "gstpinospool.h"

GST_DEBUG_CATEGORY_STATIC (gst_pinos_pool_debug_category);
#define GST_CAT_DEFAULT gst_pinos_pool_debug_category

G_DEFINE_TYPE (GstPinosPool, gst_pinos_pool, GST_TYPE_BUFFER_POOL);

GstPinosPool *
gst_pinos_pool_new (void)
{
  GstPinosPool *pool;

  pool = g_object_new (GST_TYPE_PINOS_POOL, NULL);

  return pool;
}

gboolean
gst_pinos_pool_add_buffer (GstPinosPool *pool, GstBuffer *buffer)
{
  g_return_val_if_fail (GST_IS_PINOS_POOL (pool), FALSE);
  g_return_val_if_fail (GST_IS_BUFFER (buffer), FALSE);

  GST_OBJECT_LOCK (pool);
  g_queue_push_tail (&pool->available, buffer);
  g_cond_signal (&pool->cond);
  GST_OBJECT_UNLOCK (pool);

  return TRUE;
}

gboolean
gst_pinos_pool_remove_buffer (GstPinosPool *pool, GstBuffer *buffer)
{
  g_return_val_if_fail (GST_IS_PINOS_POOL (pool), FALSE);
  g_return_val_if_fail (GST_IS_BUFFER (buffer), FALSE);

  GST_OBJECT_LOCK (pool);
  g_queue_remove (&pool->available, buffer);
  GST_OBJECT_UNLOCK (pool);

  return TRUE;
}

static GstFlowReturn
acquire_buffer (GstBufferPool * pool, GstBuffer ** buffer,
        GstBufferPoolAcquireParams * params)
{
  GstPinosPool *p = GST_PINOS_POOL (pool);

  GST_OBJECT_LOCK (pool);
  while (p->available.length == 0) {
    GST_WARNING ("queue empty");
    g_cond_wait (&p->cond, GST_OBJECT_GET_LOCK (pool));
  }
  *buffer = g_queue_pop_head (&p->available);
  GST_OBJECT_UNLOCK (pool);
  GST_DEBUG ("acquire buffer %p", *buffer);

  return GST_FLOW_OK;
}

static void
release_buffer (GstBufferPool * pool, GstBuffer *buffer)
{
  GstPinosPool *p = GST_PINOS_POOL (pool);

  GST_DEBUG ("release buffer %p", buffer);
  GST_OBJECT_LOCK (pool);
  g_queue_push_tail (&p->available, buffer);
  g_cond_signal (&p->cond);
  GST_OBJECT_UNLOCK (pool);
}

#define PROP(f,key,type,...)                                                    \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_READWRITE,type,1,__VA_ARGS__)
#define PROP_R(f,key,type,...)                                                  \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_READABLE,type,1,__VA_ARGS__)
#define PROP_MM(f,key,type,...)                                                 \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_READWRITE |                     \
                              SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_U_MM(f,key,type,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_READWRITE |                     \
                              SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_EN(f,key,type,n,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_READWRITE |                     \
                              SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)
#define PROP_U_EN(f,key,type,n,...)                                             \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_READWRITE |                     \
                              SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)
static gboolean
do_start (GstBufferPool * pool)
{
  GstPinosPool *p = GST_PINOS_POOL (pool);
  GstStructure *config;
  GstCaps *caps;
  guint size;
  guint min_buffers;
  guint max_buffers;
  SpaAllocParam *port_params[3];
  SpaPODBuilder b = { NULL };
  uint8_t buffer[1024];
  SpaPODFrame f[2];
  PinosContext *ctx = p->stream->context;

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers, &max_buffers);

  spa_pod_builder_init (&b, buffer, sizeof (buffer));
  spa_pod_builder_object (&b, &f[0], 0, ctx->uri.alloc_param_buffers.Buffers,
      PROP    (&f[1], ctx->uri.alloc_param_buffers.size,    SPA_POD_TYPE_INT, size),
      PROP    (&f[1], ctx->uri.alloc_param_buffers.stride,  SPA_POD_TYPE_INT, 0),
      PROP_MM (&f[1], ctx->uri.alloc_param_buffers.buffers, SPA_POD_TYPE_INT, min_buffers, min_buffers, max_buffers),
      PROP    (&f[1], ctx->uri.alloc_param_buffers.align,   SPA_POD_TYPE_INT, 16));
  port_params[0] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

  spa_pod_builder_object (&b, &f[0], 0, ctx->uri.alloc_param_meta_enable.MetaEnable,
      PROP    (&f[1], ctx->uri.alloc_param_meta_enable.type, SPA_POD_TYPE_INT, SPA_META_TYPE_HEADER));
  port_params[1] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

  spa_pod_builder_object (&b, &f[0], 0, ctx->uri.alloc_param_meta_enable.MetaEnable,
      PROP    (&f[1], ctx->uri.alloc_param_meta_enable.type,             SPA_POD_TYPE_INT, SPA_META_TYPE_RINGBUFFER),
      PROP    (&f[1], ctx->uri.alloc_param_meta_enable.ringbufferSize,   SPA_POD_TYPE_INT,
                                                                         size * SPA_MAX (4,
                                                                                SPA_MAX (min_buffers, max_buffers))),
      PROP    (&f[1], ctx->uri.alloc_param_meta_enable.ringbufferStride, SPA_POD_TYPE_INT, 0),
      PROP    (&f[1], ctx->uri.alloc_param_meta_enable.ringbufferBlocks, SPA_POD_TYPE_INT, 1),
      PROP    (&f[1], ctx->uri.alloc_param_meta_enable.ringbufferAlign,  SPA_POD_TYPE_INT, 16));
  port_params[2] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

  pinos_stream_finish_format (p->stream, SPA_RESULT_OK, port_params, 2);

  return TRUE;
}

static void
gst_pinos_pool_finalize (GObject * object)
{
  GstPinosPool *pool = GST_PINOS_POOL (object);

  GST_DEBUG_OBJECT (pool, "finalize");

  G_OBJECT_CLASS (gst_pinos_pool_parent_class)->finalize (object);
}

static void
gst_pinos_pool_class_init (GstPinosPoolClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBufferPoolClass *bufferpool_class = GST_BUFFER_POOL_CLASS (klass);

  gobject_class->finalize = gst_pinos_pool_finalize;

  bufferpool_class->start = do_start;
  bufferpool_class->acquire_buffer = acquire_buffer;
  bufferpool_class->release_buffer = release_buffer;

  GST_DEBUG_CATEGORY_INIT (gst_pinos_pool_debug_category, "pinospool", 0,
      "debug category for pinospool object");
}

static void
gst_pinos_pool_init (GstPinosPool * pool)
{
  g_cond_init (&pool->cond);
  g_queue_init (&pool->available);
}
