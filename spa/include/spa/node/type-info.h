/* Simple Plugin API
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

#ifndef __SPA_NODE_TYPES_H__
#define __SPA_NODE_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/type-info.h>

#include <spa/node/command.h>
#include <spa/node/event.h>
#include <spa/node/io.h>

/** Base for IO structures to interface with node ports */
#define SPA_TYPE__IO			SPA_TYPE_POINTER_BASE "IO"
#define SPA_TYPE_IO_BASE		SPA_TYPE__IO ":"

/** Base for control structures */
#define SPA_TYPE_IO__Control		SPA_TYPE_IO_BASE "Control"
#define SPA_TYPE_IO_CONTROL_BASE	SPA_TYPE_IO__Control ":"

/** An io area to exchange buffers with a port */
#define SPA_TYPE_IO__Buffers		SPA_TYPE_IO_BASE "Buffers"

/** IO area with clock information */
#define SPA_TYPE_IO__Clock		SPA_TYPE_IO_BASE "Clock"

/** IO area with latency information */
#define SPA_TYPE_IO__Latency		SPA_TYPE_IO_BASE "Latency"

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* _SPA_NODE_TYPES_H__ */
