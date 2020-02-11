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

#ifndef PIPEWIRE_CLIENT_NODE0_H
#define PIPEWIRE_CLIENT_NODE0_H

#include <pipewire/impl.h>

#include "ext-client-node.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The state of the clock */
enum spa_clock0_state {
	SPA_CLOCK0_STATE_STOPPED,	/*< the clock is stopped */
	SPA_CLOCK0_STATE_PAUSED,	/*< the clock is paused */
	SPA_CLOCK0_STATE_RUNNING,	/*< the clock is running */
};

struct spa_command_node0_clock_update_body {
	struct spa_pod_object_body body;
#define SPA_COMMAND_NODE0_CLOCK_UPDATE_TIME      (1 << 0)
#define SPA_COMMAND_NODE0_CLOCK_UPDATE_SCALE     (1 << 1)
#define SPA_COMMAND_NODE0_CLOCK_UPDATE_STATE     (1 << 2)
#define SPA_COMMAND_NODE0_CLOCK_UPDATE_LATENCY   (1 << 3)
	struct spa_pod_int change_mask          SPA_ALIGNED(8);
	struct spa_pod_int rate                 SPA_ALIGNED(8);
	struct spa_pod_long ticks               SPA_ALIGNED(8);
	struct spa_pod_long monotonic_time      SPA_ALIGNED(8);
	struct spa_pod_long offset              SPA_ALIGNED(8);
	struct spa_pod_int scale                SPA_ALIGNED(8);
	struct spa_pod_int state                SPA_ALIGNED(8);
#define SPA_COMMAND_NODE0_CLOCK_UPDATE_FLAG_LIVE (1 << 0)
	struct spa_pod_int flags                SPA_ALIGNED(8);
	struct spa_pod_long latency             SPA_ALIGNED(8);
};

struct spa_command_node0_clock_update {
	struct spa_pod pod;
	struct spa_command_node0_clock_update_body body;
};

#define SPA_COMMAND_NODE0_CLOCK_UPDATE_INIT(type,change_mask,rate,ticks,monotonic_time,offset,scale,state,flags,latency)  \
	SPA_COMMAND_INIT_FULL(struct spa_command_node0_clock_update,			\
                        sizeof(struct spa_command_node0_clock_update_body), 0, type,	\
			SPA_POD_INIT_Int(change_mask),					\
			SPA_POD_INIT_Int(rate),						\
			SPA_POD_INIT_Long(ticks),					\
			SPA_POD_INIT_Long(monotonic_time),				\
			SPA_POD_INIT_Long(offset),					\
			SPA_POD_INIT_Int(scale),					\
			SPA_POD_INIT_Int(state),					\
			SPA_POD_INIT_Int(flags),					\
			SPA_POD_INIT_Long(latency))


/** \class pw_impl_client_node0
 *
 * PipeWire client node interface
 */
struct pw_impl_client_node0 {
	struct pw_impl_node *node;

	struct pw_resource *resource;
};

struct pw_impl_client_node0 *
pw_impl_client_node0_new(struct pw_resource *resource,
		   struct pw_properties *properties);

void
pw_impl_client_node0_destroy(struct pw_impl_client_node0 *node);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_CLIENT_NODE0_H */
