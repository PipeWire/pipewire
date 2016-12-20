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

#include <spa/include/spa/buffer.h>
#include <spa/include/spa/format.h>

#include <pinos/client/context.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _PinosStream PinosStream;

typedef enum {
  PINOS_STREAM_STATE_ERROR       = -1,
  PINOS_STREAM_STATE_UNCONNECTED = 0,
  PINOS_STREAM_STATE_CONNECTING  = 1,
  PINOS_STREAM_STATE_CONFIGURE   = 2,
  PINOS_STREAM_STATE_READY       = 3,
  PINOS_STREAM_STATE_PAUSED      = 4,
  PINOS_STREAM_STATE_STREAMING   = 5
} PinosStreamState;

const char * pinos_stream_state_as_string (PinosStreamState state);

typedef enum {
  PINOS_STREAM_FLAG_NONE = 0,
  PINOS_STREAM_FLAG_AUTOCONNECT = (1 << 0),
} PinosStreamFlags;

typedef enum {
  PINOS_STREAM_MODE_BUFFER = 0,
  PINOS_STREAM_MODE_RINGBUFFER = 1,
} PinosStreamMode;

typedef struct {
  int64_t ticks;
  int32_t rate;
} PinosTime;

/**
 * PinosStream:
 *
 * Pinos stream object class.
 */
struct _PinosStream {
  PinosContext    *context;
  SpaList          link;

  char            *name;
  PinosProperties *properties;

  PINOS_SIGNAL (destroy_signal, (PinosListener *listener,
                                 PinosStream   *stream));

  PinosStreamState state;
  char            *error;
  PINOS_SIGNAL (state_changed, (PinosListener *listener,
                                PinosStream   *stream));

  PINOS_SIGNAL (format_changed, (PinosListener *listener,
                                 PinosStream   *stream,
                                 SpaFormat     *format));

  PINOS_SIGNAL (add_buffer,    (PinosListener *listener,
                                PinosStream   *stream,
                                uint32_t       id));
  PINOS_SIGNAL (remove_buffer, (PinosListener *listener,
                                PinosStream   *stream,
                                uint32_t       id));
  PINOS_SIGNAL (new_buffer,    (PinosListener *listener,
                                PinosStream   *stream,
                                uint32_t       id));
  PINOS_SIGNAL (need_buffer,   (PinosListener *listener,
                                PinosStream   *stream));
};

PinosStream *    pinos_stream_new               (PinosContext    *context,
                                                 const char      *name,
                                                 PinosProperties *props);
void             pinos_stream_destroy           (PinosStream     *stream);

bool             pinos_stream_connect           (PinosStream      *stream,
                                                 PinosDirection    direction,
                                                 PinosStreamMode   mode,
                                                 const char       *port_path,
                                                 PinosStreamFlags  flags,
                                                 unsigned int      n_possible_formats,
                                                 SpaFormat       **possible_formats);
bool             pinos_stream_disconnect        (PinosStream      *stream);

bool             pinos_stream_finish_format     (PinosStream     *stream,
                                                 SpaResult        res,
                                                 SpaAllocParam  **params,
                                                 unsigned int     n_params);



bool             pinos_stream_start             (PinosStream     *stream);
bool             pinos_stream_stop              (PinosStream     *stream);

bool             pinos_stream_get_time          (PinosStream     *stream,
                                                 PinosTime       *time);

uint32_t         pinos_stream_get_empty_buffer  (PinosStream     *stream);
bool             pinos_stream_recycle_buffer    (PinosStream     *stream,
                                                 uint32_t         id);
SpaBuffer *      pinos_stream_peek_buffer       (PinosStream     *stream,
                                                 uint32_t         id);
bool             pinos_stream_send_buffer       (PinosStream     *stream,
                                                 uint32_t         id);
#ifdef __cplusplus
}
#endif

#endif /* __PINOS_STREAM_H__ */
