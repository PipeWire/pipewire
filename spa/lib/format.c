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

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <spa/format.h>
#include <spa/debug.h>


SpaResult
spa_format_fixate (SpaFormat *format)
{
  if (format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  format->props.unset_mask = 0;

  return SPA_RESULT_OK;
}

size_t
spa_format_get_size (const SpaFormat *format)
{
  if (format == NULL)
    return 0;

  return spa_props_get_size (&format->props) - sizeof (SpaProps) + sizeof (SpaFormat);
}

size_t
spa_format_serialize (void *dest, const SpaFormat *format)
{
  SpaFormat *tf;
  size_t size;

  if (format == NULL)
    return 0;

  tf = dest;
  tf->media_type = format->media_type;
  tf->media_subtype = format->media_subtype;

  dest = SPA_MEMBER (tf, offsetof (SpaFormat, props), void);
  size = spa_props_serialize (dest, &format->props) - sizeof (SpaProps) + sizeof (SpaFormat);

  return size;
}

SpaFormat *
spa_format_deserialize (void *src, off_t offset)
{
  SpaFormat *f;

  f = SPA_MEMBER (src, offset, SpaFormat);
  spa_props_deserialize (f, offsetof (SpaFormat, props));

  return f;
}

SpaFormat *
spa_format_copy_into (void *dest, const SpaFormat *format)
{
  if (format == NULL)
    return NULL;

  spa_format_serialize (dest, format);
  return spa_format_deserialize (dest, 0);
}
