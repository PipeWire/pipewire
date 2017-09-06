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

#ifndef __PIPEWIRE_MAIN_LOOP_H__
#define __PIPEWIRE_MAIN_LOOP_H__

#ifdef __cplusplus
extern "C" {
#endif

/** \class pw_main_loop
 *
 * \brief PipeWire main-loop interface.
 *
 * A main loop object
 */
/** A main loop object \memberof pw_main_loop */
struct pw_main_loop;

#include <pipewire/loop.h>
#include <pipewire/properties.h>

/** Events of the main loop */
struct pw_main_loop_events {
#define PW_VERSION_MAIN_LOOP_EVENTS	0
	uint32_t version;

	/** Emited when the main loop is destroyed */
	void (*destroy) (void *data);
};

/** Create a new main loop */
struct pw_main_loop *
pw_main_loop_new(struct pw_properties *properties);

/** Add an event listener */
void pw_main_loop_add_listener(struct pw_main_loop *loop,
			       struct spa_hook *listener,
			       const struct pw_main_loop_events *events,
			       void *data);

/** Get the loop implementation */
struct pw_loop * pw_main_loop_get_loop(struct pw_main_loop *loop);

/** Destroy a loop */
void pw_main_loop_destroy(struct pw_main_loop *loop);

/** Run a main loop. This blocks until \ref pw_main_loop_quit is called */
void pw_main_loop_run(struct pw_main_loop *loop);

/** Quit a main loop */
void pw_main_loop_quit(struct pw_main_loop *loop);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_MAIN_LOOP_H__ */
