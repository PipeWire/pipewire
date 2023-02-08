/* PipeWire GStreamer Elements */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

/**
 * SECTION:element-pipewiresrc
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v pipewiresrc ! ximagesink
 * ]| Shows PipeWire output in an X window.
 * </refsect2>
 */

#include "config.h"

#include "gstpipewiresrc.h"
#include "gstpipewiresink.h"
#include "gstpipewiredeviceprovider.h"

GST_DEBUG_CATEGORY (pipewire_debug);

static gboolean
plugin_init (GstPlugin *plugin)
{
  pw_init (NULL, NULL);

  gst_element_register (plugin, "pipewiresrc", GST_RANK_PRIMARY + 1,
      GST_TYPE_PIPEWIRE_SRC);
  gst_element_register (plugin, "pipewiresink", GST_RANK_NONE,
      GST_TYPE_PIPEWIRE_SINK);

#ifdef HAVE_GSTREAMER_DEVICE_PROVIDER
  if (!gst_device_provider_register (plugin, "pipewiredeviceprovider",
       GST_RANK_PRIMARY + 1, GST_TYPE_PIPEWIRE_DEVICE_PROVIDER))
    return FALSE;
#endif

  GST_DEBUG_CATEGORY_INIT (pipewire_debug, "pipewire", 0, "PipeWire elements");

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    pipewire,
    "Uses PipeWire to handle media streams",
    plugin_init, PACKAGE_VERSION, "MIT/X11", "pipewire", "pipewire.org")
