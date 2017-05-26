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

#ifndef __PIPEWIRE_LINK_H__
#define __PIPEWIRE_LINK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/ringbuffer.h>

#include <pipewire/client/mem.h>

#include <pipewire/server/core.h>
#include <pipewire/server/port.h>
#include <pipewire/server/main-loop.h>

/**
 * pw_link:
 *
 * PipeWire link interface.
 */
struct pw_link {
	struct pw_core *core;
	struct spa_list link;
	struct pw_global *global;

	struct pw_properties *properties;

	enum pw_link_state state;
	char *error;
	PW_SIGNAL(state_changed, (struct pw_listener *listener,
				  struct pw_link *link,
				  enum pw_link_state old, enum pw_link_state state));

	PW_SIGNAL(destroy_signal, (struct pw_listener *, struct pw_link *));
	PW_SIGNAL(free_signal, (struct pw_listener *, struct pw_link *));

	struct spa_list resource_list;

	struct pw_port *output;
	struct spa_list output_link;
	struct pw_port *input;
	struct spa_list input_link;
	PW_SIGNAL(port_unlinked, (struct pw_listener *listener,
				  struct pw_link *link, struct pw_port *port));

	struct {
		uint32_t in_ready;
		struct pw_port *input;
		struct pw_port *output;
		struct spa_list input_link;
		struct spa_list output_link;
	} rt;
};


struct pw_link *
pw_link_new(struct pw_core *core,
	    struct pw_port *output,
	    struct pw_port *input,
	    struct spa_format **format_filter,
	    struct pw_properties *properties);

void
pw_link_destroy(struct pw_link *link);

bool
pw_link_activate(struct pw_link *link);

bool
pw_link_deactivate(struct pw_link *link);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_LINK_H__ */
