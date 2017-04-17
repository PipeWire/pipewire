/* Pinos
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

#include <pthread.h>

#include "pinos.h"
#include "thread-mainloop.h"

typedef struct {
  PinosThreadMainLoop this;

  char *name;

  pthread_mutex_t lock;
  pthread_cond_t  cond;
  pthread_cond_t  accept_cond;

  bool running;
  pthread_t       thread;

  SpaSource *event;

  int n_waiting;
  int n_waiting_for_accept;
} PinosThreadMainLoopImpl;

static void
pre_hook (SpaLoopControl *ctrl,
          void           *data)
{
  PinosThreadMainLoopImpl *impl = data;
  pthread_mutex_unlock (&impl->lock);
}

static void
post_hook (SpaLoopControl *ctrl,
           void           *data)
{
  PinosThreadMainLoopImpl *impl = data;
  pthread_mutex_lock (&impl->lock);
}

static void
do_stop (SpaLoopUtils *utils,
         SpaSource    *source,
         void         *data)
{
  PinosThreadMainLoopImpl *impl = data;
  impl->running = false;
}

/**
 * pinos_thread_main_loop_new:
 * @context: a #GMainContext
 * @name: a thread name
 *
 * Make a new #PinosThreadMainLoop that will run a mainloop on @context in
 * a thread with @name.
 *
 * Returns: a #PinosThreadMainLoop
 */
PinosThreadMainLoop *
pinos_thread_main_loop_new (PinosLoop  *loop,
                            const char *name)
{
  PinosThreadMainLoopImpl *impl;
  PinosThreadMainLoop *this;
  pthread_mutexattr_t attr;

  impl = calloc (1, sizeof (PinosThreadMainLoopImpl));
  if (impl == NULL)
    return NULL;

  this = &impl->this;
  pinos_log_debug ("thread-mainloop %p: new", impl);

  this->loop = loop;
  this->name = name ? strdup (name) : NULL;

  pinos_loop_set_hooks (loop,
                        pre_hook,
                        post_hook,
                        impl);

  pinos_signal_init (&this->destroy_signal);

  pthread_mutexattr_init (&attr);
  pthread_mutexattr_settype (&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init (&impl->lock, &attr);
  pthread_cond_init (&impl->cond, NULL);
  pthread_cond_init (&impl->accept_cond, NULL);

  impl->event = pinos_loop_add_event (this->loop,
                                      do_stop,
                                      impl);

  return this;
}

void
pinos_thread_main_loop_destroy (PinosThreadMainLoop *loop)
{
  PinosThreadMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosThreadMainLoopImpl, this);

  pinos_signal_emit (&loop->destroy_signal, loop);

  pinos_thread_main_loop_stop (loop);

  if (loop->name)
    free (loop->name);
  pthread_mutex_destroy (&impl->lock);
  pthread_cond_destroy (&impl->cond);
  pthread_cond_destroy (&impl->accept_cond);

  free (impl);
}

static void *
do_loop (void *user_data)
{
  PinosThreadMainLoopImpl *impl = user_data;
  PinosThreadMainLoop *this = &impl->this;
  SpaResult res;

  pthread_mutex_lock (&impl->lock);
  pinos_log_debug ("thread-mainloop %p: enter thread", this);
  pinos_loop_enter (this->loop);

  while (impl->running) {
    if ((res = pinos_loop_iterate (this->loop, -1)) < 0)
      pinos_log_warn ("thread-mainloop %p: iterate error %d", this, res);
  }
  pinos_log_debug ("thread-mainloop %p: leave thread", this);
  pinos_loop_leave (this->loop);
  pthread_mutex_unlock (&impl->lock);

  return NULL;
}

/**
 * pinos_thread_main_loop_start:
 * @loop: a #PinosThreadMainLoop
 *
 * Start the thread to handle @loop.
 *
 * Returns: %SPA_RESULT_OK on success.
 */
