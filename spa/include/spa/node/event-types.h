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

#ifndef __SPA_EVENT_NODE_TYPES_H__
#define __SPA_EVENT_NODE_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/type-info.h>
#include <spa/node/event.h>

#define SPA_TYPE_EVENT__Node		SPA_TYPE_EVENT_BASE "Node"
#define SPA_TYPE_EVENT_NODE_BASE	SPA_TYPE_EVENT__Node ":"

#define SPA_TYPE_EVENT_NODE__Error		SPA_TYPE_EVENT_NODE_BASE "Error"
#define SPA_TYPE_EVENT_NODE__Buffering		SPA_TYPE_EVENT_NODE_BASE "Buffering"
#define SPA_TYPE_EVENT_NODE__RequestRefresh	SPA_TYPE_EVENT_NODE_BASE "RequestRefresh"

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_EVENT_NODE_TYPES_H__ */
