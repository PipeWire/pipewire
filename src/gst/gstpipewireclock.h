/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef __GST_PIPEWIRE_CLOCK_H__
#define __GST_PIPEWIRE_CLOCK_H__

#include "config.h"
#include "gstpipewirestream.h"

#include <gst/gst.h>
#include <pipewire/pipewire.h>

G_BEGIN_DECLS

#define GST_TYPE_PIPEWIRE_CLOCK (gst_pipewire_clock_get_type())
G_DECLARE_FINAL_TYPE (GstPipeWireClock, gst_pipewire_clock, GST, PIPEWIRE_CLOCK, GstSystemClock)

struct _GstPipeWireClock {
  GstSystemClock parent;

  GWeakRef stream;

  GstClockTime last_time;
  GstClockTimeDiff time_offset;
};

GstClock *      gst_pipewire_clock_new           (GstPipeWireStream *stream,
                                                  GstClockTime last_time);
void            gst_pipewire_clock_reset         (GstPipeWireClock *clock,
                                                  GstClockTime time);

G_END_DECLS

#endif /* __GST_PIPEWIRE_CLOCK_H__ */
