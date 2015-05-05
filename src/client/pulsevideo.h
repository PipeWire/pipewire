/* Pulsevideo
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PULSEVIDEO_H__
#define __PULSEVIDEO_H__

#include <client/pv-stream.h>
#include <client/pv-context.h>
#include <client/pv-subscribe.h>
#include <client/pv-introspect.h>

#define PV_DBUS_SERVICE "org.pulsevideo"
#define PV_DBUS_OBJECT_PREFIX "/org/pulsevideo"
#define PV_DBUS_OBJECT_SERVER PV_DBUS_OBJECT_PREFIX "/server"
#define PV_DBUS_OBJECT_SOURCE PV_DBUS_OBJECT_PREFIX "/source"
#define PV_DBUS_OBJECT_CLIENT PV_DBUS_OBJECT_PREFIX "/client"

void pv_init (int *argc, char **argv[]);

#endif /* __PULSEVIDEO_H__ */

