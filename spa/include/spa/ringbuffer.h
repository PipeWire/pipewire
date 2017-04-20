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

#define SPA_TYPE__RingBuffer           SPA_TYPE_INTERFACE_BASE "RingBuffer"
#define SPA_TYPE_RINGBUFFER_BASE       SPA_TYPE__RingBuffer ":"

#include <string.h>

#include <spa/defs.h>
#include <spa/barrier.h>

/**
 * SpaRingbuffer:
 * @readindex: the current read index
 * @writeindex: the current write index
 * @size: the size of the ringbuffer must be power of 2
 * @mask: mask as @size - 1
 */
struct _SpaRingbuffer {
  uint32_t     readindex;
  uint32_t     writeindex;
  uint32_t     size;
  uint32_t     mask;
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

/**
 * spa_ringbuffer_get_read_index:
 * @rbuf: a #SpaRingbuffer
 * @index: the value of readindex, should be masked to get the
 *         offset in the ringbuffer memory
 *
 * Returns: number of available bytes to read. values < 0 mean
 *          there was an underrun. values > rbuf->size means there
 *          was an overrun.
 */
static inline int32_t
spa_ringbuffer_get_read_index (SpaRingbuffer *rbuf,
                               uint32_t      *index)
{
  int32_t avail;

  *index = rbuf->readindex;
  avail = (int32_t) (rbuf->writeindex - *index);
  spa_barrier_read();

  return avail;
}

/**
 * spa_ringbuffer_read_data:
 * @rbuf: a #SpaRingbuffer
 * @buffer: memory to read from
 * @offset: offset in @buffer to read from
 * @data: destination memory
 * @len: number of bytes to read
 *
 * Read @len bytes from @rbuf starting @offset. @offset must be masked
 * with the size of @rbuf and len should be smaller than the size.
 */
static inline void
spa_ringbuffer_read_data (SpaRingbuffer      *rbuf,
                          void               *buffer,
                          uint32_t            offset,
                          void               *data,
                          uint32_t            len)
{
  if (SPA_LIKELY (offset + len < rbuf->size)) {
    memcpy (data, buffer + offset, len);
  } else {
    uint32_t l0 = rbuf->size - offset;
    memcpy (data, buffer + offset, l0);
    memcpy (data + l0, buffer, len - l0);
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
  rbuf->readindex += len;
}

/**
 * spa_ringbuffer_get_write_index:
 * @rbuf: a #SpaRingbuffer
 * @index: the value of writeindex, should be masked to get the
 *         offset in the ringbuffer memory
 *
 * Returns: the fill level of @rbuf. values < 0 mean
 *          there was an underrun. values > rbuf->size means there
 *          was an overrun. Subsctract from the buffer size to get
 *          the number of bytes available for writing.
 */
static inline int32_t
spa_ringbuffer_get_write_index (SpaRingbuffer *rbuf,
                                uint32_t      *index)
{
  int32_t filled;

  *index = rbuf->writeindex;
  filled = (int32_t) (*index - rbuf->readindex);
  spa_barrier_full();

  return filled;
}

static inline void
spa_ringbuffer_write_data (SpaRingbuffer      *rbuf,
                           void               *buffer,
                           uint32_t            offset,
                           void               *data,
                           uint32_t            len)
{
  if (SPA_LIKELY (offset + len < rbuf->size)) {
    memcpy (buffer + offset, data, len);
  } else {
    uint32_t l0 = rbuf->size - offset;
    memcpy (buffer + offset, data, l0);
    memcpy (buffer, data + l0, len - l0);
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
  rbuf->writeindex += len;
}


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_RINGBUFFER_H__ */
