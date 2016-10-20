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

#ifndef __PINOS_DATA_LOOP_H__
#define __PINOS_DATA_LOOP_H__

#include <glib-object.h>

G_BEGIN_DECLS

#include <spa/include/spa/poll.h>

typedef struct _PinosDataLoop PinosDataLoop;
typedef struct _PinosDataLoopClass PinosDataLoopClass;
typedef struct _PinosDataLoopPrivate PinosDataLoopPrivate;

#define PINOS_TYPE_DATA_LOOP                 (pinos_data_loop_get_type ())
#define PINOS_IS_DATA_LOOP(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_DATA_LOOP))
#define PINOS_IS_DATA_LOOP_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_DATA_LOOP))
#define PINOS_DATA_LOOP_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_DATA_LOOP, PinosDataLoopClass))
#define PINOS_DATA_LOOP(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_DATA_LOOP, PinosDataLoop))
#define PINOS_DATA_LOOP_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_DATA_LOOP, PinosDataLoopClass))
#define PINOS_DATA_LOOP_CAST(obj)            ((PinosDataLoop*)(obj))
#define PINOS_DATA_LOOP_CLASS_CAST(klass)    ((PinosDataLoopClass*)(klass))

/**
 * PinosDataLoop:
 *
 * Pinos rt-loop class.
 */
struct _PinosDataLoop {
  GObject object;

  SpaPoll poll;

  PinosDataLoopPrivate *priv;
};

/**
 * PinosDataLoopClass:
 *
 * Pinos rt-loop class.
 */
struct _PinosDataLoopClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType               pinos_data_loop_get_type                (void);

PinosDataLoop *     pinos_data_loop_new                     (void);

G_END_DECLS

#endif /* __PINOS_DATA_LOOP_H__ */