SpaResult
pinos_thread_main_loop_start (PinosThreadMainLoop *loop)
{
  PinosThreadMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosThreadMainLoopImpl, this);

  if (!impl->running) {
    int err;

    impl->running = true;
    if ((err = pthread_create (&impl->thread, NULL, do_loop, impl)) != 0) {
      pinos_log_warn ("thread-mainloop %p: can't create thread: %s", impl, strerror (err));
      impl->running = false;
      return SPA_RESULT_ERROR;
    }
  }
  return SPA_RESULT_OK;
}

/**
 * pinos_thread_main_loop_stop:
 * @loop: a #PinosThreadMainLoop
 *
 * Quit the main loop and stop its thread.
 */
void
pinos_thread_main_loop_stop (PinosThreadMainLoop *loop)
{
  PinosThreadMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosThreadMainLoopImpl, this);

  pinos_log_debug ("thread-mainloop: %p stopping", impl);
  if (impl->running) {
    pinos_log_debug ("thread-mainloop: %p signal", impl);
    pinos_loop_signal_event (loop->loop, impl->event);
    pinos_log_debug ("thread-mainloop: %p join", impl);
    pthread_join (impl->thread, NULL);
    pinos_log_debug ("thread-mainloop: %p joined", impl);
    impl->running = false;
  }
  pinos_log_debug ("thread-mainloop: %p stopped", impl);
}

/**
 * pinos_thread_main_loop_lock:
 * @loop: a #PinosThreadMainLoop
 *
 * Lock the mutex associated with @loop.
 */
void
pinos_thread_main_loop_lock (PinosThreadMainLoop *loop)
{
  PinosThreadMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosThreadMainLoopImpl, this);
  pthread_mutex_lock (&impl->lock);
}

/**
 * pinos_thread_main_loop_unlock:
 * @loop: a #PinosThreadMainLoop
 *
 * Unlock the mutex associated with @loop.
 */
void
pinos_thread_main_loop_unlock (PinosThreadMainLoop *loop)
{
  PinosThreadMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosThreadMainLoopImpl, this);
  pthread_mutex_unlock (&impl->lock);
}

/**
 * pinos_thread_main_loop_signal:
 * @loop: a #PinosThreadMainLoop
 *
 * Signal the main thread of @loop. If @wait_for_accept is %TRUE,
 * this function waits until pinos_thread_main_loop_accept() is called.
 */
void
pinos_thread_main_loop_signal (PinosThreadMainLoop *loop,
                               bool                 wait_for_accept)
{
  PinosThreadMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosThreadMainLoopImpl, this);

  if (impl->n_waiting > 0)
    pthread_cond_broadcast (&impl->cond);

  if (wait_for_accept) {
     impl->n_waiting_for_accept++;

     while (impl->n_waiting_for_accept > 0)
       pthread_cond_wait (&impl->accept_cond, &impl->lock);
  }
}

/**
 * pinos_thread_main_loop_wait:
 * @loop: a #PinosThreadMainLoop
 *
 * Wait for the loop thread to call pinos_thread_main_loop_signal().
 */
void
pinos_thread_main_loop_wait (PinosThreadMainLoop *loop)
{
  PinosThreadMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosThreadMainLoopImpl, this);

  impl->n_waiting++;

  pthread_cond_wait (&impl->cond, &impl->lock);
  impl->n_waiting --;
}

/**
 * pinos_thread_main_loop_accept:
 * @loop: a #PinosThreadMainLoop
 *
 * Signal the loop thread waiting for accept with pinos_thread_main_loop_signal().
 */
void
pinos_thread_main_loop_accept (PinosThreadMainLoop *loop)
{
  PinosThreadMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosThreadMainLoopImpl, this);

  impl->n_waiting_for_accept--;
  pthread_cond_signal (&impl->accept_cond);
}

/**
 * pinos_thread_main_loop_in_thread:
 * @loop: a #PinosThreadMainLoop
 *
 * Check if we are inside the thread of @loop.
 *
 * Returns: %TRUE when called inside the thread of @loop.
 */
bool
pinos_thread_main_loop_in_thread (PinosThreadMainLoop *loop)
{
  PinosThreadMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosThreadMainLoopImpl, this);
  return pthread_self() == impl->thread;
}
