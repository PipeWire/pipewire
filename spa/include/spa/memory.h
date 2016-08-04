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

#ifndef __SPA_MEMORY_H__
#define __SPA_MEMORY_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaMemory SpaMemory;
typedef struct _SpaMemoryRef SpaMemoryRef;
typedef struct _SpaMemoryPool SpaMemoryPool;

#include <spa/defs.h>

/**
 * SpaMemoryFlags:
 * @SPA_MEMORY_FLAG_NONE: no flag
 * @SPA_MEMORY_FLAG_READABLE: memory is writable
 * @SPA_MEMORY_FLAG_WRITABLE: memory is readable
 */
typedef enum {
  SPA_MEMORY_FLAG_NONE               =  0,
  SPA_MEMORY_FLAG_READABLE           = (1 << 0),
  SPA_MEMORY_FLAG_WRITABLE           = (1 << 1),
} SpaMemoryFlags;

#define SPA_MEMORY_FLAG_READWRITE   (SPA_MEMORY_FLAG_READABLE|SPA_MEMORY_FLAG_WRITABLE)

/**
 * SpaMemoryRef:
 * @pool_id: the pool id
 * @id: mem_id
 */
struct _SpaMemoryRef {
  uint32_t  pool_id;
  uint32_t  id;
};

/**
 * SpaMemory:
 * @refcount: a refcount
 * @notify: notify when refcount is 0
 * @pool_id: the id of the pool
 * @id: the memory id
 * @flags: extra memory flags
 * @type: memory type
 * @fd: fd of memory of -1 if not known
 * @ptr: ptr to memory accessible with CPU
 * @size: size of memory
 */
struct _SpaMemory {
  int             refcount;
  SpaNotify       notify;
  SpaMemoryRef    mem;
  SpaMemoryFlags  flags;
  const char     *type;
  int             fd;
  void           *ptr;
  size_t          size;
};

void             spa_memory_init           (void);

uint32_t         spa_memory_pool_get       (uint32_t type);
uint32_t         spa_memory_pool_new       (void);
void             spa_memory_pool_free      (uint32_t);

SpaMemory *      spa_memory_alloc          (uint32_t pool_id);
SpaMemory *      spa_memory_alloc_with_fd  (uint32_t pool_id, void *data, size_t size);

SpaResult        spa_memory_free           (SpaMemoryRef *ref);
SpaMemory *      spa_memory_import         (SpaMemoryRef *ref);
SpaMemory *      spa_memory_find           (SpaMemoryRef *ref);

void *           spa_memory_ensure_ptr     (SpaMemory *mem);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_MEMORY_H__ */
