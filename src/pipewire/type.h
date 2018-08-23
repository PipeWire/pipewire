/* PipeWire
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

#ifndef __PIPEWIRE_TYPE_H__
#define __PIPEWIRE_TYPE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/node/event.h>
#include <spa/node/command.h>
#include <spa/monitor/monitor.h>
#include <spa/param/buffers.h>
#include <spa/param/meta.h>
#include <spa/param/io.h>
#include <spa/node/io.h>

#include <pipewire/map.h>

enum {
	PW_ID_FIRST = SPA_ID_VENDOR_PipeWire,

	PW_ID_INTERFACE_Core,
	PW_ID_INTERFACE_Registry,
	PW_ID_INTERFACE_Node,
	PW_ID_INTERFACE_Port,
	PW_ID_INTERFACE_Factory,
	PW_ID_INTERFACE_Link,
	PW_ID_INTERFACE_Client,
	PW_ID_INTERFACE_Module,
	PW_ID_INTERFACE_ClientNode,

	PW_ID_IO_BASE = PW_ID_FIRST + SPA_ID_IO_BASE,
	PW_ID_IO_ClientNodePosition,
};

#define PW_TYPE_BASE		"PipeWire:"

#define PW_TYPE__Object		PW_TYPE_BASE "Object"
#define PW_TYPE_OBJECT_BASE	PW_TYPE__Object ":"

#define PW_TYPE__Interface	PW_TYPE_BASE "Interface"
#define PW_TYPE_INTERFACE_BASE	PW_TYPE__Interface ":"

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_TYPE_H__ */
