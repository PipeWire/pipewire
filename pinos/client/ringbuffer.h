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

#ifndef __PINOS_RINGBUFFER_H__
#define __PINOS_RINGBUFFER_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _PinosRingbuffer PinosRingbuffer;
typedef struct _PinosRingbufferClass PinosRingbufferClass;
typedef struct _PinosRingbufferPrivate PinosRingbufferPrivate;

#include <pinos/client/introspect.h>

#define PINOS_TYPE_RINGBUFFER                 (pinos_ringbuffer_get_type ())
#define PINOS_IS_RINGBUFFER(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_RINGBUFFER))
#define PINOS_IS_RINGBUFFER_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_RINGBUFFER))
#define PINOS_RINGBUFFER_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_RINGBUFFER, PinosRingbufferClass))
#define PINOS_RINGBUFFER(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_RINGBUFFER, PinosRingbuffer))
#define PINOS_RINGBUFFER_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_RINGBUFFER, PinosRingbufferClass))
#define PINOS_RINGBUFFER_CAST(obj)            ((PinosRingbuffer*)(obj))
#define PINOS_RINGBUFFER_CLASS_CAST(klass)    ((PinosRingbufferClass*)(klass))

/**
 * PinosRingbuffer:
 *
 * Pinos ringbuffer object class.
 */
struct _PinosRingbuffer {
  GObject object;

  PinosRingbufferPrivate *priv;
};

/**
 * PinosRingbufferClass:
 *
 * Pinos ringbuffer object class.
 */
struct _PinosRingbufferClass {
  GObjectClass parent_class;
};

typedef void (*PinosRingbufferCallback)    (PinosRingbuffer *rb, gpointer user_data);

typedef enum {
  PINOS_RINGBUFFER_MODE_READ,
  PINOS_RINGBUFFER_MODE_WRITE,
} PinosRingbufferMode;

typedef struct {
  gpointer data;
  gsize    len;
} PinosRingbufferArea;

/* normal GObject stuff */
GType               pinos_ringbuffer_get_type       (void);

PinosRingbuffer *   pinos_ringbuffer_new            (PinosRingbufferMode  mode,
                                                     gsize                size);
PinosRingbuffer *   pinos_ringbuffer_new_import     (PinosRingbufferMode  mode,
                                                     guint                fdsize,
                                                     int                  fd,
                                                     int                  semaphore);

gboolean            pinos_ringbuffer_get_read_areas  (PinosRingbuffer      *rbuf,
                                                      PinosRingbufferArea   areas[2]);
gboolean            pinos_ringbuffer_read_advance    (PinosRingbuffer      *rbuf,
                                                      gssize                len);

gboolean            pinos_ringbuffer_get_write_areas (PinosRingbuffer      *rbuf,
                                                      PinosRingbufferArea   areas[2]);
gboolean            pinos_ringbuffer_write_advance   (PinosRingbuffer      *rbuf,
                                                      gssize                len);

G_END_DECLS

#endif /* __PINOS_RINGBUFFER_H__ */
