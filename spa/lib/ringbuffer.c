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

#include <spa/ringbuffer.h>

/**
 * spa_ringbuffer_init:
 * @rbuf: a #SpaRingbuffer
 * @data: pointer to data
 * @size: size of @data
 *
 * Initialize a #SpaRingbuffer with @data and @size.
 * When size is a power of 2, size_mask will be set with the mask to
 * efficiently wrap around the indexes.
 *
 * Returns: #SPA_RESULT_OK on success
 *          #SPA_RESULT_INVALID_ARGUMENTS when data or rbuf is %NULL
 */
SpaResult
spa_ringbuffer_init (SpaRingbuffer *rbuf,
                     uint8_t *data, size_t size)
{
  if (rbuf == NULL || data == NULL || size == 0)
    return SPA_RESULT_INVALID_ARGUMENTS;

  rbuf->data = data;
  rbuf->size = size;
  rbuf->readindex = 0;
  rbuf->writeindex = 0;
  if ((size & (size - 1)) == 0)
    rbuf->size_mask = size - 1;
  else
    rbuf->size_mask = 0;

  return SPA_RESULT_OK;
}

/**
 * spa_ringbuffer_clear:
 * @rbuf: a #SpaRingbuffer
 *
 * Clear @rbuf
 *
 * Returns: #SPA_RESULT_OK
 */
SpaResult
spa_ringbuffer_clear (SpaRingbuffer *rbuf)
{
  rbuf->readindex = 0;
  rbuf->writeindex = 0;
  return SPA_RESULT_OK;
}

/**
 * spa_ringbuffer_get_read_areas:
 * @rbuf: a #SpaRingbuffer
 * @areas: an array of #SpaRingbufferArea
 *
 * Fill @areas with pointers to read from. The total amount of
 * bytes that can be read can be obtained by summing the areas len fields.
 *
 * Returns: #SPA_RESULT_OK
 */
SpaResult
spa_ringbuffer_get_read_areas (SpaRingbuffer      *rbuf,
                               SpaRingbufferArea   areas[2])
{
  size_t avail, end, w, r;

  w = rbuf->writeindex;
  r = rbuf->readindex;

  if (w > r) {
    avail = w - r;
  } else {
    avail = (w - r + rbuf->size);
    avail = (rbuf->size_mask ? avail & rbuf->size_mask : avail % rbuf->size);
  }
  end = r + avail;
  if (end > rbuf->size) {
    areas[0].data = &rbuf->data[r];
    areas[0].len = rbuf->size - r;
    areas[1].data = rbuf->data;
    areas[1].len = end - rbuf->size;
  } else {
    areas[0].data = &rbuf->data[r];
    areas[0].len = avail;
    areas[1].len = 0;
  }
  return SPA_RESULT_OK;
}

/**
 * spa_ringbuffer_read_advance:
 * @rbuf: a #SpaRingbuffer
 * @len: number of bytes to advance
 *
 * Advance the read pointer by @len
 *
 * Returns: #SPA_RESULT_OK
 */
SpaResult
spa_ringbuffer_read_advance (SpaRingbuffer      *rbuf,
                             ssize_t             len)
{
  size_t tmp = rbuf->readindex + len;
  rbuf->readindex = (rbuf->size_mask ? tmp & rbuf->size_mask : tmp % rbuf->size);
  return SPA_RESULT_OK;
}

/**
 * spa_ringbuffer_get_write_areas:
 * @rbuf: a #SpaRingbuffer
 * @areas: an array of #SpaRingbufferArea
 *
 * Fill @areas with pointers to write to. The total amount of
 * bytes that can be written can be obtained by summing the areas len fields.
 *
 * Returns: #SPA_RESULT_OK
 */
SpaResult
spa_ringbuffer_get_write_areas (SpaRingbuffer      *rbuf,
                                SpaRingbufferArea   areas[2])
{
  size_t avail, end, w, r;

  w = rbuf->writeindex;
  r = rbuf->readindex;

  if (w > r) {
    avail = (r - w + rbuf->size);
    avail = (rbuf->size_mask ? avail & rbuf->size_mask : avail % rbuf->size);
    avail -= 1;
  } else if (w < r) {
    avail = r - w - 1;
  } else {
    avail = rbuf->size - 1;
  }
  end = w + avail;
  if (end > rbuf->size) {
    areas[0].data = &rbuf->data[w];
    areas[0].len = rbuf->size - w;
    areas[1].data = rbuf->data;
    areas[1].len = end - rbuf->size;
  } else {
    areas[0].data = &rbuf->data[w];
    areas[0].len = avail;
    areas[1].len = 0;
  }
  return SPA_RESULT_OK;
}

/**
 * spa_ringbuffer_write_advance:
 * @rbuf: a #SpaRingbuffer
 * @len: number of bytes to advance
 *
 * Advance the write pointer by @len
 *
 * Returns: #SPA_RESULT_OK
 */
SpaResult
spa_ringbuffer_write_advance (SpaRingbuffer      *rbuf,
                              ssize_t             len)
{
  size_t tmp = rbuf->writeindex + len;
  rbuf->writeindex = (rbuf->size_mask ? tmp & rbuf->size_mask : tmp % rbuf->size);
  return SPA_RESULT_OK;
}
