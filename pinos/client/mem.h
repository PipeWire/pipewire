/* Pinos
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

#ifndef __PINOS_MEM_H__
#define __PINOS_MEM_H__

#include <spa/defs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _PinosMemblock PinosMemblock;

typedef enum {
  PINOS_MEMBLOCK_FLAG_NONE           = 0,
  PINOS_MEMBLOCK_FLAG_WITH_FD        = (1 << 0),
  PINOS_MEMBLOCK_FLAG_SEAL           = (1 << 1),
  PINOS_MEMBLOCK_FLAG_MAP_READ       = (1 << 2),
  PINOS_MEMBLOCK_FLAG_MAP_WRITE      = (1 << 3),
  PINOS_MEMBLOCK_FLAG_MAP_TWICE      = (1 << 4),
} PinosMemblockFlags;

#define PINOS_MEMBLOCK_FLAG_MAP_READWRITE (PINOS_MEMBLOCK_FLAG_MAP_READ | PINOS_MEMBLOCK_FLAG_MAP_WRITE)

struct _PinosMemblock {
  PinosMemblockFlags flags;
  int                fd;
  off_t              offset;
  void              *ptr;
  size_t             size;
};

SpaResult     pinos_memblock_alloc     (PinosMemblockFlags  flags,
                                        size_t              size,
                                        PinosMemblock      *mem);
SpaResult     pinos_memblock_map       (PinosMemblock      *mem);
void          pinos_memblock_free      (PinosMemblock      *mem);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_MEM_H__ */
