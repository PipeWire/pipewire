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
 * A ringbuffer type.
 */
struct spa_ringbuffer {
	uint32_t readindex;	/*< the current read index */
	uint32_t writeindex;	/*< the current write index */
};

#define SPA_RINGBUFFER_INIT()	(struct spa_ringbuffer) { 0, 0 }

/**
 * Initialize a spa_ringbuffer with \a size.
 *
 * \param rbuf a spa_ringbuffer
 * \param size the number of elements in the ringbuffer
 */
static inline void spa_ringbuffer_init(struct spa_ringbuffer *rbuf)
{
	*rbuf = SPA_RINGBUFFER_INIT();
}

/**
 * Sets the pointers so that the ringbuffer contains \a size bytes.
 *
 * \param rbuf a spa_ringbuffer
 */
static inline void spa_ringbuffer_set_avail(struct spa_ringbuffer *rbuf, uint32_t size)
{
	rbuf->readindex = 0;
	rbuf->writeindex = size;
}

/**
 * Get the read index and available bytes for reading.
 *
 * \param rbuf a  spa_ringbuffer
 * \param index the value of readindex, should be taken modulo the size of the
 *         ringbuffer memory to get the offset in the ringbuffer memory
 * \return number of available bytes to read. values < 0 mean
 *         there was an underrun. values > rbuf->size means there
 *         was an overrun.
 */
static inline int32_t spa_ringbuffer_get_read_index(struct spa_ringbuffer *rbuf, uint32_t *index)
{
	*index = __atomic_load_n(&rbuf->readindex, __ATOMIC_RELAXED);
	return (int32_t) (__atomic_load_n(&rbuf->writeindex, __ATOMIC_ACQUIRE) - *index);
}

/**
 * Read \a len bytes from \a rbuf starting \a offset. \a offset must be taken
 * modulo \a size and len should be smaller than \a size.
 *
 * \param rbuf a #struct spa_ringbuffer
 * \param buffer memory to read from
 * \param size the size of \a buffer
 * \param offset offset in \a buffer to read from
 * \param data destination memory
 * \param len number of bytes to read
 */
static inline void
spa_ringbuffer_read_data(struct spa_ringbuffer *rbuf,
			 const void *buffer, uint32_t size,
			 uint32_t offset, void *data, uint32_t len)
{
	uint32_t l0 = SPA_MIN(len, size - offset), l1 = len - l0;
	memcpy(data, SPA_MEMBER(buffer, offset, void), l0);
	if (SPA_UNLIKELY(l1 > 0))
		memcpy(SPA_MEMBER(data, l0, void), buffer, l1);
}

/**
 * Update the read pointer to \a index.
 *
 * \param rbuf a spa_ringbuffer
 * \param index new index
 */
static inline void spa_ringbuffer_read_update(struct spa_ringbuffer *rbuf, int32_t index)
{
	__atomic_store_n(&rbuf->readindex, index, __ATOMIC_RELEASE);
}

/**
 * Get the write index and the number of bytes inside the ringbuffer.
 *
 * \param rbuf a  spa_ringbuffer
 * \param index the value of writeindex, should be taken modulo the size of the
 *         ringbuffer memory to get the offset in the ringbuffer memory
 * \return the fill level of \a rbuf. values < 0 mean
 *         there was an underrun. values > rbuf->size means there
 *         was an overrun. Subtract from the buffer size to get
 *         the number of bytes available for writing.
 */
static inline int32_t spa_ringbuffer_get_write_index(struct spa_ringbuffer *rbuf, uint32_t *index)
{
	*index = __atomic_load_n(&rbuf->writeindex, __ATOMIC_RELAXED);
	return (int32_t) (*index - __atomic_load_n(&rbuf->readindex, __ATOMIC_ACQUIRE));
}

/**
 * Write \a len bytes to \a buffer starting \a offset. \a offset must be taken
 * modulo \a size and len should be smaller than \a size.
 *
 * \param rbuf a spa_ringbuffer
 * \param buffer memory to write to
 * \param size the size of \a buffer
 * \param offset offset in \a buffer to write to
 * \param data source memory
 * \param len number of bytes to write
 */
static inline void
spa_ringbuffer_write_data(struct spa_ringbuffer *rbuf,
			  void *buffer, uint32_t size,
			  uint32_t offset, const void *data, uint32_t len)
{
	uint32_t l0 = SPA_MIN(len, size - offset), l1 = len - l0;
	memcpy(SPA_MEMBER(buffer, offset, void), data, l0);
	if (SPA_UNLIKELY(l1 > 0))
		memcpy(buffer, SPA_MEMBER(data, l0, void), l1);
}

/**
 * Update the write pointer to \a index
 *
 * \param rbuf a spa_ringbuffer
 * \param index new index
 */
static inline void spa_ringbuffer_write_update(struct spa_ringbuffer *rbuf, int32_t index)
{
	__atomic_store_n(&rbuf->writeindex, index, __ATOMIC_RELEASE);
}


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_RINGBUFFER_H__ */
