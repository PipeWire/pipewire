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

#ifndef __GST_PINOS_CLOCK_H__
#define __GST_PINOS_CLOCK_H__

#include <gst/gst.h>

#include <client/pinos.h>

G_BEGIN_DECLS

#define GST_TYPE_PINOS_CLOCK \
  (gst_pinos_clock_get_type())
#define GST_PINOS_CLOCK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PINOS_CLOCK,GstPinosClock))
#define GST_PINOS_CLOCK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PINOS_CLOCK,GstPinosClockClass))
#define GST_IS_PINOS_CLOCK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PINOS_CLOCK))
#define GST_IS_PINOS_CLOCK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PINOS_CLOCK))
#define GST_PINOS_CLOCK_GET_CLASS(klass) \
  (G_TYPE_INSTANCE_GET_CLASS ((klass), GST_TYPE_PINOS_CLOCK, GstPinosClockClass))

typedef struct _GstPinosClock GstPinosClock;
typedef struct _GstPinosClockClass GstPinosClockClass;

struct _GstPinosClock {
  GstSystemClock parent;

  PinosStream *stream;
};

struct _GstPinosClockClass {
  GstSystemClockClass parent_class;
};

GType gst_pinos_clock_get_type (void);

GstClock *      gst_pinos_clock_new           (PinosStream *stream);


G_END_DECLS

#endif /* __GST_PINOS_CLOCK_H__ */
