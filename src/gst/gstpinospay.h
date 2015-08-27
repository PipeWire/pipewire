/* GStreamer
 * Copyright (C) 2014 William Manley <will@williammanley.net>
 *           (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef _GST_PINOS_PAY_H_
#define _GST_PINOS_PAY_H_

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_PINOS_PAY              (gst_pinos_pay_get_type())
#define GST_PINOS_PAY(obj)              (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PINOS_PAY,GstPinosPay))
#define GST_PINOS_PAY_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PINOS_PAY,GstPinosPayClass))
#define GST_IS_PINOS_PAY(obj)           (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PINOS_PAY))
#define GST_IS_PINOS_PAY_CLASS(obj)     (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PINOS_PAY))

typedef struct _GstPinosPay GstPinosPay;
typedef struct _GstPinosPayClass GstPinosPayClass;

struct _GstPinosPay
{
  GstElement    parent;

  gboolean negotiated;
  GstPad *srcpad, *sinkpad;

  GstAllocator *allocator;

};

struct _GstPinosPayClass
{
  GstElementClass parent_class;
};

GType gst_pinos_pay_get_type (void);

G_END_DECLS

#endif
