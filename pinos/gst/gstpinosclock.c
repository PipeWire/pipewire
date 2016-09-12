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

#include "gstpinosclock.h"

GST_DEBUG_CATEGORY_STATIC (gst_pinos_clock_debug_category);
#define GST_CAT_DEFAULT gst_pinos_clock_debug_category

G_DEFINE_TYPE (GstPinosClock, gst_pinos_clock, GST_TYPE_SYSTEM_CLOCK);

GstClock *
gst_pinos_clock_new (PinosStream *stream)
{
  GstPinosClock *clock;

  clock = g_object_new (GST_TYPE_PINOS_CLOCK, NULL);
  clock->stream = stream;

  return GST_CLOCK_CAST (clock);
}

static GstClockTime
gst_pinos_clock_get_internal_time (GstClock * clock)
{
  GstPinosClock *pclock = (GstPinosClock *) clock;
  GstClockTime result;
  PinosTime t;

  pinos_stream_get_time (pclock->stream, &t);

  result = gst_util_uint64_scale_int (t.ticks, GST_SECOND, t.rate);
  GST_DEBUG ("%"PRId64", %d %"PRId64, t.ticks, t.rate, result);

  return result;
}


static void
gst_pinos_clock_finalize (GObject * object)
{
  GstPinosClock *clock = GST_PINOS_CLOCK (object);

  GST_DEBUG_OBJECT (clock, "finalize");

  G_OBJECT_CLASS (gst_pinos_clock_parent_class)->finalize (object);
}

static void
gst_pinos_clock_class_init (GstPinosClockClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstClockClass *gstclock_class = GST_CLOCK_CLASS (klass);

  gobject_class->finalize = gst_pinos_clock_finalize;

  gstclock_class->get_internal_time = gst_pinos_clock_get_internal_time;

  GST_DEBUG_CATEGORY_INIT (gst_pinos_clock_debug_category, "pinosclock", 0,
      "debug category for pinosclock object");
}

static void
gst_pinos_clock_init (GstPinosClock * clock)
{
  GST_OBJECT_FLAG_SET (clock, GST_CLOCK_FLAG_CAN_SET_MASTER);
}
