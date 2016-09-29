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
  void *p;
  size_t size;

  if (format == NULL)
    return NULL;

  size = spa_format_get_size (format);
  p = malloc (size);
  return spa_format_copy_into (p, format);
}

static void
format_free (SpaFormat *format)
{
  g_free (format);
}

G_DEFINE_BOXED_TYPE (SpaFormat, spa_format,
        format_copy, format_free);
