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

#ifndef __PINOS_RTLOOP_H__
#define __PINOS_RTLOOP_H__

#include <glib-object.h>

G_BEGIN_DECLS

#include <spa/include/spa/poll.h>

typedef struct _PinosRTLoop PinosRTLoop;
typedef struct _PinosRTLoopClass PinosRTLoopClass;
typedef struct _PinosRTLoopPrivate PinosRTLoopPrivate;

#define PINOS_TYPE_RTLOOP                 (pinos_rtloop_get_type ())
#define PINOS_IS_RTLOOP(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_RTLOOP))
#define PINOS_IS_RTLOOP_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_RTLOOP))
#define PINOS_RTLOOP_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_RTLOOP, PinosRTLoopClass))
#define PINOS_RTLOOP(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_RTLOOP, PinosRTLoop))
#define PINOS_RTLOOP_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_RTLOOP, PinosRTLoopClass))
#define PINOS_RTLOOP_CAST(obj)            ((PinosRTLoop*)(obj))
#define PINOS_RTLOOP_CLASS_CAST(klass)    ((PinosRTLoopClass*)(klass))

/**
 * PinosRTLoop:
 *
 * Pinos rt-loop class.
 */
struct _PinosRTLoop {
  GObject object;

  SpaPoll poll;

  PinosRTLoopPrivate *priv;
};

/**
 * PinosRTLoopClass:
 *
 * Pinos rt-loop class.
 */
struct _PinosRTLoopClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType               pinos_rtloop_get_type                (void);

PinosRTLoop *       pinos_rtloop_new                     (void);


gboolean            pinos_rtloop_add_poll                (PinosRTLoop *loop,
                                                          SpaPollItem *item);
gboolean            pinos_rtloop_update_poll             (PinosRTLoop *loop,
                                                          SpaPollItem *item);
gboolean            pinos_rtloop_remove_poll             (PinosRTLoop *loop,
                                                          SpaPollItem *item);

G_END_DECLS

#endif /* __PINOS_RTLOOP_H__ */
