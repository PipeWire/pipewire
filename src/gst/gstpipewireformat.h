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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef _GST_PIPEWIRE_FORMAT_H_
#define _GST_PIPEWIRE_FORMAT_H_

#include <gst/gst.h>

#include <spa/support/type-map.h>
#include <spa/pod/pod.h>

G_BEGIN_DECLS

struct spa_pod * gst_caps_to_format      (GstCaps *caps, guint index, struct spa_type_map *map);
GPtrArray *      gst_caps_to_format_all  (GstCaps *caps, struct spa_type_map *map);

GstCaps *        gst_caps_from_format    (const struct spa_pod *format, struct spa_type_map *map);

G_END_DECLS

#endif
