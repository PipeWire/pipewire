/* Pulsevideo
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

#ifndef __PV_STREAM_H__
#define __PV_STREAM_H__

#include <gio/gio.h>
#include <glib-object.h>

#include "pv-context.h"

G_BEGIN_DECLS

#define PV_TYPE_STREAM                 (pv_stream_get_type ())
#define PV_IS_STREAM(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PV_TYPE_STREAM))
#define PV_IS_STREAM_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PV_TYPE_STREAM))
#define PV_STREAM_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PV_TYPE_STREAM, PvStreamClass))
#define PV_STREAM(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PV_TYPE_STREAM, PvStream))
#define PV_STREAM_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PV_TYPE_STREAM, PvStreamClass))
#define PV_STREAM_CAST(obj)            ((PvStream*)(obj))
#define PV_STREAM_CLASS_CAST(klass)    ((PvStreamClass*)(klass))

typedef struct _PvStream PvStream;
typedef struct _PvStreamClass PvStreamClass;
typedef struct _PvStreamPrivate PvStreamPrivate;

typedef enum {
  PV_STREAM_STATE_UNCONNECTED = 0,
  PV_STREAM_STATE_CONNECTING  = 1,
  PV_STREAM_STATE_READY       = 2,
  PV_STREAM_STATE_STARTING    = 3,
  PV_STREAM_STATE_STREAMING   = 4,
  PV_STREAM_STATE_ERROR       = 5
} PvStreamState;


typedef enum {
  PV_STREAM_FLAGS_NONE = 0,
} PvStreamFlags;

typedef struct {
  guint32 flags;
  guint32 seq;
  gint64 pts;
  gint64 dts_offset;
  guint64 offset;
  guint64 size;
  GSocketControlMessage *message;
} PvBufferInfo;

typedef enum {
  PV_STREAM_MODE_SOCKET = 0,
  PV_STREAM_MODE_BUFFER = 1,
} PvStreamMode;

/**
 * PvStream:
 *
 * Pulsevideo stream object class.
 */
struct _PvStream {
  GObject object;

  PvStreamPrivate *priv;
};

/**
 * PvStreamClass:
 *
 * Pulsevideo stream object class.
 */
struct _PvStreamClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType           pv_stream_get_type             (void);


PvStream *      pv_stream_new                  (PvContext * context,
                                                const gchar *name);

PvStreamState   pv_stream_get_state            (PvStream *stream);

gboolean        pv_stream_connect_capture      (PvStream *stream,
                                                const gchar *source,
                                                PvStreamFlags flags);
gboolean        pv_stream_disconnect           (PvStream *stream);

gboolean        pv_stream_start                (PvStream *stream, PvStreamMode mode);
gboolean        pv_stream_stop                 (PvStream *stream);

gboolean        pv_stream_capture_buffer       (PvStream *stream,
                                                PvBufferInfo *info);

G_END_DECLS

#endif /* __PV_STREAM_H__ */

