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

#ifndef __PIPEWIRE_LOOP_H__
#define __PIPEWIRE_LOOP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/support/loop.h>

#include <pipewire/properties.h>

/** \class pw_loop
 *
 * PipeWire loop object provides an implementation of
 * the spa loop interfaces. It can be used to implement various
 * event loops.
 */
struct pw_loop {
	struct spa_loop *loop;			/**< wrapped loop */
	struct spa_loop_control *control;	/**< loop control */
	struct spa_loop_utils *utils;		/**< loop utils */
};

struct pw_loop *
pw_loop_new(struct pw_properties *properties);

void
pw_loop_destroy(struct pw_loop *loop);

#define pw_loop_add_source(l,...)	spa_loop_add_source((l)->loop,__VA_ARGS__)
#define pw_loop_update_source(l,...)	spa_loop_update_source(__VA_ARGS__)
#define pw_loop_remove_source(l,...)	spa_loop_remove_source(__VA_ARGS__)
#define pw_loop_invoke(l,...)		spa_loop_invoke((l)->loop,__VA_ARGS__)

#define pw_loop_get_fd(l)		spa_loop_control_get_fd((l)->control)
#define pw_loop_add_hook(l,...)		spa_loop_control_add_hook((l)->control,__VA_ARGS__)
#define pw_loop_enter(l)		spa_loop_control_enter((l)->control)
#define pw_loop_iterate(l,...)		spa_loop_control_iterate((l)->control,__VA_ARGS__)
#define pw_loop_leave(l)		spa_loop_control_leave((l)->control)

#define pw_loop_add_io(l,...)		spa_loop_utils_add_io((l)->utils,__VA_ARGS__)
#define pw_loop_update_io(l,...)	spa_loop_utils_update_io((l)->utils,__VA_ARGS__)
#define pw_loop_add_idle(l,...)		spa_loop_utils_add_idle((l)->utils,__VA_ARGS__)
#define pw_loop_enable_idle(l,...)	spa_loop_utils_enable_idle((l)->utils,__VA_ARGS__)
#define pw_loop_add_event(l,...)	spa_loop_utils_add_event((l)->utils,__VA_ARGS__)
#define pw_loop_signal_event(l,...)	spa_loop_utils_signal_event((l)->utils,__VA_ARGS__)
#define pw_loop_add_timer(l,...)	spa_loop_utils_add_timer((l)->utils,__VA_ARGS__)
#define pw_loop_update_timer(l,...)	spa_loop_utils_update_timer((l)->utils,__VA_ARGS__)
#define pw_loop_add_signal(l,...)	spa_loop_utils_add_signal((l)->utils,__VA_ARGS__)
#define pw_loop_destroy_source(l,...)	spa_loop_utils_destroy_source((l)->utils,__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_LOOP_H__ */
