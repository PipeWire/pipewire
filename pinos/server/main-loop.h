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
typedef struct _PinosMainLoopClass PinosMainLoopClass;
typedef struct _PinosMainLoopPrivate PinosMainLoopPrivate;

#define PINOS_TYPE_MAIN_LOOP                 (pinos_main_loop_get_type ())
#define PINOS_IS_MAIN_LOOP(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_MAIN_LOOP))
#define PINOS_IS_MAIN_LOOP_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_MAIN_LOOP))
#define PINOS_MAIN_LOOP_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_MAIN_LOOP, PinosMainLoopClass))
#define PINOS_MAIN_LOOP(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_MAIN_LOOP, PinosMainLoop))
#define PINOS_MAIN_LOOP_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_MAIN_LOOP, PinosMainLoopClass))
#define PINOS_MAIN_LOOP_CAST(obj)            ((PinosMainLoop*)(obj))
#define PINOS_MAIN_LOOP_CLASS_CAST(klass)    ((PinosMainLoopClass*)(klass))

/**
 * PinosMainLoop:
 *
 * Pinos rt-loop class.
 */
struct _PinosMainLoop {
  GObject object;

  SpaPoll poll;

  PinosMainLoopPrivate *priv;
};

/**
 * PinosMainLoopClass:
 *
 * Pinos rt-loop class.
 */
struct _PinosMainLoopClass {
  GObjectClass parent_class;
};

typedef void (*PinosEventFunc) (SpaNodeEvent *event,
                                void         *user_data);

typedef void (*PinosDeferFunc) (gpointer       obj,
                                gpointer       data,
                                SpaResult      res,
                                gulong         id);

/* normal GObject stuff */
GType               pinos_main_loop_get_type                (void);

PinosMainLoop *     pinos_main_loop_new                     (void);


gulong              pinos_main_loop_defer                   (PinosMainLoop  *loop,
                                                             gpointer        obj,
                                                             SpaResult       res,
                                                             PinosDeferFunc  func,
                                                             gpointer        data,
                                                             GDestroyNotify  notify);
void                pinos_main_loop_defer_cancel            (PinosMainLoop  *loop,
                                                             gpointer        obj,
                                                             gulong          id);
void                pinos_main_loop_defer_complete          (PinosMainLoop  *loop,
                                                             gpointer        obj,
                                                             uint32_t        seq,
                                                             SpaResult       res);

G_END_DECLS

#endif /* __PINOS_MAIN_LOOP_H__ */
