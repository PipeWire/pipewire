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
#include <pinos/client/signal.h>

typedef struct _PinosLoop PinosLoop;

typedef void (*PinosLoopHook)    (PinosLoop *loop,
                                  void      *data);

typedef struct _PinosSource PinosSource;

typedef void (*PinosSourceIOFunc)     (PinosSource *source,
                                       int          fd,
                                       SpaIO        mask,
                                       void        *data);
typedef void (*PinosSourceIdleFunc)   (PinosSource *source,
                                       void        *data);
typedef void (*PinosSourceEventFunc)  (PinosSource *source,
                                       void        *data);
typedef void (*PinosSourceTimerFunc)  (PinosSource *source,
                                       void        *data);
typedef void (*PinosSourceSignalFunc) (PinosSource *source,
                                       int          signal_number,
                                       void        *data);
/**
 * PinosLoop:
 *
 * Pinos loop interface.
 */
struct _PinosLoop {
  SpaLoop *loop;

  PINOS_SIGNAL  (destroy_signal,   (PinosListener *listener,
                                    PinosLoop     *loop));
};

PinosLoop *    pinos_loop_new             (void);
void           pinos_loop_destroy         (PinosLoop *loop);

int            pinos_loop_get_fd          (PinosLoop *loop);

void           pinos_loop_set_hooks       (PinosLoop     *loop,
                                           PinosLoopHook  pre_func,
                                           PinosLoopHook  post_func,
                                           void          *data);
void           pinos_loop_enter_thread    (PinosLoop     *loop);
void           pinos_loop_leave_thread    (PinosLoop     *loop);

SpaResult      pinos_loop_iterate         (PinosLoop     *loop,
                                           int            timeout);

#define pinos_loop_add_source(l,s)        ((l)->loop->add_source((l)->loop,s);
#define pinos_loop_update_source(l,s)     ((l)->loop->update_source(s);
#define pinos_loop_remove_source(l,s)     ((l)->loop->remove_source(s);

PinosSource *  pinos_loop_add_io          (PinosLoop            *loop,
                                           int                   fd,
                                           SpaIO                 mask,
                                           bool                  close,
                                           PinosSourceIOFunc     func,
                                           void                 *data);
SpaResult      pinos_source_io_update     (PinosSource          *source,
                                           SpaIO                 mask);

PinosSource *  pinos_loop_add_idle        (PinosLoop            *loop,
                                           PinosSourceIdleFunc   func,
                                           void                 *data);
void           pinos_source_idle_enable   (PinosSource          *source,
                                           bool                  enabled);

PinosSource *  pinos_loop_add_event       (PinosLoop            *loop,
                                           PinosSourceEventFunc  func,
                                           void                 *data);
void           pinos_source_event_signal  (PinosSource          *source);

PinosSource *  pinos_loop_add_timer       (PinosLoop            *loop,
                                           PinosSourceTimerFunc  func,
                                           void                 *data);
SpaResult      pinos_source_timer_update  (PinosSource          *source,
                                           struct timespec      *value,
                                           struct timespec      *interval,
                                           bool                  absolute);

PinosSource *  pinos_loop_add_signal      (PinosLoop            *loop,
                                           int                   signal_number,
                                           PinosSourceSignalFunc func,
                                           void                 *data);

void           pinos_source_destroy       (PinosSource          *source);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_LOOP_H__ */
