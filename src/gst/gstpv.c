/* GStreamer
 * Copyright (C) <2015> Wim Taymans <wim.taymans@gmail.com>
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

/**
 * SECTION:element-pulsevideosrc
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v pulsevideosrc ! ximagesink
 * ]| Shows pulsevideo output in an X window.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstpvsrc.h"
#include "gstpvsink.h"
#include "gstfdpay.h"
#include "gstfddepay.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  gst_element_register (plugin, "pvfdpay", GST_RANK_NONE,
      GST_TYPE_FDPAY);
  gst_element_register (plugin, "pvfddepay", GST_RANK_NONE,
      GST_TYPE_FDDEPAY);
  gst_element_register (plugin, "pulsevideosrc", GST_RANK_NONE,
      GST_TYPE_PULSEVIDEO_SRC);
  gst_element_register (plugin, "pulsevideosink", GST_RANK_NONE,
      GST_TYPE_PULSEVIDEO_SINK);
  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    pulsevideo,
    "Uses pulsevideo to handle video streams",
    plugin_init, VERSION, "LGPL", "pulsevideo", "pulsevideo.org")
