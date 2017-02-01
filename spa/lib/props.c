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
spa_props_set_value (SpaProps           *props,
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
  if (info->maxsize < value->size)
    return SPA_RESULT_WRONG_PROPERTY_SIZE;

  memcpy (SPA_MEMBER (props, info->offset, void), value->value, value->size);

  SPA_PROPS_INDEX_SET (props, index);

  return SPA_RESULT_OK;
}


SpaResult
spa_props_get_value (const SpaProps *props,
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

  if (SPA_PROPS_INDEX_IS_UNSET (props, index))
    return SPA_RESULT_PROPERTY_UNSET;

  value->size = info->maxsize;
  value->value = SPA_MEMBER (props, info->offset, void);

  return SPA_RESULT_OK;
}

SpaResult
spa_props_copy_values (const SpaProps *src,
                       SpaProps       *dest)
{
  int i;

  if (src == dest)
    return SPA_RESULT_OK;

  for (i = 0; i < dest->n_prop_info; i++) {
    const SpaPropInfo *dinfo = &dest->prop_info[i];
    const SpaPropInfo *sinfo;
    unsigned int idx;

    if (!(dinfo->flags & SPA_PROP_FLAG_WRITABLE))
      continue;
    if ((idx = spa_props_index_for_id (src, dinfo->id)) == SPA_IDX_INVALID)
      continue;
    sinfo = &src->prop_info[idx];
    if (sinfo->maxsize > dinfo->maxsize)
      return SPA_RESULT_WRONG_PROPERTY_SIZE;

    memcpy (SPA_MEMBER (dest, dinfo->offset, void), SPA_MEMBER (src, sinfo->offset, void), sinfo->maxsize);

    if (!SPA_PROPS_INDEX_IS_UNSET (src, idx))
      SPA_PROPS_INDEX_SET (dest, i);
  }
  return SPA_RESULT_OK;
}
