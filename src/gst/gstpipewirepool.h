/* GStreamer
 * Copyright (C) <2016> Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __GST_PIPEWIRE_POOL_H__
#define __GST_PIPEWIRE_POOL_H__

#include <gst/gst.h>

#include <pipewire/pipewire.h>

G_BEGIN_DECLS

#define GST_TYPE_PIPEWIRE_POOL \
  (gst_pipewire_pool_get_type())
#define GST_PIPEWIRE_POOL(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PIPEWIRE_POOL,GstPipeWirePool))
#define GST_PIPEWIRE_POOL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PIPEWIRE_POOL,GstPipeWirePoolClass))
#define GST_IS_PIPEWIRE_POOL(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PIPEWIRE_POOL))
#define GST_IS_PIPEWIRE_POOL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PIPEWIRE_POOL))
#define GST_PIPEWIRE_POOL_GET_CLASS(klass) \
  (G_TYPE_INSTANCE_GET_CLASS ((klass), GST_TYPE_PIPEWIRE_POOL, GstPipeWirePoolClass))

typedef struct _GstPipeWirePoolData GstPipeWirePoolData;
typedef struct _GstPipeWirePool GstPipeWirePool;
typedef struct _GstPipeWirePoolClass GstPipeWirePoolClass;

struct _GstPipeWirePoolData {
  GstPipeWirePool *pool;
  void *owner;
  struct spa_meta_header *header;
  guint flags;
  goffset offset;
  struct pw_buffer *b;
  GstBuffer *buf;
};

struct _GstPipeWirePool {
  GstBufferPool parent;

  struct pw_stream *stream;
  struct pw_type *t;

  GstAllocator *fd_allocator;
  GstAllocator *dmabuf_allocator;

  GCond cond;
};

struct _GstPipeWirePoolClass {
  GstBufferPoolClass parent_class;
};

GType gst_pipewire_pool_get_type (void);

GstPipeWirePool *  gst_pipewire_pool_new           (void);

void gst_pipewire_pool_wrap_buffer (GstPipeWirePool *pool, struct pw_buffer *buffer);

GstPipeWirePoolData *gst_pipewire_pool_get_data (GstBuffer *buffer);

//gboolean        gst_pipewire_pool_add_buffer    (GstPipeWirePool *pool, GstBuffer *buffer);
//gboolean        gst_pipewire_pool_remove_buffer (GstPipeWirePool *pool, GstBuffer *buffer);

G_END_DECLS

#endif /* __GST_PIPEWIRE_POOL_H__ */
