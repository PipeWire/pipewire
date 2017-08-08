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

#include <spa/hook.h>

/** \class pw_data_loop
 *
 * PipeWire rt-loop object.
 */
struct pw_data_loop;

#include <pipewire/loop.h>
#include <pipewire/properties.h>

struct pw_data_loop_events {
#define PW_VERSION_DATA_LOOP_EVENTS		0
	uint32_t version;

	void (*destroy) (void *data);
};

struct pw_data_loop *
pw_data_loop_new(struct pw_properties *properties);

void pw_data_loop_add_listener(struct pw_data_loop *loop,
			       struct spa_hook *listener,
			       const struct pw_data_loop_events *events,
			       void *data);

struct pw_loop *
pw_data_loop_get_loop(struct pw_data_loop *loop);

void
pw_data_loop_destroy(struct pw_data_loop *loop);

int
pw_data_loop_start(struct pw_data_loop *loop);

int
pw_data_loop_stop(struct pw_data_loop *loop);

bool
pw_data_loop_in_thread(struct pw_data_loop *loop);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_DATA_LOOP_H__ */
