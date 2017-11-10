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

struct spa_ringbuffer;
#define SPA_TYPE__RingBuffer		SPA_TYPE_INTERFACE_BASE "RingBuffer"
#define SPA_TYPE_RINGBUFFER_BASE	SPA_TYPE__RingBuffer ":"

#include <string.h>

#include <spa/utils/defs.h>

/**
 * spa_ringbuffer:
 * @readindex: the current read index
 * @writeindex: the current write index
 * @size: the size of the ringbuffer
 * @mask: mask as @size - 1
 */
struct spa_ringbuffer {
	uint32_t readindex;
	uint32_t writeindex;
	uint32_t size;
	uint32_t mask;
};

#define SPA_RINGBUFFER_INIT(size)	(struct spa_ringbuffer) { 0, 0, (size), (size)-1 }

/**
 * spa_ringbuffer_init:
 * @rbuf: a #struct spa_ringbuffer
 * @size: the number of elements in the ringbuffer
 *
 * Initialize a #struct spa_ringbuffer with @size.
 */
static inline void spa_ringbuffer_init(struct spa_ringbuffer *rbuf, uint32_t size)
{
	*rbuf = SPA_RINGBUFFER_INIT(size);
}

/**
 * spa_ringbuffer_clear:
 * @rbuf: a #struct spa_ringbuffer
 *
 * Clear @rbuf
 */
static inline void spa_ringbuffer_clear(struct spa_ringbuffer *rbuf)
{
	rbuf->readindex = 0;
	rbuf->writeindex = 0;
}

/**
 * spa_ringbuffer_get_read_index:
 * @rbuf: a #struct spa_ringbuffer
 * @index: the value of readindex, should be masked to get the
 *         offset in the ringbuffer memory
 *
 * Returns: number of available bytes to read. values < 0 mean
 *          there was an underrun. values > rbuf->size means there
 *          was an overrun.
 */
static inline int32_t spa_ringbuffer_get_read_index(struct spa_ringbuffer *rbuf, uint32_t *index)
{
	int32_t avail;

	*index = __atomic_load_n(&rbuf->readindex, __ATOMIC_RELAXED);
	avail = (int32_t) (__atomic_load_n(&rbuf->writeindex, __ATOMIC_ACQUIRE) - *index);

	return avail;
}

/**
 * spa_ringbuffer_read_data:
 * @rbuf: a #struct spa_ringbuffer
 * @buffer: memory to read from
 * @offset: offset in @buffer to read from
 * @data: destination memory
 * @len: number of bytes to read
 *
 * Read @len bytes from @rbuf starting @offset. @offset must be masked
 * with the size of @rbuf and len should be smaller than the size.
 */
static inline void
spa_ringbuffer_read_data(struct spa_ringbuffer *rbuf,
			 void *buffer, uint32_t offset, void *data, uint32_t len)
{
	uint32_t first = SPA_MIN(len, rbuf->size - offset);
	memcpy(data, buffer + offset, first);
	if (SPA_UNLIKELY(len > first)) {
		memcpy(data + first, buffer, len - first);
	}
}

/**
 * spa_ringbuffer_read_update:
 * @rbuf: a #struct spa_ringbuffer
 * @index: new index
 *
 * Update the read pointer to @index
 */
static inline void spa_ringbuffer_read_update(struct spa_ringbuffer *rbuf, int32_t index)
{
	__atomic_store_n(&rbuf->readindex, index, __ATOMIC_RELEASE);
}

/**
 * spa_ringbuffer_get_write_index:
 * @rbuf: a #struct spa_ringbuffer
 * @index: the value of writeindex, should be masked to get the
 *         offset in the ringbuffer memory
 *
 * Returns: the fill level of @rbuf. values < 0 mean
 *          there was an underrun. values > rbuf->size means there
 *          was an overrun. Subtract from the buffer size to get
 *          the number of bytes available for writing.
 */
static inline int32_t spa_ringbuffer_get_write_index(struct spa_ringbuffer *rbuf, uint32_t *index)
{
	int32_t filled;

	*index = __atomic_load_n(&rbuf->writeindex, __ATOMIC_RELAXED);
	filled = (int32_t) (*index - __atomic_load_n(&rbuf->readindex, __ATOMIC_ACQUIRE));

	return filled;
}

static inline void
spa_ringbuffer_write_data(struct spa_ringbuffer *rbuf,
			  void *buffer, uint32_t offset, void *data, uint32_t len)
{
	uint32_t first = SPA_MIN(len, rbuf->size - offset);
	memcpy(buffer + offset, data, first);
	if (SPA_UNLIKELY(len > first)) {
		memcpy(buffer, data + first, len - first);
	}
}

/**
 * spa_ringbuffer_write_update:
 * @rbuf: a #struct spa_ringbuffer
 * @index: new index
 *
 * Update the write pointer to @index
 *
 */
static inline void spa_ringbuffer_write_update(struct spa_ringbuffer *rbuf, int32_t index)
{
	__atomic_store_n(&rbuf->writeindex, index, __ATOMIC_RELEASE);
}


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_RINGBUFFER_H__ */
