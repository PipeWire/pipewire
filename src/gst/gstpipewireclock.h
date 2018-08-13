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

#ifndef __GST_PIPEWIRE_CLOCK_H__
#define __GST_PIPEWIRE_CLOCK_H__

#include <gst/gst.h>

#include <pipewire/pipewire.h>

G_BEGIN_DECLS

#define GST_TYPE_PIPEWIRE_CLOCK \
  (gst_pipewire_clock_get_type())
#define GST_PIPEWIRE_CLOCK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PIPEWIRE_CLOCK,GstPipeWireClock))
#define GST_PIPEWIRE_CLOCK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PIPEWIRE_CLOCK,GstPipeWireClockClass))
#define GST_IS_PIPEWIRE_CLOCK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PIPEWIRE_CLOCK))
#define GST_IS_PIPEWIRE_CLOCK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PIPEWIRE_CLOCK))
#define GST_PIPEWIRE_CLOCK_GET_CLASS(klass) \
  (G_TYPE_INSTANCE_GET_CLASS ((klass), GST_TYPE_PIPEWIRE_CLOCK, GstPipeWireClockClass))

typedef struct _GstPipeWireClock GstPipeWireClock;
typedef struct _GstPipeWireClockClass GstPipeWireClockClass;

struct _GstPipeWireClock {
  GstSystemClock parent;

  struct pw_stream *stream;
  GstClockTime last_time;
};

struct _GstPipeWireClockClass {
  GstSystemClockClass parent_class;
};

GType gst_pipewire_clock_get_type (void);

GstClock *      gst_pipewire_clock_new           (struct pw_stream *stream,
					          GstClockTime last_time);

G_END_DECLS

#endif /* __GST_PIPEWIRE_CLOCK_H__ */
