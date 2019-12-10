/* PipeWire
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

#ifndef __PIPEWIRE_CLIENT_NODE0_TRANSPORT_H__
#define __PIPEWIRE_CLIENT_NODE0_TRANSPORT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>

#include <spa/utils/defs.h>

#include <pipewire/mem.h>

/** information about the transport region \memberof pw_client_node */
struct pw_client_node0_transport_info {
	int memfd;		/**< the memfd of the transport area */
	uint32_t offset;	/**< offset to map \a memfd at */
	uint32_t size;		/**< size of memfd mapping */
};

struct pw_client_node0_transport *
pw_client_node0_transport_new(struct pw_context *context, uint32_t max_input_ports, uint32_t max_output_ports);

struct pw_client_node0_transport *
pw_client_node0_transport_new_from_info(struct pw_client_node0_transport_info *info);

int
pw_client_node0_transport_get_info(struct pw_client_node0_transport *trans,
				  struct pw_client_node0_transport_info *info);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PIPEWIRE_CLIENT_NODE0_TRANSPORT_H__ */
