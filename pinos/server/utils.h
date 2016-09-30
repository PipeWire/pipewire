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

#ifndef __PINOS_UTILS_H__
#define __PINOS_UTILS_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _PinosMemblock PinosMemblock;

typedef enum {
  PINOS_MEMBLOCK_FLAG_NONE           = 0,
  PINOS_MEMBLOCK_FLAG_WITH_FD        = (1 << 0),
  PINOS_MEMBLOCK_FLAG_MAP_READ       = (1 << 1),
  PINOS_MEMBLOCK_FLAG_MAP_WRITE      = (1 << 2),
  PINOS_MEMBLOCK_FLAG_SEAL           = (1 << 3),
} PinosMemblockFlags;

#define PINOS_MEMBLOCK_FLAG_MAP_READWRITE (PINOS_MEMBLOCK_FLAG_MAP_READ | PINOS_MEMBLOCK_FLAG_MAP_WRITE)

struct _PinosMemblock {
  PinosMemblockFlags flags;
  int                fd;
  gpointer          *ptr;
  gsize              size;
};

gboolean      pinos_memblock_alloc     (PinosMemblockFlags  flags,
                                        gsize               size,
                                        PinosMemblock      *mem);
void          pinos_memblock_free      (PinosMemblock      *mem);

G_END_DECLS

#endif /* __PINOS_UTILS_H__ */
