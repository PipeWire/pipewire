/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef _GST_PIPEWIRE_FORMAT_H_
#define _GST_PIPEWIRE_FORMAT_H_

#include <gst/gst.h>

#include <spa/pod/pod.h>

G_BEGIN_DECLS

GPtrArray *      gst_caps_to_format_all  (GstCaps *caps);

GstCaps *        gst_caps_from_format    (const struct spa_pod *format);

void             gst_caps_sanitize       (GstCaps **caps);

G_END_DECLS

#endif
