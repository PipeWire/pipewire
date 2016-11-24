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

#ifndef __PINOS_THREAD_MAIN_LOOP_H__
#define __PINOS_THREAD_MAIN_LOOP_H__

#include <pinos/client/loop.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _PinosThreadMainLoop PinosThreadMainLoop;

/**
 * PinosThreadMainLoop:
 *
 * Pinos main loop object class.
 */
struct _PinosThreadMainLoop {
  PinosLoop *loop;
  char      *name;

  PINOS_SIGNAL (destroy_signal, (PinosListener       *listener,
                                 PinosThreadMainLoop *loop));
};

PinosThreadMainLoop *  pinos_thread_main_loop_new             (PinosLoop  *loop,
                                                               const char *name);
void                   pinos_thread_main_loop_destroy         (PinosThreadMainLoop *loop);

SpaResult              pinos_thread_main_loop_start           (PinosThreadMainLoop *loop);
void                   pinos_thread_main_loop_stop            (PinosThreadMainLoop *loop);

void                   pinos_thread_main_loop_lock            (PinosThreadMainLoop *loop);
void                   pinos_thread_main_loop_unlock          (PinosThreadMainLoop *loop);

void                   pinos_thread_main_loop_wait            (PinosThreadMainLoop *loop);
void                   pinos_thread_main_loop_signal          (PinosThreadMainLoop *loop,
                                                               bool                 wait_for_accept);
void                   pinos_thread_main_loop_accept          (PinosThreadMainLoop *loop);

bool                   pinos_thread_main_loop_in_thread       (PinosThreadMainLoop *loop);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_THREAD_MAIN_LOOP_H__ */
