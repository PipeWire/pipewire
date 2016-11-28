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

#ifndef __PINOS_LOOP_H__
#define __PINOS_LOOP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/include/spa/list.h>
#include <spa/include/spa/loop.h>
#include <pinos/client/sig.h>

typedef struct _PinosLoop PinosLoop;

/**
 * PinosLoop:
 *
 * Pinos loop interface.
 */
struct _PinosLoop {
  SpaLoop        *loop;
  SpaLoopControl *control;
  SpaLoopUtils   *utils;

  PINOS_SIGNAL  (destroy_signal,   (PinosListener *listener,
                                    PinosLoop     *loop));
};

PinosLoop *    pinos_loop_new             (void);
void           pinos_loop_destroy         (PinosLoop *loop);

#define pinos_loop_add_source(l,...)      spa_loop_add_source((l)->loop,__VA_ARGS__)
#define pinos_loop_update_source(l,...)   spa_loop_update_source(__VA_ARGS__)
#define pinos_loop_remove_source(l,...)   spa_loop_remove_source(__VA_ARGS__)
#define pinos_loop_invoke(l,...)          spa_loop_invoke((l)->loop,__VA_ARGS__)

#define pinos_loop_get_fd(l)              spa_loop_control_get_fd((l)->control)
#define pinos_loop_set_hooks(l,...)       spa_loop_control_set_hooks((l)->control,__VA_ARGS__)
#define pinos_loop_enter(l)               spa_loop_control_enter((l)->control)
#define pinos_loop_iterate(l,...)         spa_loop_control_iterate((l)->control,__VA_ARGS__)
#define pinos_loop_leave(l)               spa_loop_control_leave((l)->control)

#define pinos_loop_add_io(l,...)          spa_loop_utils_add_io((l)->utils,__VA_ARGS__)
#define pinos_loop_update_io(l,...)       spa_loop_utils_update_io((l)->utils,__VA_ARGS__)
#define pinos_loop_add_idle(l,...)        spa_loop_utils_add_idle((l)->utils,__VA_ARGS__)
#define pinos_loop_enable_idle(l,...)     spa_loop_utils_enable_idle((l)->utils,__VA_ARGS__)
#define pinos_loop_add_event(l,...)       spa_loop_utils_add_event((l)->utils,__VA_ARGS__)
#define pinos_loop_signal_event(l,...)    spa_loop_utils_signal_event((l)->utils,__VA_ARGS__)
#define pinos_loop_add_timer(l,...)       spa_loop_utils_add_timer((l)->utils,__VA_ARGS__)
#define pinos_loop_update_timer(l,...)    spa_loop_utils_update_timer((l)->utils,__VA_ARGS__)
#define pinos_loop_add_signal(l,...)      spa_loop_utils_add_signal((l)->utils,__VA_ARGS__)
#define pinos_loop_destroy_source(l,...)  spa_loop_utils_destroy_source((l)->utils,__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_LOOP_H__ */
