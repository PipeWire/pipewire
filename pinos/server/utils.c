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

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "memfd-wrappers.h"

#include <pinos/server/utils.h>

gboolean
pinos_memblock_alloc (PinosMemblockFlags  flags,
                      gsize               size,
                      PinosMemblock      *mem)
{
  g_return_val_if_fail (mem != NULL, FALSE);
  g_return_val_if_fail (size > 0, FALSE);

  mem->flags = flags;
  mem->size = size;

  if (flags & PINOS_MEMBLOCK_FLAG_WITH_FD) {
    mem->fd = memfd_create ("pinos-memfd", MFD_CLOEXEC | MFD_ALLOW_SEALING);

    if (ftruncate (mem->fd, size) < 0) {
      g_warning ("Failed to truncate temporary file: %s", strerror (errno));
      close (mem->fd);
      return FALSE;
    }
    if (flags & PINOS_MEMBLOCK_FLAG_SEAL) {
      unsigned int seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
      if (fcntl (mem->fd, F_ADD_SEALS, seals) == -1) {
        g_warning ("Failed to add seals: %s", strerror (errno));
      }
    }
    if (flags & PINOS_MEMBLOCK_FLAG_MAP_READWRITE) {
      int prot = 0;

      if (flags & PINOS_MEMBLOCK_FLAG_MAP_READ)
        prot |= PROT_READ;
      if (flags & PINOS_MEMBLOCK_FLAG_MAP_WRITE)
        prot |= PROT_WRITE;

      mem->ptr = mmap (NULL, size, prot, MAP_SHARED, mem->fd, 0);
    } else {
      mem->ptr = NULL;
    }
  } else {
    mem->ptr = g_malloc (size);
    mem->fd = -1;
  }
  return TRUE;
}

void
pinos_memblock_free  (PinosMemblock *mem)
{
  g_return_if_fail (mem != NULL);

  if (mem->flags & PINOS_MEMBLOCK_FLAG_WITH_FD) {
    if (mem->ptr)
      munmap (mem->ptr, mem->size);
    if (mem->fd != -1)
      close (mem->fd);
  } else {
    g_free (mem->ptr);
  }
  mem->ptr = NULL;
  mem->fd = -1;
}
