/* PipeWire
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

#ifndef __PIPEWIRE_MEM_H__
#define __PIPEWIRE_MEM_H__

#include <spa/defs.h>

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
pw_memblock_alloc(enum pw_memblock_flags flags, size_t size, struct pw_memblock *mem);

int
pw_memblock_map(struct pw_memblock *mem);

void
pw_memblock_free(struct pw_memblock *mem);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_MEM_H__ */
