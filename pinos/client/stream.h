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

#ifndef __PINOS_STREAM_H__
#define __PINOS_STREAM_H__

#include <glib-object.h>

#include <pinos/client/buffer.h>
#include <pinos/client/context.h>

G_BEGIN_DECLS

#define PINOS_TYPE_STREAM                 (pinos_stream_get_type ())
#define PINOS_IS_STREAM(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_STREAM))
#define PINOS_IS_STREAM_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_STREAM))
#define PINOS_STREAM_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_STREAM, PinosStreamClass))
#define PINOS_STREAM(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_STREAM, PinosStream))
#define PINOS_STREAM_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_STREAM, PinosStreamClass))
#define PINOS_STREAM_CAST(obj)            ((PinosStream*)(obj))
#define PINOS_STREAM_CLASS_CAST(klass)    ((PinosStreamClass*)(klass))

typedef struct _PinosStream PinosStream;
typedef struct _PinosStreamClass PinosStreamClass;
typedef struct _PinosStreamPrivate PinosStreamPrivate;

typedef enum {
  PINOS_STREAM_STATE_ERROR       = -1,
  PINOS_STREAM_STATE_UNCONNECTED = 0,
  PINOS_STREAM_STATE_CONNECTING  = 1,
  PINOS_STREAM_STATE_READY       = 2,
  PINOS_STREAM_STATE_STARTING    = 3,
  PINOS_STREAM_STATE_STREAMING   = 4
} PinosStreamState;

const gchar * pinos_stream_state_as_string (PinosStreamState state);

typedef enum {
  PINOS_STREAM_FLAGS_NONE = 0,
} PinosStreamFlags;

typedef enum {
  PINOS_STREAM_MODE_SOCKET = 0,
  PINOS_STREAM_MODE_BUFFER = 1,
} PinosStreamMode;

/**
 * PinosStream:
 *
 * Pinos stream object class.
 */
struct _PinosStream {
  GObject object;

  PinosStreamPrivate *priv;
};

/**
 * PinosStreamClass:
 *
 * Pinos stream object class.
 */
struct _PinosStreamClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType            pinos_stream_get_type          (void);


PinosStream *    pinos_stream_new               (PinosContext    *context,
                                                 const gchar     *name,
                                                 PinosProperties *props);

PinosStreamState pinos_stream_get_state         (PinosStream *stream);
const GError *   pinos_stream_get_error         (PinosStream *stream);

gboolean         pinos_stream_connect           (PinosStream      *stream,
                                                 PinosDirection    direction,
                                                 const gchar      *port_path,
                                                 PinosStreamFlags  flags,
                                                 GBytes           *possible_formats);
gboolean         pinos_stream_connect_provide   (PinosStream      *stream,
                                                 PinosStreamFlags  flags,
                                                 GBytes           *possible_formats);
gboolean         pinos_stream_disconnect        (PinosStream      *stream);

gboolean         pinos_stream_start             (PinosStream     *stream,
                                                 GBytes          *format,
                                                 PinosStreamMode  mode);
gboolean         pinos_stream_stop              (PinosStream     *stream);

gboolean         pinos_stream_get_buffer        (PinosStream     *stream,
                                                 PinosBuffer     **buffer);
gboolean         pinos_stream_send_buffer       (PinosStream     *stream,
                                                 PinosBuffer     *buffer);

G_END_DECLS

#endif /* __PINOS_STREAM_H__ */
