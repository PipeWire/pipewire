/* Pinos
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>

#include "pinos/client/log.h"
#include "pinos/client/rtkit.h"
#include "pinos/server/data-loop.h"

typedef struct
{
  PinosDataLoop this;

  PinosSource *event;

  bool running;
  pthread_t thread;
} PinosDataLoopImpl;

static void
make_realtime (PinosDataLoop *this)
{
  struct sched_param sp;
  PinosRTKitBus *system_bus;
  struct rlimit rl;
  int r, rtprio;
  long long rttime;

  rtprio = 20;
  rttime = 20000;

  spa_zero (sp);
  sp.sched_priority = rtprio;

  if (pthread_setschedparam (pthread_self(), SCHED_RR|SCHED_RESET_ON_FORK, &sp) == 0) {
    pinos_log_debug ("SCHED_OTHER|SCHED_RESET_ON_FORK worked.");
    return;
  }
  system_bus = pinos_rtkit_bus_get_system ();

  rl.rlim_cur = rl.rlim_max = rttime;
  if ((r = setrlimit (RLIMIT_RTTIME, &rl)) < 0)
    pinos_log_debug ("setrlimit() failed: %s", strerror (errno));

  if (rttime >= 0) {
    r = getrlimit (RLIMIT_RTTIME, &rl);
    if (r >= 0 && (long long) rl.rlim_max > rttime) {
      pinos_log_debug ("Clamping rlimit-rttime to %lld for RealtimeKit", rttime);
      rl.rlim_cur = rl.rlim_max = rttime;

      if ((r = setrlimit (RLIMIT_RTTIME, &rl)) < 0)
        pinos_log_debug ("setrlimit() failed: %s", strerror (errno));
    }
  }

  if ((r = pinos_rtkit_make_realtime (system_bus, 0, rtprio)) < 0) {
    pinos_log_debug ("could not make thread realtime: %s", strerror (r));
  } else {
    pinos_log_debug ("thread made realtime");
  }
  pinos_rtkit_bus_free (system_bus);
}

static void *
do_loop (void *user_data)
{
  PinosDataLoopImpl *impl = user_data;
  PinosDataLoop *this = &impl->this;
  SpaResult res;

  make_realtime (this);

  pinos_log_debug ("data-loop %p: enter thread", this);
  pinos_loop_enter_thread (impl->this.loop);

  while (impl->running) {
    if ((res = pinos_loop_iterate (this->loop, -1)) < 0)
      pinos_log_warn ("data-loop %p: iterate error %d", this, res);
  }
  pinos_log_debug ("data-loop %p: leave thread", this);
  pinos_loop_leave_thread (impl->this.loop);

  return NULL;
}


static void
do_stop (PinosSource *source,
         void        *data)
{
  PinosDataLoopImpl *impl = data;
  impl->running = false;
}

/**
 * pinos_data_loop_new:
 *
 * Create a new #PinosDataLoop.
 *
 * Returns: a new #PinosDataLoop
 */
PinosDataLoop *
pinos_data_loop_new (void)
{
  PinosDataLoopImpl *impl;
  PinosDataLoop *this;

  impl = calloc (1, sizeof (PinosDataLoopImpl));
  pinos_log_debug ("data-loop %p: new", impl);

  this = &impl->this;
  this->loop = pinos_loop_new ();
  pinos_signal_init (&this->destroy_signal);

  impl->event = pinos_loop_add_event (this->loop,
                                      do_stop,
                                      impl);
  return this;
}

void
pinos_data_loop_destroy (PinosDataLoop *loop)
{
  PinosDataLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosDataLoopImpl, this);

  pinos_log_debug ("data-loop %p: destroy", impl);
  pinos_signal_emit (&loop->destroy_signal, loop);

  pinos_data_loop_stop (loop);

  pinos_source_destroy (impl->event);
  pinos_loop_destroy (loop->loop);
  free (impl);
}

SpaResult
pinos_data_loop_start (PinosDataLoop *loop)
{
  PinosDataLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosDataLoopImpl, this);

  if (!impl->running) {
    int err;

    impl->running = true;
    if ((err = pthread_create (&impl->thread, NULL, do_loop, impl)) != 0) {
      pinos_log_warn ("data-loop %p: can't create thread: %s", impl, strerror (err));
      impl->running = false;
      return SPA_RESULT_ERROR;
    }
  }
  return SPA_RESULT_OK;
}

SpaResult
pinos_data_loop_stop (PinosDataLoop *loop)
{
  PinosDataLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosDataLoopImpl, this);

  pinos_source_event_signal (impl->event);

  pthread_join (impl->thread, NULL);

  return SPA_RESULT_OK;
}

bool
pinos_data_loop_in_thread (PinosDataLoop *loop)
{
  PinosDataLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosDataLoopImpl, this);
  return pthread_equal (impl->thread, pthread_self());
}
