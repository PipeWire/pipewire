/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <gst/gst.h>

#include "gstpipewireclock.h"

GST_DEBUG_CATEGORY_STATIC (gst_pipewire_clock_debug_category);
#define GST_CAT_DEFAULT gst_pipewire_clock_debug_category

G_DEFINE_TYPE (GstPipeWireClock, gst_pipewire_clock, GST_TYPE_SYSTEM_CLOCK);

GstClock *
gst_pipewire_clock_new (struct pw_stream *stream, GstClockTime last_time)
{
  GstPipeWireClock *clock;

  clock = g_object_new (GST_TYPE_PIPEWIRE_CLOCK, NULL);
  clock->stream = stream;
  clock->last_time = last_time;
  clock->time_offset = last_time;

  return GST_CLOCK_CAST (clock);
}

static GstClockTime
gst_pipewire_clock_get_internal_time (GstClock * clock)
{
  GstPipeWireClock *pclock = (GstPipeWireClock *) clock;
  GstClockTime result;
  uint64_t now;

  now = pw_stream_get_nsec(pclock->stream);
#if 0
  struct pw_time t;
  if (pclock->stream == NULL ||
      pw_stream_get_time_n (pclock->stream, &t, sizeof(t)) < 0 ||
      t.rate.denom == 0)
    return pclock->last_time;

  result = gst_util_uint64_scale_int (t.ticks, GST_SECOND * t.rate.num, t.rate.denom);
  result += now - t.now;

  result += pclock->time_offset;
  pclock->last_time = result;

  GST_DEBUG ("%"PRId64", %d/%d %"PRId64" %"PRId64" %"PRId64,
                t.ticks, t.rate.num, t.rate.denom, t.now, result, now);
#else
  result = now + pclock->time_offset;
  pclock->last_time = result;
#endif

  return result;
}

static void
gst_pipewire_clock_finalize (GObject * object)
{
  GstPipeWireClock *clock = GST_PIPEWIRE_CLOCK (object);

  GST_DEBUG_OBJECT (clock, "finalize");

  G_OBJECT_CLASS (gst_pipewire_clock_parent_class)->finalize (object);
}

static void
gst_pipewire_clock_class_init (GstPipeWireClockClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstClockClass *gstclock_class = GST_CLOCK_CLASS (klass);

  gobject_class->finalize = gst_pipewire_clock_finalize;

  gstclock_class->get_internal_time = gst_pipewire_clock_get_internal_time;

  GST_DEBUG_CATEGORY_INIT (gst_pipewire_clock_debug_category, "pipewireclock", 0,
      "debug category for pipewireclock object");
}

static void
gst_pipewire_clock_init (GstPipeWireClock * clock)
{
  GST_OBJECT_FLAG_SET (clock, GST_CLOCK_FLAG_CAN_SET_MASTER);
}

void
gst_pipewire_clock_reset (GstPipeWireClock * clock, GstClockTime time)
{
#if 0
  GstClockTimeDiff time_offset;

  if (clock->last_time >= time)
    time_offset = clock->last_time - time;
  else
    time_offset = -(time - clock->last_time);

  clock->time_offset = time_offset;

  GST_DEBUG_OBJECT (clock,
      "reset clock to %" GST_TIME_FORMAT ", last %" GST_TIME_FORMAT
      ", offset %" GST_STIME_FORMAT, GST_TIME_ARGS (time),
      GST_TIME_ARGS (clock->last_time), GST_STIME_ARGS (time_offset));
#endif
}
