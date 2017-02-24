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

#if 0

static int
compare_value (SpaPropType type, const SpaPropRangeInfo *r1, const SpaPropRangeInfo *r2)
{
  switch (type) {
    case SPA_PROP_TYPE_INVALID:
      return 0;
    case SPA_PROP_TYPE_BOOL:
      return *(bool*)r1->val.value == *(bool*)r2->val.value;
    case SPA_PROP_TYPE_INT8:
      return *(int8_t*)r1->val.value - *(int8_t*)r2->val.value;
    case SPA_PROP_TYPE_UINT8:
      return *(uint8_t*)r1->val.value - *(uint8_t*)r2->val.value;
    case SPA_PROP_TYPE_INT16:
      return *(int16_t*)r1->val.value - *(int16_t*)r2->val.value;
    case SPA_PROP_TYPE_UINT16:
      return *(uint16_t*)r1->val.value - *(uint16_t*)r2->val.value;
    case SPA_PROP_TYPE_INT32:
      return *(int32_t*)r1->val.value - *(int32_t*)r2->val.value;
    case SPA_PROP_TYPE_UINT32:
      return *(uint32_t*)r1->val.value - *(uint32_t*)r2->val.value;
    case SPA_PROP_TYPE_INT64:
      return *(int64_t*)r1->val.value - *(int64_t*)r2->val.value;
    case SPA_PROP_TYPE_UINT64:
      return *(uint64_t*)r1->val.value - *(uint64_t*)r2->val.value;
    case SPA_PROP_TYPE_INT:
      return *(int*)r1->val.value - *(int*)r2->val.value;
    case SPA_PROP_TYPE_UINT:
      return *(unsigned int*)r1->val.value - *(unsigned int*)r2->val.value;
    case SPA_PROP_TYPE_FLOAT:
      return *(float*)r1->val.value - *(float*)r2->val.value;
    case SPA_PROP_TYPE_DOUBLE:
      return *(double*)r1->val.value - *(double*)r2->val.value;
    case SPA_PROP_TYPE_STRING:
      return strcmp (r1->val.value, r2->val.value);
    case SPA_PROP_TYPE_RECTANGLE:
    {
      const SpaRectangle *rec1 = r1->val.value, *rec2 = r2->val.value;
      if (rec1->width == rec2->width && rec1->height == rec2->height)
        return 0;
      else if (rec1->width < rec2->width || rec1->height < rec2->height)
        return -1;
      else
        return 1;
    }
    case SPA_PROP_TYPE_FRACTION:
      break;
    case SPA_PROP_TYPE_BITMASK:
    case SPA_PROP_TYPE_POINTER:
      break;
  }
  return 0;
}

