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

#ifndef __PINOS_MAIN_LOOP_H__
#define __PINOS_MAIN_LOOP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/include/spa/loop.h>
#include <pinos/client/loop.h>

typedef struct _PinosMainLoop PinosMainLoop;

/**
 * PinosMainLoop:
 *
 * Pinos main-loop interface.
 */
struct _PinosMainLoop {
  PinosLoop    *loop;

  PINOS_SIGNAL (destroy_signal, (PinosListener *listener,
                                 PinosMainLoop *loop));
};

PinosMainLoop *     pinos_main_loop_new                     (void);
void                pinos_main_loop_destroy                 (PinosMainLoop *loop);

void                pinos_main_loop_run                     (PinosMainLoop *loop);
void                pinos_main_loop_quit                    (PinosMainLoop *loop);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_MAIN_LOOP_H__ */
