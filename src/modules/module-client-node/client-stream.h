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

#ifndef __PIPEWIRE_CLIENT_STREAM_H__
#define __PIPEWIRE_CLIENT_STREAM_H__

#include <pipewire/node.h>
#include <extensions/client-stream.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \class pw_client_stream
 *
 * PipeWire client stream interface
 */
struct pw_client_stream {
	struct pw_node *node;
};

struct pw_client_stream *
pw_client_stream_new(struct pw_resource *resource,
		   struct pw_properties *properties);

void
pw_client_stream_destroy(struct pw_client_stream *stream);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_CLIENT_STREAM_H__ */
