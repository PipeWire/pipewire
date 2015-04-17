/* GStreamer
 * Copyright (C) 2014 William Manley <will@williammanley.net>
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

#ifndef _GST_FDDEPAY_H_
#define _GST_FDDEPAY_H_

#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS
#define GST_TYPE_FDDEPAY   (gst_fddepay_get_type())
#define GST_FDDEPAY(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_FDDEPAY,GstFddepay))
#define GST_FDDEPAY_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_FDDEPAY,GstFddepayClass))
#define GST_IS_FDDEPAY(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_FDDEPAY))
#define GST_IS_FDDEPAY_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_FDDEPAY))
typedef struct _GstFddepay GstFddepay;
typedef struct _GstFddepayClass GstFddepayClass;

struct _GstFddepay
{
  GstBaseTransform base_fddepay;
  GstAllocator *fd_allocator;
};

struct _GstFddepayClass
{
  GstBaseTransformClass base_fddepay_class;
};

GType gst_fddepay_get_type (void);

G_END_DECLS
#endif
