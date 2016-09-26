/* Pinos
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#include <pinos/client/pinos.h>
#include <pinos/client/format.h>

static SpaFormat *
format_copy (SpaFormat *format)
{
  SpaMemory *mem;
  SpaFormat *f;

  if (format == NULL)
    return NULL;

  mem = spa_memory_alloc_size (format->mem.mem.pool_id, format, format->mem.size);
  f = spa_memory_ensure_ptr (mem);
  f->mem.mem = mem->mem;
  f->mem.offset = 0;
  f->mem.size = format->mem.size;

  return spa_memory_ensure_ptr (mem);
}

static void
format_free (SpaFormat *format)
{
  g_return_if_fail (format != NULL);

  spa_memory_unref (&format->mem.mem);
}

G_DEFINE_BOXED_TYPE (SpaFormat, spa_format,
        format_copy, format_free);
