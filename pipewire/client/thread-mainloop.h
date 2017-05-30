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

#ifndef __PIPEWIRE_THREAD_MAIN_LOOP_H__
#define __PIPEWIRE_THREAD_MAIN_LOOP_H__

#include <pipewire/client/loop.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \class pw_thread_main_loop
 *
 * \brief PipeWire threaded main loop object
 *
 * The threaded main loop object runs a \ref pw_loop in a separate thread
 * and ensures proper locking is done.
 *
 * All of the loop callbacks will be executed with the main loop
 * lock held.
 */
struct pw_thread_main_loop {
	struct pw_loop *loop;	/**< the \ref pw_loop that is wrapped */
	char *name;		/**< the thread name */

	/** Emited when the mainloop is destroyed */
	PW_SIGNAL(destroy_signal, (struct pw_listener *listener,
				   struct pw_thread_main_loop *loop));
};

struct pw_thread_main_loop *
pw_thread_main_loop_new(struct pw_loop *loop, const char *name);

void
pw_thread_main_loop_destroy(struct pw_thread_main_loop *loop);

int
pw_thread_main_loop_start(struct pw_thread_main_loop *loop);

void
pw_thread_main_loop_stop(struct pw_thread_main_loop *loop);

void
pw_thread_main_loop_lock(struct pw_thread_main_loop *loop);

void
pw_thread_main_loop_unlock(struct pw_thread_main_loop *loop);

void
pw_thread_main_loop_wait(struct pw_thread_main_loop *loop);

void
pw_thread_main_loop_signal(struct pw_thread_main_loop *loop, bool wait_for_accept);

void
pw_thread_main_loop_accept(struct pw_thread_main_loop *loop);

bool
pw_thread_main_loop_in_thread(struct pw_thread_main_loop *loop);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_THREAD_MAIN_LOOP_H__ */
