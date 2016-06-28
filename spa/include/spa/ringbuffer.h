/* Simple Plugin API
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

#ifndef __SPA_RINGBUFFER_H__
#define __SPA_RINGBUFFER_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaRingbuffer SpaRingbuffer;

#include <spa/defs.h>

typedef struct {
  uint8_t *data;
  size_t   len;
} SpaRingbufferArea;

/**
 * SpaRingbuffer:
 * @data: pointer to data
 * @readindex: the current read index
 * @writeindex: the current write index
 * @size: the size of the ringbuffer
 * @size_mask: mask if @size is power of 2
 */
struct _SpaRingbuffer {
  uint8_t            *data;
  volatile size_t     readindex;
  volatile size_t     writeindex;
  size_t              size;
  size_t              size_mask;
};

SpaResult   spa_ringbuffer_init              (SpaRingbuffer *rbuf,
                                              uint8_t *data, size_t size);

SpaResult   spa_ringbuffer_clear             (SpaRingbuffer *rbuf);

SpaResult   spa_ringbuffer_get_read_areas    (SpaRingbuffer      *rbuf,
                                              SpaRingbufferArea   areas[2]);
SpaResult   spa_ringbuffer_read_advance      (SpaRingbuffer      *rbuf,
                                              ssize_t             len);

SpaResult   spa_ringbuffer_get_write_areas   (SpaRingbuffer      *rbuf,
                                              SpaRingbufferArea   areas[2]);
SpaResult   spa_ringbuffer_write_advance     (SpaRingbuffer      *rbuf,
                                              ssize_t             len);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_RINGBUFFER_H__ */
