/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2016 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

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
