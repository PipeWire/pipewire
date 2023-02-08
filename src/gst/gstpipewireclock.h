/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

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
  GstClockTimeDiff time_offset;
};

struct _GstPipeWireClockClass {
  GstSystemClockClass parent_class;
};

GType gst_pipewire_clock_get_type (void);

GstClock *      gst_pipewire_clock_new           (struct pw_stream *stream,
                                                  GstClockTime last_time);
void            gst_pipewire_clock_reset         (GstPipeWireClock *clock,
                                                  GstClockTime time);

G_END_DECLS

#endif /* __GST_PIPEWIRE_CLOCK_H__ */
