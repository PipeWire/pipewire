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

#ifndef __PIPEWIRE_CONTROL_H__
#define __PIPEWIRE_CONTROL_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PW_TYPE__Control			"PipeWire:Object:Control"
#define PW_TYPE_CONTROL_BASE			PW_TYPE__Control ":"

#include <spa/utils/hook.h>

/** \page page_control Control
 *
 * \section page_control_overview Overview
 *
 * A control can be used to control a port property.
 */
/** \class pw_control
 *
 * The control object
 */
struct pw_control;

#include <pipewire/core.h>
#include <pipewire/introspect.h>
#include <pipewire/node.h>

/** Port events, use \ref pw_control_add_listener */
struct pw_control_events {
#define PW_VERSION_PORT_EVENTS 0
	uint32_t version;

	/** The control is destroyed */
	void (*destroy) (void *data);

	/** The control is freed */
	void (*free) (void *data);

	/** control is linked to another control */
	void (*linked) (void *data, struct pw_control *other);
	/** control is unlinked from another control */
	void (*unlinked) (void *data, struct pw_control *other);

};

/** Get the control parent port or NULL when not set */
struct pw_port *pw_control_get_port(struct pw_control *control);

/** Add an event listener on the control */
void pw_control_add_listener(struct pw_control *control,
			     struct spa_hook *listener,
			     const struct pw_control_events *events,
			     void *data);

int pw_control_link(struct pw_control *control, struct pw_control *other);
int pw_control_unlink(struct pw_control *control, struct pw_control *other);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_CONTROL_H__ */
