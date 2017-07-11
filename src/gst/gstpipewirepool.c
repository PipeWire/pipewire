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

#include "gstpipewirepool.h"

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

GstPipeWirePool *
gst_pipewire_pool_new (void)
{
  GstPipeWirePool *pool;

  pool = g_object_new (GST_TYPE_PIPEWIRE_POOL, NULL);

  return pool;
}

gboolean
gst_pipewire_pool_add_buffer (GstPipeWirePool *pool, GstBuffer *buffer)
{
  g_return_val_if_fail (GST_IS_PIPEWIRE_POOL (pool), FALSE);
  g_return_val_if_fail (GST_IS_BUFFER (buffer), FALSE);

  GST_OBJECT_LOCK (pool);
  g_queue_push_tail (&pool->available, buffer);
  g_cond_signal (&pool->cond);
  GST_OBJECT_UNLOCK (pool);

  return TRUE;
}

gboolean
gst_pipewire_pool_remove_buffer (GstPipeWirePool *pool, GstBuffer *buffer)
{
  gboolean res;

  g_return_val_if_fail (GST_IS_PIPEWIRE_POOL (pool), FALSE);
  g_return_val_if_fail (GST_IS_BUFFER (buffer), FALSE);

  GST_OBJECT_LOCK (pool);
  res = g_queue_remove (&pool->available, buffer);
  GST_OBJECT_UNLOCK (pool);

  return res;
}

static GstFlowReturn
acquire_buffer (GstBufferPool * pool, GstBuffer ** buffer,
        GstBufferPoolAcquireParams * params)
{
  GstPipeWirePool *p = GST_PIPEWIRE_POOL (pool);

  GST_OBJECT_LOCK (pool);
  while (TRUE) {
    if (G_UNLIKELY (GST_BUFFER_POOL_IS_FLUSHING (pool)))
      goto flushing;

    if (p->available.length > 0)
      break;

    GST_WARNING ("queue empty");
    g_cond_wait (&p->cond, GST_OBJECT_GET_LOCK (pool));
  }
  *buffer = g_queue_pop_head (&p->available);
  GST_OBJECT_UNLOCK (pool);
  GST_DEBUG ("acquire buffer %p", *buffer);

  return GST_FLOW_OK;

flushing:
  {
    GST_OBJECT_UNLOCK (pool);
    return GST_FLOW_FLUSHING;
  }
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
  GstPipeWirePool *p = GST_PIPEWIRE_POOL (pool);

  GST_DEBUG ("release buffer %p", buffer);
  GST_OBJECT_LOCK (pool);
  g_queue_push_tail (&p->available, buffer);
  g_cond_signal (&p->cond);
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

  G_OBJECT_CLASS (gst_pipewire_pool_parent_class)->finalize (object);
}

static void
gst_pipewire_pool_class_init (GstPipeWirePoolClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBufferPoolClass *bufferpool_class = GST_BUFFER_POOL_CLASS (klass);

  gobject_class->finalize = gst_pipewire_pool_finalize;

  bufferpool_class->start = do_start;
  bufferpool_class->flush_start = flush_start;
  bufferpool_class->acquire_buffer = acquire_buffer;
  bufferpool_class->release_buffer = release_buffer;

  pool_signals[ACTIVATED] =
      g_signal_new ("activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      0, NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 0, G_TYPE_NONE);

  GST_DEBUG_CATEGORY_INIT (gst_pipewire_pool_debug_category, "pipewirepool", 0,
      "debug category for pipewirepool object");
}

static void
gst_pipewire_pool_init (GstPipeWirePool * pool)
{
  g_cond_init (&pool->cond);
  g_queue_init (&pool->available);
}
