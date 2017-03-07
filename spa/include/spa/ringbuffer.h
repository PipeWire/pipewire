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

#define SPA_RINGBUFFER_URI             "http://spaplug.in/ns/ringbuffer"
#define SPA_RINGBUFFER_PREFIX          SPA_RINGBUFFER_URI "#"

#include <string.h>

#include <spa/defs.h>
#include <spa/barrier.h>

typedef struct {
  uint32_t  offset;
  uint32_t  len;
} SpaRingbufferArea;

/**
 * SpaRingbuffer:
 * @readindex: the current read index
 * @writeindex: the current write index
 * @size: the size of the ringbuffer
 * @size_mask: mask if @size is power of 2
 */
struct _SpaRingbuffer {
  volatile uint32_t     readindex;
  volatile uint32_t     writeindex;
  uint32_t              size;
  uint32_t              mask;
  uint32_t              mask2;
};

/**
 * spa_ringbuffer_init:
 * @rbuf: a #SpaRingbuffer
 * @data: pointer to an array
 * @size: the number of elements in @data
 *
 * Initialize a #SpaRingbuffer with @data and @size.
 * Size must be a power of 2.
 *
 * Returns: %SPA_RESULT_OK, unless size is not a power of 2.
 */
static inline SpaResult
spa_ringbuffer_init (SpaRingbuffer *rbuf,
                     uint32_t size)
{
  if (SPA_UNLIKELY ((size & (size - 1)) != 0))
    return SPA_RESULT_ERROR;

  rbuf->size = size;
  rbuf->mask = size - 1;
  rbuf->mask2 = (size << 1) - 1;
  rbuf->readindex = 0;
  rbuf->writeindex = 0;

  return SPA_RESULT_OK;
}

/**
 * spa_ringbuffer_clear:
 * @rbuf: a #SpaRingbuffer
 *
 * Clear @rbuf
 */
static inline void
spa_ringbuffer_clear (SpaRingbuffer *rbuf)
{
  rbuf->readindex = 0;
  rbuf->writeindex = 0;
}

static inline uint32_t
spa_ringbuffer_get_read_offset (SpaRingbuffer *rbuf,
                                uint32_t      *offset)
{
  uint32_t avail, r;

  r = rbuf->readindex;
  *offset = r & rbuf->mask;
  avail = (rbuf->writeindex - r) & rbuf->mask2;

  spa_barrier_read();

  return avail;
}

/**
 * spa_ringbuffer_get_read_areas:
 * @rbuf: a #SpaRingbuffer
 * @areas: an array of #SpaRingbufferArea
 *
 * Fill @areas with pointers to read from. The total amount of
 * bytes that can be read can be obtained by summing the areas len fields.
 */
static inline uint32_t
spa_ringbuffer_get_read_areas (SpaRingbuffer      *rbuf,
                               SpaRingbufferArea   areas[2])
{
  uint32_t avail, end, r;

  avail = spa_ringbuffer_get_read_offset (rbuf, &r);
  end = r + avail;

  areas[0].offset = r;
  areas[1].offset = 0;

  if (SPA_UNLIKELY (end > rbuf->size)) {
    areas[0].len = rbuf->size - r;
    areas[1].len = end - rbuf->size;
  } else {
    areas[0].len = avail;
    areas[1].len = 0;
  }
  return avail;
}

static inline void
spa_ringbuffer_read_data (SpaRingbuffer      *rbuf,
                          void               *buffer,
                          SpaRingbufferArea   areas[2],
                          void               *data,
                          uint32_t            size)
{
  if (SPA_LIKELY (size < areas[0].len))
    memcpy (data, buffer + areas[0].offset, size);
  else {
    memcpy (data, buffer + areas[0].offset, areas[0].len);
    memcpy (data + areas[0].len, buffer, size - areas[0].len);
  }
}

/**
 * spa_ringbuffer_read_advance:
 * @rbuf: a #SpaRingbuffer
 * @len: number of bytes to advance
 *
 * Advance the read pointer by @len
 */
static inline void
spa_ringbuffer_read_advance (SpaRingbuffer      *rbuf,
                             int32_t             len)
{
  spa_barrier_full();
  rbuf->readindex = (rbuf->readindex + len) & rbuf->mask2;
}

static inline uint32_t
spa_ringbuffer_get_write_offset (SpaRingbuffer *rbuf,
                                 uint32_t      *offset)
{
  uint32_t avail, w;

  w = rbuf->writeindex;
  *offset = w & rbuf->mask;
  avail = rbuf->size - ((w - rbuf->readindex) & rbuf->mask2);
  spa_barrier_full();

  return avail;
}

/**
 * spa_ringbuffer_get_write_areas:
 * @rbuf: a #SpaRingbuffer
 * @areas: an array of #SpaRingbufferArea
 *
 * Fill @areas with pointers to write to. The total amount of
 * bytes that can be written can be obtained by summing the areas len fields.
 */
static inline uint32_t
spa_ringbuffer_get_write_areas (SpaRingbuffer      *rbuf,
                                SpaRingbufferArea   areas[2])
{
  uint32_t avail, end, w;

  avail = spa_ringbuffer_get_write_offset (rbuf, &w);
  end = w + avail;

  areas[0].offset = w;
  areas[1].offset = 0;

  if (SPA_UNLIKELY (end > rbuf->size)) {
    areas[0].len = rbuf->size - w;
    areas[1].len = end - rbuf->size;
  } else {
    areas[0].len = avail;
    areas[1].len = 0;
  }
  return avail;
}

static inline void
spa_ringbuffer_write_data (SpaRingbuffer      *rbuf,
                           void               *buffer,
                           SpaRingbufferArea   areas[2],
                           void               *data,
                           uint32_t            size)
{
  if (SPA_LIKELY (size < areas[0].len))
    memcpy (buffer + areas[0].offset, data, size);
  else {
    memcpy (buffer + areas[0].offset, data, areas[0].len);
    memcpy (buffer, data + areas[0].len, size - areas[0].len);
  }
}

/**
 * spa_ringbuffer_write_advance:
 * @rbuf: a #SpaRingbuffer
 * @len: number of bytes to advance
 *
 * Advance the write pointer by @len
 *
 */
static inline void
spa_ringbuffer_write_advance (SpaRingbuffer      *rbuf,
                              int32_t             len)
{
  spa_barrier_write();
  rbuf->writeindex = (rbuf->writeindex + len) & rbuf->mask2;
}


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_RINGBUFFER_H__ */
