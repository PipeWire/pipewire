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
  uint32_t        pool_id;
  uint32_t        id;
  SpaMemoryFlags  flags;
  const char     *type;
  int             fd;
  void           *ptr;
  size_t          size;
};

uint32_t         spa_memory_pool_get       (uint32_t type);
uint32_t         spa_memory_pool_new       (void);
void             spa_memory_pool_free      (uint32_t);

SpaMemory *      spa_memory_alloc          (uint32_t pool_id);
SpaResult        spa_memory_free           (uint32_t pool_id, uint32_t id);

SpaMemory *      spa_memory_find           (uint32_t pool_id, uint32_t id);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_MEMORY_H__ */
