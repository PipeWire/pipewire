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

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/eventfd.h>

#include "spa/include/spa/list.h"
#include "spa/include/spa/ringbuffer.h"
#include "pinos/client/log.h"
#include "pinos/server/main-loop.h"

typedef struct
{
  PinosMainLoop this;

  bool running;
} PinosMainLoopImpl;

/**
 * pinos_main_loop_new:
 *
 * Create a new #PinosMainLoop.
 *
 * Returns: a new #PinosMainLoop
 */
PinosMainLoop *
pinos_main_loop_new (void)
{
  PinosMainLoopImpl *impl;
  PinosMainLoop *this;

  impl = calloc (1, sizeof (PinosMainLoopImpl));
  if (impl == NULL)
    return NULL;

  pinos_log_debug ("main-loop %p: new", impl);
  this = &impl->this;

  this->loop = pinos_loop_new ();
  if (this->loop == NULL)
    goto no_loop;

  pinos_signal_init (&this->destroy_signal);

  return this;

no_loop:
  free (impl);
  return NULL;
}

void
pinos_main_loop_destroy (PinosMainLoop *loop)
{
  PinosMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosMainLoopImpl, this);

  pinos_log_debug ("main-loop %p: destroy", impl);
  pinos_signal_emit (&loop->destroy_signal, loop);

  pinos_loop_destroy (loop->loop);

  free (impl);
}

/**
 * pinos_main_loop_quit:
 * @loop: a #PinosMainLoop
 *
 * Stop the running @loop.
 */
void
pinos_main_loop_quit (PinosMainLoop *loop)
{
  PinosMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosMainLoopImpl, this);
  pinos_log_debug ("main-loop %p: quit", impl);
  impl->running = false;
}

/**
 * pinos_main_loop_run:
 * @loop: a #PinosMainLoop
 *
 * Start running @loop. This function blocks until pinos_main_loop_quit()
 * has been called.
 */
void
pinos_main_loop_run (PinosMainLoop *loop)
{
  PinosMainLoopImpl *impl = SPA_CONTAINER_OF (loop, PinosMainLoopImpl, this);

  pinos_log_debug ("main-loop %p: run", impl);

  impl->running = true;
  pinos_loop_enter (loop->loop);
  while (impl->running) {
    pinos_loop_iterate (loop->loop, -1);
  }
  pinos_loop_leave (loop->loop);
}
