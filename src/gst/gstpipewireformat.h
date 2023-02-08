/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef _GST_PIPEWIRE_FORMAT_H_
#define _GST_PIPEWIRE_FORMAT_H_

#include <gst/gst.h>

#include <spa/pod/pod.h>

G_BEGIN_DECLS

struct spa_pod * gst_caps_to_format      (GstCaps *caps,
                                          guint index, uint32_t id);
GPtrArray *      gst_caps_to_format_all  (GstCaps *caps, uint32_t id);

GstCaps *        gst_caps_from_format    (const struct spa_pod *format);

G_END_DECLS

#endif
