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

#ifndef __PIPEWIRE_THREAD_LOOP_H__
#define __PIPEWIRE_THREAD_LOOP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <pipewire/loop.h>

/** \page page_thread_loop Threaded Loop
 *
 * \section sec_thread_loop_overview Overview
 *
 * The threaded loop implementation is a special wrapper around the
 * regular \ref pw_loop implementation.
 *
 * The added feature in the threaded loop is that it spawns a new thread
 * that runs the wrapped loop. This allows a synchronous application to use
 * the asynchronous API without risking to stall the PipeWire library.
 *
 * \section sec_thread_loop_create Creation
 *
 * A \ref pw_thread_loop object is created using pw_thread_loop_new().
 * The \ref pw_loop to wrap must be given as an argument along with the name
 * for the thread that will be spawned.
 *
 * After allocating the object, the thread must be started with
 * pw_thread_loop_start()
 *
 * \section sec_thread_loop_destruction Destruction
 *
 * When the PipeWire connection has been terminated, the thread must be
 * stopped and the resources freed. Stopping the thread is done using
 * pw_thread_loop_stop(), which must be called without the lock (see
 * below) held. When that function returns, the thread is stopped and the
 * \ref pw_thread_loop object can be freed using pw_thread_loop_destroy().
 *
 * \section sec_thread_loop_locking Locking
 *
 * Since the PipeWire API doesn't allow concurrent accesses to objects,
 * a locking scheme must be used to guarantee safe usage. The threaded
 * loop API provides such a scheme through the functions
 * pw_thread_loop_lock() and pw_thread_loop_unlock().
 *
 * The lock is recursive, so it's safe to use it multiple times from the same
 * thread. Just make sure you call pw_thread_loop_unlock() the same
 * number of times you called pw_thread_loop_lock().
 *
 * The lock needs to be held whenever you call any PipeWire function that
 * uses an object associated with this loop. Make sure you do not hold
 * on to the lock more than necessary though, as the threaded loop stops
 * while the lock is held.
 *
 * \section sec_thread_loop_events Events and Callbacks
 *
 * All events and callbacks are called with the thread lock held.
 *
 */
/** \class pw_thread_loop
 *
 * \brief PipeWire threaded loop object
 *
 * The threaded loop object runs a \ref pw_loop in a separate thread
 * and ensures proper locking is done.
 *
 * All of the loop callbacks will be executed with the loop
 * lock held.
 *
 * See also \ref page_thread_loop
 */
struct pw_thread_loop;

/** Thread loop events */
struct pw_thread_loop_events {
#define PW_VERSION_THREAD_LOOP_EVENTS	0
        uint32_t version;

	/** the loop is destroyed */
        void (*destroy) (void *data);
};

/** Make a new thread loop with the given name */
struct pw_thread_loop *
pw_thread_loop_new(struct pw_loop *loop, const char *name);

/** Destroy a thread loop */
void pw_thread_loop_destroy(struct pw_thread_loop *loop);

/** Add an event listener */
void pw_thread_loop_add_listener(struct pw_thread_loop *loop,
				 struct spa_hook *listener,
				 const struct pw_thread_loop_events *events,
				 void *data);

/** Get the loop implementation of the thread loop */
struct pw_loop * pw_thread_loop_get_loop(struct pw_thread_loop *loop);

/** Start the thread loop */
int pw_thread_loop_start(struct pw_thread_loop *loop);

/** Stop the thread loop */
void pw_thread_loop_stop(struct pw_thread_loop *loop);

/** Lock the loop. This ensures exclusive ownership of the loop */
void pw_thread_loop_lock(struct pw_thread_loop *loop);

/** Unlock the loop */
void pw_thread_loop_unlock(struct pw_thread_loop *loop);

/** Release the lock and wait until some thread calls \ref pw_thread_loop_signal */
void pw_thread_loop_wait(struct pw_thread_loop *loop);

/** Release the lock and wait a maximum of 'wait_max_sec' seconds
 *  until some thread calls \ref pw_thread_loop_signal or time out */
int pw_thread_loop_timed_wait(struct pw_thread_loop *loop, int wait_max_sec);

/** Signal all threads waiting with \ref pw_thread_loop_wait */
void pw_thread_loop_signal(struct pw_thread_loop *loop, bool wait_for_accept);

/** Signal all threads executing \ref pw_thread_loop_signal with wait_for_accept */
void pw_thread_loop_accept(struct pw_thread_loop *loop);

/** Check if inside the thread */
bool pw_thread_loop_in_thread(struct pw_thread_loop *loop);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_THREAD_LOOP_H__ */