SpaResult
spa_props_filter (SpaPropBuilder *b,
                  const SpaProps *props,
                  const SpaProps *filter)
{
  int i, j, k;

  if (filter == NULL)
    return SPA_RESULT_OK;

  for (i = 0; i < props->n_prop_info; i++) {
    unsigned int idx;
    const SpaPropInfo *i1, *i2;
    SpaPropBuilderInfo *bi;
    const SpaPropRangeInfo *ri1, *ri2;
    SpaPropRangeInfo sri1, sri2;
    unsigned int nri1, nri2;
    SpaPropRangeType rt1, rt2;
    SpaPropBuilderRange *br;

    i1 = &props->prop_info[i];

    idx = spa_props_index_for_id (filter, i1->id);

    /* always copy the property if it turns out incomplatible with the
     * filter later, we abort and return SPA_RESULT_NO_FORMAT */
    bi = alloca (sizeof (SpaPropBuilderInfo));
    memcpy (&bi->info, i1, sizeof (SpaPropInfo));
    spa_prop_builder_add_info (b, bi);
    bi->value = SPA_MEMBER (props, i1->offset, void);

    if (idx == SPA_IDX_INVALID)
      continue;

    i2 = &filter->prop_info[idx];

    if (i1->type != i2->type)
      return SPA_RESULT_NO_FORMAT;

    if (SPA_PROPS_INDEX_IS_UNSET (props, i)) {
      ri1 = i1->range_values;
      nri1 = i1->n_range_values;
      rt1 = i1->range_type;
    } else {
      sri1.name = "";
      sri1.val.size = i1->maxsize;
      sri1.val.value = SPA_MEMBER (props, i1->offset, void);
      ri1 = &sri1;
      nri1 = 1;
      rt1 = SPA_PROP_RANGE_TYPE_NONE;
    }
    if (SPA_PROPS_INDEX_IS_UNSET (filter, idx)) {
      ri2 = i2->range_values;
      nri2 = i2->n_range_values;
      rt2 = i2->range_type;
    } else {
      sri2.name = "";
      sri2.val.size = i2->maxsize;
      sri2.val.value = SPA_MEMBER (filter, i2->offset, void);
      ri2 = &sri2;
      nri2 = 1;
      rt2 = SPA_PROP_RANGE_TYPE_NONE;
    }

    if ((rt1 == SPA_PROP_RANGE_TYPE_NONE && rt2 == SPA_PROP_RANGE_TYPE_NONE) ||
        (rt1 == SPA_PROP_RANGE_TYPE_NONE && rt2 == SPA_PROP_RANGE_TYPE_ENUM) ||
        (rt1 == SPA_PROP_RANGE_TYPE_ENUM && rt2 == SPA_PROP_RANGE_TYPE_NONE) ||
        (rt1 == SPA_PROP_RANGE_TYPE_ENUM && rt2 == SPA_PROP_RANGE_TYPE_ENUM)) {
      int n_copied = 0;
      /* copy all equal values */
      for (j = 0; j < nri1; j++) {
        for (k = 0; k < nri2; k++) {
          if (compare_value (i1->type, &ri1[j], &ri2[k]) == 0) {
            br = alloca (sizeof (SpaPropBuilderRange));
            memcpy (&br->info, &ri1[j], sizeof (SpaPropRangeInfo));
            spa_prop_builder_add_range (b, br);
            n_copied++;
          }
        }
      }
      if (n_copied == 0)
        return SPA_RESULT_NO_FORMAT;
      b->info->info.n_range_values = n_copied;
      b->info->info.range_type = SPA_PROP_RANGE_TYPE_ENUM;
      continue;
    }

    if ((rt1 == SPA_PROP_RANGE_TYPE_NONE && rt2 == SPA_PROP_RANGE_TYPE_MIN_MAX) ||
        (rt1 == SPA_PROP_RANGE_TYPE_ENUM && rt2 == SPA_PROP_RANGE_TYPE_MIN_MAX)) {
      int n_copied = 0;
      /* copy all values inside the range */
      for (j = 0; j < nri1; j++) {
        if (compare_value (i1->type, &ri1[j], &ri2[0]) < 0)
          continue;
        if (compare_value (i1->type, &ri1[j], &ri2[1]) > 0)
          continue;
        br = alloca (sizeof (SpaPropBuilderRange));
        memcpy (&br->info, &ri1[j], sizeof (SpaPropRangeInfo));
        spa_prop_builder_add_range (b, br);
        n_copied++;
      }
      if (n_copied == 0)
        return SPA_RESULT_NO_FORMAT;
      b->info->info.n_range_values = n_copied;
      b->info->info.range_type = SPA_PROP_RANGE_TYPE_ENUM;
    }

    if (rt1 == SPA_PROP_RANGE_TYPE_NONE && rt2 == SPA_PROP_RANGE_TYPE_STEP)
      return SPA_RESULT_NOT_IMPLEMENTED;

    if (rt1 == SPA_PROP_RANGE_TYPE_NONE && rt2 == SPA_PROP_RANGE_TYPE_FLAGS)
      return SPA_RESULT_NOT_IMPLEMENTED;

    if ((rt1 == SPA_PROP_RANGE_TYPE_MIN_MAX && rt2 == SPA_PROP_RANGE_TYPE_NONE) ||
        (rt1 == SPA_PROP_RANGE_TYPE_MIN_MAX && rt2 == SPA_PROP_RANGE_TYPE_ENUM)) {
      int n_copied = 0;
      /* copy all values inside the range */
      for (k = 0; k < nri2; k++) {
        if (compare_value (i1->type, &ri2[k], &ri1[0]) < 0)
          continue;
        if (compare_value (i1->type, &ri2[k], &ri1[1]) > 0)
          continue;
        br = alloca (sizeof (SpaPropBuilderRange));
        memcpy (&br->info, &ri2[k], sizeof (SpaPropRangeInfo));
        spa_prop_builder_add_range (b, br);
        n_copied++;
      }
      if (n_copied == 0)
        return SPA_RESULT_NO_FORMAT;
      b->info->info.n_range_values = n_copied;
      b->info->info.range_type = SPA_PROP_RANGE_TYPE_ENUM;
    }

    if (rt1 == SPA_PROP_RANGE_TYPE_MIN_MAX && rt2 == SPA_PROP_RANGE_TYPE_MIN_MAX) {
      br = alloca (sizeof (SpaPropBuilderRange));
      if (compare_value (i1->type, &ri1[0], &ri2[0]) < 0)
        memcpy (&br->info, &ri2[0], sizeof (SpaPropRangeInfo));
      else
        memcpy (&br->info, &ri1[0], sizeof (SpaPropRangeInfo));
      spa_prop_builder_add_range (b, br);

      br = alloca (sizeof (SpaPropBuilderRange));
      if (compare_value (i1->type, &ri1[1], &ri2[1]) < 0)
        memcpy (&br->info, &ri1[1], sizeof (SpaPropRangeInfo));
      else
        memcpy (&br->info, &ri2[1], sizeof (SpaPropRangeInfo));
      spa_prop_builder_add_range (b, br);
    }
    if (rt1 == SPA_PROP_RANGE_TYPE_MIN_MAX && rt2 == SPA_PROP_RANGE_TYPE_STEP)
      return SPA_RESULT_NOT_IMPLEMENTED;

    if (rt1 == SPA_PROP_RANGE_TYPE_MIN_MAX && rt2 == SPA_PROP_RANGE_TYPE_FLAGS)
      return SPA_RESULT_NOT_IMPLEMENTED;

    if (rt1 == SPA_PROP_RANGE_TYPE_ENUM && rt2 == SPA_PROP_RANGE_TYPE_STEP)
      return SPA_RESULT_NOT_IMPLEMENTED;
    if (rt1 == SPA_PROP_RANGE_TYPE_ENUM && rt2 == SPA_PROP_RANGE_TYPE_FLAGS)
      return SPA_RESULT_NOT_IMPLEMENTED;

    if (rt1 == SPA_PROP_RANGE_TYPE_STEP && rt2 == SPA_PROP_RANGE_TYPE_NONE)
      return SPA_RESULT_NOT_IMPLEMENTED;
    if (rt1 == SPA_PROP_RANGE_TYPE_STEP && rt2 == SPA_PROP_RANGE_TYPE_MIN_MAX)
      return SPA_RESULT_NOT_IMPLEMENTED;

    if (rt1 == SPA_PROP_RANGE_TYPE_STEP && rt2 == SPA_PROP_RANGE_TYPE_STEP)
      return SPA_RESULT_NOT_IMPLEMENTED;
    if (rt1 == SPA_PROP_RANGE_TYPE_STEP && rt2 == SPA_PROP_RANGE_TYPE_ENUM)
      return SPA_RESULT_NOT_IMPLEMENTED;
    if (rt1 == SPA_PROP_RANGE_TYPE_STEP && rt2 == SPA_PROP_RANGE_TYPE_FLAGS)
      return SPA_RESULT_NOT_IMPLEMENTED;

    if (rt1 == SPA_PROP_RANGE_TYPE_FLAGS && rt2 == SPA_PROP_RANGE_TYPE_NONE)
      return SPA_RESULT_NOT_IMPLEMENTED;
    if (rt1 == SPA_PROP_RANGE_TYPE_FLAGS && rt2 == SPA_PROP_RANGE_TYPE_MIN_MAX)
      return SPA_RESULT_NOT_IMPLEMENTED;
    if (rt1 == SPA_PROP_RANGE_TYPE_FLAGS && rt2 == SPA_PROP_RANGE_TYPE_STEP)
      return SPA_RESULT_NOT_IMPLEMENTED;
    if (rt1 == SPA_PROP_RANGE_TYPE_FLAGS && rt2 == SPA_PROP_RANGE_TYPE_ENUM)
      return SPA_RESULT_NOT_IMPLEMENTED;
    if (rt1 == SPA_PROP_RANGE_TYPE_FLAGS && rt2 == SPA_PROP_RANGE_TYPE_FLAGS)
      return SPA_RESULT_NOT_IMPLEMENTED;
  }

  spa_prop_builder_finish (b);

  return SPA_RESULT_OK;
}
#endif
