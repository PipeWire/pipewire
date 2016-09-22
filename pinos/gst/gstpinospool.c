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
  GST_OBJECT_UNLOCK (pool);
}

static gboolean
do_start (GstBufferPool * pool)
{
  GstPinosPool *p = GST_PINOS_POOL (pool);
  GstStructure *config;
  GstCaps *caps;
  guint size;
  guint min_buffers;
  guint max_buffers;
  SpaAllocParam *port_params[2];
  SpaAllocParamMetaEnable param_meta_enable;
  SpaAllocParamBuffers param_buffers;

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers, &max_buffers);

  port_params[0] = &param_buffers.param;
  param_buffers.param.type = SPA_ALLOC_PARAM_TYPE_BUFFERS;
  param_buffers.param.size = sizeof (SpaAllocParamBuffers);
  param_buffers.minsize = size;
  param_buffers.stride = 0;
  param_buffers.min_buffers = min_buffers;
  param_buffers.max_buffers = max_buffers;
  param_buffers.align = 16;
  port_params[1] = &param_meta_enable.param;
  param_meta_enable.param.type = SPA_ALLOC_PARAM_TYPE_META_ENABLE;
  param_meta_enable.param.size = sizeof (SpaAllocParamMetaEnable);
  param_meta_enable.type = SPA_META_TYPE_HEADER;

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
