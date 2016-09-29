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
  SpaResult res;

  if (src == dest)
    return SPA_RESULT_OK;

  for (i = 0; i < dest->n_prop_info; i++) {
    const SpaPropInfo *info = &dest->prop_info[i];
    SpaPropValue value;

    if (!(info->flags & SPA_PROP_FLAG_WRITABLE))
      continue;
    if ((res = spa_props_get_value (src, spa_props_index_for_id (src, info->id), &value)) < 0)
      continue;
    if (value.size > info->maxsize)
      return SPA_RESULT_WRONG_PROPERTY_SIZE;

    memcpy (SPA_MEMBER (dest, info->offset, void), value.value, value.size);

    SPA_PROPS_INDEX_SET (dest, i);
  }
  return SPA_RESULT_OK;
}

size_t
spa_props_get_size (const SpaProps *props)
{
  size_t len;
  unsigned int i, j;
  SpaPropInfo *pi;
  SpaPropRangeInfo *ri;

  if (props == NULL)
    return 0;

  len = sizeof (SpaProps);
  for (i = 0; i < props->n_prop_info; i++) {
    pi = (SpaPropInfo *) &props->prop_info[i];
    len += sizeof (SpaPropInfo);
    len += pi->name ? strlen (pi->name) + 1 : 0;
    len += pi->description ? strlen (pi->description) + 1 : 0;
    /* for the value */
    len += pi->maxsize;
    for (j = 0; j < pi->n_range_values; j++) {
      ri = (SpaPropRangeInfo *)&pi->range_values[j];
      len += sizeof (SpaPropRangeInfo);
      len += ri->name ? strlen (ri->name) + 1 : 0;
      len += ri->description ? strlen (ri->description) + 1 : 0;
      /* the size of the range value */
      len += ri->val.size;
    }
  }
  return len;
}

size_t
spa_props_serialize (void *p, const SpaProps *props)
{
  size_t len, slen;
  unsigned int i, j, c;
  SpaProps *tp;
  SpaPropInfo *pi;
  SpaPropRangeInfo *ri;

  if (props == NULL)
    return 0;

  tp = p;
  memcpy (tp, props, sizeof (SpaProps));
  pi = SPA_MEMBER (tp, sizeof(SpaProps), SpaPropInfo);
  ri = SPA_MEMBER (pi, sizeof(SpaPropInfo) * tp->n_prop_info, SpaPropRangeInfo);

  tp->prop_info = SPA_INT_TO_PTR (SPA_PTRDIFF (pi, tp));

  /* write propinfo array */
  for (i = 0, c = 0; i < tp->n_prop_info; i++) {
    memcpy (&pi[i], &props->prop_info[i], sizeof (SpaPropInfo));
    pi[i].range_values = SPA_INT_TO_PTR (SPA_PTRDIFF (&ri[c], tp));
    for (j = 0; j < pi[i].n_range_values; j++, c++) {
      memcpy (&ri[c], &props->prop_info[i].range_values[j], sizeof (SpaPropRangeInfo));
    }
  }
  p = &ri[c];
  /* strings and default values from props and ranges */
  for (i = 0, c = 0; i < tp->n_prop_info; i++) {
    if (pi[i].name) {
      slen = strlen (pi[i].name) + 1;
      memcpy (p, pi[i].name, slen);
      pi[i].name = SPA_INT_TO_PTR (SPA_PTRDIFF (p, tp));
      p += slen;
    } else {
      pi[i].name = 0;
    }
    if (pi[i].description) {
      slen = strlen (pi[i].description) + 1;
      memcpy (p, pi[i].description, slen);
      pi[i].description = SPA_INT_TO_PTR (SPA_PTRDIFF (p, tp));
      p += slen;
    } else {
      pi[i].description = 0;
    }
    for (j = 0; j < pi[i].n_range_values; j++, c++) {
      if (ri[c].name) {
        slen = strlen (ri[c].name) + 1;
        memcpy (p, ri[c].name, slen);
        ri[c].name = SPA_INT_TO_PTR (SPA_PTRDIFF (p, tp));
        p += slen;
      } else {
        ri[c].name = 0;
      }
      if (ri[c].description) {
        slen = strlen (ri[c].description) + 1;
        memcpy (p, ri[c].description, slen);
        ri[c].description = SPA_INT_TO_PTR (SPA_PTRDIFF (p, tp));
        p += slen;
      } else {
        ri[c].description = 0;
      }
      if (ri[c].val.size) {
        memcpy (p, ri[c].val.value, ri[c].val.size);
        ri[c].val.value = SPA_INT_TO_PTR (SPA_PTRDIFF (p, tp));
        p += ri[c].val.size;
      } else {
        ri[c].val.value = 0;
      }
    }
  }
  /* and the actual values */
  for (i = 0; i < tp->n_prop_info; i++) {
    if (pi[i].offset) {
      memcpy (p, SPA_MEMBER (props, pi[i].offset, void), pi[i].maxsize);
      pi[i].offset = SPA_PTRDIFF (p, tp);
      p += pi[i].maxsize;
    } else {
      pi[i].offset = 0;
    }
  }
  len = SPA_PTRDIFF (p, tp);

  return len;
}

SpaProps *
spa_props_deserialize (void *p, off_t offset)
{
  SpaProps *tp;
  unsigned int i, j;
  SpaPropInfo *pi;
  SpaPropRangeInfo *ri;

  tp = SPA_MEMBER (p, offset, SpaProps);
  if (tp->prop_info)
    tp->prop_info = SPA_MEMBER (tp, SPA_PTR_TO_INT (tp->prop_info), SpaPropInfo);
  /* now fix all the pointers */
  for (i = 0; i < tp->n_prop_info; i++) {
    pi = (SpaPropInfo *) &tp->prop_info[i];
    if (pi->name)
      pi->name = SPA_MEMBER (tp, SPA_PTR_TO_INT (pi->name), char);
    if (pi->description)
      pi->description = SPA_MEMBER (tp, SPA_PTR_TO_INT (pi->description), char);
    if (pi->range_values)
      pi->range_values = SPA_MEMBER (tp, SPA_PTR_TO_INT (pi->range_values), SpaPropRangeInfo);

    for (j = 0; j < pi->n_range_values; j++) {
      ri = (SpaPropRangeInfo *) &pi->range_values[j];
      if (ri->name)
        ri->name = SPA_MEMBER (tp, SPA_PTR_TO_INT (ri->name), char);
      if (ri->description)
        ri->description = SPA_MEMBER (tp, SPA_PTR_TO_INT (ri->description), char);
      if (ri->val.value)
        ri->val.value = SPA_MEMBER (tp, SPA_PTR_TO_INT (ri->val.value), void);
    }
  }
  return tp;
}

SpaProps *
spa_props_copy_into (void *dest, const SpaProps *props)
{
  if (props == NULL)
    return NULL;

  spa_props_serialize (dest, props);
  return spa_props_deserialize (dest, 0);
}
