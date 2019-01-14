/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef PIPEWIRE_MEM_H
#define PIPEWIRE_MEM_H

#include <spa/utils/defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Flags passed to \ref pw_memblock_alloc() \memberof pw_memblock */
enum pw_memblock_flags {
	PW_MEMBLOCK_FLAG_NONE = 0,
	PW_MEMBLOCK_FLAG_WITH_FD = (1 << 0),
	PW_MEMBLOCK_FLAG_SEAL = (1 << 1),
	PW_MEMBLOCK_FLAG_MAP_READ = (1 << 2),
	PW_MEMBLOCK_FLAG_MAP_WRITE = (1 << 3),
	PW_MEMBLOCK_FLAG_MAP_TWICE = (1 << 4),
};

#define PW_MEMBLOCK_FLAG_MAP_READWRITE (PW_MEMBLOCK_FLAG_MAP_READ | PW_MEMBLOCK_FLAG_MAP_WRITE)

/** \class pw_memblock
 * Memory block structure */
struct pw_memblock {
	enum pw_memblock_flags flags;	/**< flags used when allocating */
	int fd;				/**< memfd if any */
	off_t offset;			/**< offset of mappable memory */
	void *ptr;			/**< ptr to mapped memory */
	size_t size;			/**< size of mapped memory */
};

int
pw_memblock_alloc(enum pw_memblock_flags flags, size_t size, struct pw_memblock **mem);

int
pw_memblock_import(enum pw_memblock_flags flags,
		   int fd, off_t offset, size_t size,
		   struct pw_memblock **mem);

int
pw_memblock_map(struct pw_memblock *mem);

void
pw_memblock_free(struct pw_memblock *mem);

/** Find memblock for given \a ptr */
struct pw_memblock * pw_memblock_find(const void *ptr);

/** parameters to map a memory range */
struct pw_map_range {
	uint32_t start;		/** offset in first page with start of data */
	uint32_t offset;	/** page aligned offset to map */
	uint32_t size;		/** size to map */
};

#define PW_MAP_RANGE_INIT (struct pw_map_range){ 0, }

/** Calculate parameters to mmap() memory into \a range so that
 * \a size bytes at \a offset can be mapped with mmap().  */
static inline void pw_map_range_init(struct pw_map_range *range,
				     uint32_t offset, uint32_t size,
				     uint32_t page_size)
{
	range->offset = SPA_ROUND_DOWN_N(offset, page_size);
	range->start = offset - range->offset;
	range->size = offset + size - range->offset;
}


#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_MEM_H */
