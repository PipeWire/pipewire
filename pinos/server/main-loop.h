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

#include <glib-object.h>

G_BEGIN_DECLS

#include <spa/include/spa/poll.h>
#include <spa/include/spa/node-event.h>

typedef struct _PinosMainLoop PinosMainLoop;

typedef void (*PinosDeferFunc) (void       *obj,
                                void       *data,
                                SpaResult   res,
                                gulong      id);

/**
 * PinosMainLoop:
 *
 * Pinos main-loop interface.
 */
struct _PinosMainLoop {
  SpaPoll *poll;

  void         (*run)             (PinosMainLoop  *loop);
  void         (*quit)            (PinosMainLoop  *loop);

  uint32_t     (*defer)           (PinosMainLoop  *loop,
                                   void           *obj,
                                   SpaResult       res,
                                   PinosDeferFunc  func,
                                   void           *data,
                                   GDestroyNotify  notify);
  void         (*defer_cancel)    (PinosMainLoop  *loop,
                                   void           *obj,
                                   uint32_t        id);
  bool         (*defer_complete)  (PinosMainLoop  *loop,
                                   void           *obj,
                                   uint32_t        seq,
                                   SpaResult       res);
};

PinosMainLoop *     pinos_main_loop_new                     (GMainContext *context);
void                pinos_main_loop_destroy                 (PinosMainLoop *loop);

#define pinos_main_loop_run(m)                 (m)->run(m)
#define pinos_main_loop_quit(m)                (m)->quit(m)

#define pinos_main_loop_defer(m,...)           (m)->defer(m,__VA_ARGS__)
#define pinos_main_loop_defer_cancel(m,...)    (m)->defer_cancel(m,__VA_ARGS__)
#define pinos_main_loop_defer_complete(m,...)  (m)->defer_complete(m,__VA_ARGS__)

G_END_DECLS

#endif /* __PINOS_MAIN_LOOP_H__ */
