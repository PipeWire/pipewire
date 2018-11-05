/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef __PIPEWIRE_TYPE_H__
#define __PIPEWIRE_TYPE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/node/event.h>
#include <spa/node/command.h>
#include <spa/monitor/monitor.h>
#include <spa/param/param.h>
#include <spa/node/io.h>

#include <pipewire/map.h>

enum {
	PW_TYPE_FIRST = SPA_TYPE_VENDOR_PipeWire,

	PW_TYPE_INTERFACE_Core,
	PW_TYPE_INTERFACE_Registry,
	PW_TYPE_INTERFACE_Node,
	PW_TYPE_INTERFACE_Port,
	PW_TYPE_INTERFACE_Factory,
	PW_TYPE_INTERFACE_Link,
	PW_TYPE_INTERFACE_Client,
	PW_TYPE_INTERFACE_Module,
	PW_TYPE_INTERFACE_ClientNode,

};

#define PW_TYPE_BASE		"PipeWire:"

#define PW_TYPE__Object		PW_TYPE_BASE "Object"
#define PW_TYPE_OBJECT_BASE	PW_TYPE__Object ":"

#define PW_TYPE__Interface	PW_TYPE_BASE "Interface"
#define PW_TYPE_INTERFACE_BASE	PW_TYPE__Interface ":"

const struct spa_type_info * pw_type_info(void);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_TYPE_H__ */
