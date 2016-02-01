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

#ifndef __PINOS_MAIN_LOOP_H__
#define __PINOS_MAIN_LOOP_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define PINOS_TYPE_MAIN_LOOP                 (pinos_main_loop_get_type ())
#define PINOS_IS_MAIN_LOOP(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_MAIN_LOOP))
#define PINOS_IS_MAIN_LOOP_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_MAIN_LOOP))
#define PINOS_MAIN_LOOP_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_MAIN_LOOP, PinosMainLoopClass))
#define PINOS_MAIN_LOOP(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_MAIN_LOOP, PinosMainLoop))
#define PINOS_MAIN_LOOP_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_MAIN_LOOP, PinosMainLoopClass))
#define PINOS_MAIN_LOOP_CAST(obj)            ((PinosMainLoop*)(obj))
#define PINOS_MAIN_LOOP_CLASS_CAST(klass)    ((PinosMainLoopClass*)(klass))

typedef struct _PinosMainLoop PinosMainLoop;
typedef struct _PinosMainLoopClass PinosMainLoopClass;
typedef struct _PinosMainLoopPrivate PinosMainLoopPrivate;


/**
 * PinosMainLoop:
 *
 * Pinos main loop object class.
 */
struct _PinosMainLoop {
  GObject object;

  PinosMainLoopPrivate *priv;
};

/**
 * PinosMainLoopClass:
 *
 * Pinos main loop object class.
 */
struct _PinosMainLoopClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType            pinos_main_loop_get_type        (void);

PinosMainLoop *  pinos_main_loop_new             (GMainContext * context,
                                                  const gchar   *name);

GMainLoop *      pinos_main_loop_get_impl        (PinosMainLoop *loop);

gboolean         pinos_main_loop_start           (PinosMainLoop *loop, GError **error);
void             pinos_main_loop_stop            (PinosMainLoop *loop);

void             pinos_main_loop_lock            (PinosMainLoop *loop);
void             pinos_main_loop_unlock          (PinosMainLoop *loop);

void             pinos_main_loop_wait            (PinosMainLoop *loop);
void             pinos_main_loop_signal          (PinosMainLoop *loop, gboolean wait_for_accept);
void             pinos_main_loop_accept          (PinosMainLoop *loop);

gboolean         pinos_main_loop_in_thread       (PinosMainLoop *loop);


G_END_DECLS

#endif /* __PINOS_MAIN_LOOP_H__ */
