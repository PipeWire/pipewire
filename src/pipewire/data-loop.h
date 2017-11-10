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

#ifndef __PIPEWIRE_DATA_LOOP_H__
#define __PIPEWIRE_DATA_LOOP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/hook.h>

/** \class pw_data_loop
 *
 * PipeWire rt-loop object. This loop starts a new real-time thread that
 * is designed to run the processing graph.
 */
struct pw_data_loop;

#include <pipewire/loop.h>
#include <pipewire/properties.h>

/** Loop events, use \ref pw_data_loop_add_listener to add a listener */
struct pw_data_loop_events {
#define PW_VERSION_DATA_LOOP_EVENTS		0
	uint32_t version;
	/** The loop is destroyed */
	void (*destroy) (void *data);
};

/** Make a new loop */
struct pw_data_loop *
pw_data_loop_new(struct pw_properties *properties);

/** Add an event listener to loop */
void pw_data_loop_add_listener(struct pw_data_loop *loop,
			       struct spa_hook *listener,
			       const struct pw_data_loop_events *events,
			       void *data);

/** Get the loop implementation of this data loop */
struct pw_loop *
pw_data_loop_get_loop(struct pw_data_loop *loop);

/** Destroy the loop */
void pw_data_loop_destroy(struct pw_data_loop *loop);

/** Start the processing thread */
int pw_data_loop_start(struct pw_data_loop *loop);

/** Stop the processing thread */
int pw_data_loop_stop(struct pw_data_loop *loop);

/** Check if the current thread is the processing thread */
bool pw_data_loop_in_thread(struct pw_data_loop *loop);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_DATA_LOOP_H__ */
