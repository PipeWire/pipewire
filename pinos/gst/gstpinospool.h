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

#ifndef __GST_PINOS_POOL_H__
#define __GST_PINOS_POOL_H__

#include <gst/gst.h>

#include <client/pinos.h>

G_BEGIN_DECLS

#define GST_TYPE_PINOS_POOL \
  (gst_pinos_pool_get_type())
#define GST_PINOS_POOL(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PINOS_POOL,GstPinosPool))
#define GST_PINOS_POOL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PINOS_POOL,GstPinosPoolClass))
#define GST_IS_PINOS_POOL(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PINOS_POOL))
#define GST_IS_PINOS_POOL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PINOS_POOL))
#define GST_PINOS_POOL_GET_CLASS(klass) \
  (G_TYPE_INSTANCE_GET_CLASS ((klass), GST_TYPE_PINOS_POOL, GstPinosPoolClass))

typedef struct _GstPinosPool GstPinosPool;
typedef struct _GstPinosPoolClass GstPinosPoolClass;

struct _GstPinosPool {
  GstBufferPool parent;

  PinosStream *stream;
  GQueue available;
  GCond cond;
};

struct _GstPinosPoolClass {
  GstBufferPoolClass parent_class;
};

GType gst_pinos_pool_get_type (void);

GstPinosPool *  gst_pinos_pool_new           (void);

gboolean        gst_pinos_pool_add_buffer    (GstPinosPool *pool, GstBuffer *buffer);
gboolean        gst_pinos_pool_remove_buffer (GstPinosPool *pool, GstBuffer *buffer);

G_END_DECLS

#endif /* __GST_PINOS_POOL_H__ */
