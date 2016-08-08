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

#include <spa/props.h>

SpaResult
spa_props_set_prop (SpaProps           *props,
                    unsigned int        index,
                    const SpaPropValue *value)
{
  const SpaPropInfo *info;

  if (props == NULL || value == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;
  if (index >= props->n_prop_info)
    return SPA_RESULT_INVALID_PROPERTY_INDEX;

  info = &props->prop_info[index];
  if ((info->flags & SPA_PROP_FLAG_WRITABLE) == 0)
    return SPA_RESULT_INVALID_PROPERTY_ACCESS;
  if (info->type != value->type)
    return SPA_RESULT_WRONG_PROPERTY_TYPE;
  if (info->maxsize < value->size)
    return SPA_RESULT_WRONG_PROPERTY_SIZE;

  if (info->offset != 0)
    memcpy ((uint8_t*)props + info->offset, value->value, value->size);

  props->unset_mask &= ~(1 << index);

  return SPA_RESULT_OK;
}


SpaResult
spa_props_get_prop (const SpaProps *props,
                    unsigned int    index,
                    SpaPropValue   *value)
{
  const SpaPropInfo *info;

  if (props == NULL || value == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;
  if (index >= props->n_prop_info)
    return SPA_RESULT_INVALID_PROPERTY_INDEX;

  info = &props->prop_info[index];
  if ((info->flags & SPA_PROP_FLAG_READABLE) == 0)
    return SPA_RESULT_INVALID_PROPERTY_ACCESS;

  if (props->unset_mask & (1 << index))
    return SPA_RESULT_PROPERTY_UNSET;

  value->type = info->type;
  value->size = info->maxsize;
  if (info->offset != 0)
    value->value = (uint8_t*)props + info->offset;

  return SPA_RESULT_OK;
}

SpaResult
spa_props_copy (const SpaProps *src,
                SpaProps       *dest)
{
  int i;
  SpaResult res;

  if (src == dest)
    return SPA_RESULT_OK;

  for (i = 0; i < dest->n_prop_info; i++) {
    const SpaPropInfo *info = &dest->prop_info[i];
    SpaPropValue value;

    if (!(info->flags & SPA_PROP_FLAG_WRITABLE))
      continue;
    if ((res = spa_props_get_prop (src, spa_props_index_for_id (src, info->id), &value)) < 0)
      continue;
    if (value.type != info->type)
      return SPA_RESULT_WRONG_PROPERTY_TYPE;
    if (value.size > info->maxsize)
      return SPA_RESULT_WRONG_PROPERTY_SIZE;

    if (info->offset)
      memcpy ((uint8_t*)dest + info->offset, value.value, value.size);

    dest->unset_mask &= ~(1 << i);
  }
  return SPA_RESULT_OK;
}
