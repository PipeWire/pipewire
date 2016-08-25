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
 * SECTION:element-pinossrc
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v pinossrc ! ximagesink
 * ]| Shows pinos output in an X window.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstpinossrc.h"
#include "gstpinossink.h"
#include "gstpinosdeviceprovider.h"

GST_DEBUG_CATEGORY (pinos_debug);

static gboolean
plugin_init (GstPlugin * plugin)
{
  pinos_init (NULL, NULL);

  gst_element_register (plugin, "pinossrc", GST_RANK_PRIMARY + 1,
      GST_TYPE_PINOS_SRC);
  gst_element_register (plugin, "pinossink", GST_RANK_NONE,
      GST_TYPE_PINOS_SINK);

  if (!gst_device_provider_register (plugin, "pinosdeviceprovider",
       GST_RANK_PRIMARY + 1, GST_TYPE_PINOS_DEVICE_PROVIDER))
    return FALSE;

  GST_DEBUG_CATEGORY_INIT (pinos_debug, "pinos", 0, "Pinos elements");

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    pinos,
    "Uses pinos to handle media streams",
    plugin_init, VERSION, "LGPL", "pinos", "pinos.org")
