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

#ifndef __PIPEWIRE_MAIN_LOOP_H__
#define __PIPEWIRE_MAIN_LOOP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/include/spa/loop.h>
#include <pipewire/client/loop.h>

/**
 * pa_main_loop:
 *
 * PipeWire main-loop interface.
 */
struct pw_main_loop {
  struct pw_loop    *loop;

  PW_SIGNAL (destroy_signal, (struct pw_listener *listener,
                              struct pw_main_loop *loop));
};

struct pw_main_loop * pw_main_loop_new                     (void);
void                  pw_main_loop_destroy                 (struct pw_main_loop *loop);

void                  pw_main_loop_run                     (struct pw_main_loop *loop);
void                  pw_main_loop_quit                    (struct pw_main_loop *loop);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_MAIN_LOOP_H__ */
