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

#if 0
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
#endif

static int
compare_value (SpaPODType type, const void *r1, const void *r2)
{
  switch (type) {
    case SPA_POD_TYPE_INVALID:
      return 0;
    case SPA_POD_TYPE_BOOL:
      return *(int32_t*)r1 == *(uint32_t*)r2;
    case SPA_POD_TYPE_INT:
      printf ("%d <> %d\n", *(int32_t*)r1, *(int32_t*)r2);
      return *(int32_t*)r1 - *(int32_t*)r2;
    case SPA_POD_TYPE_LONG:
      return *(int64_t*)r1 - *(int64_t*)r2;
    case SPA_POD_TYPE_FLOAT:
      return *(float*)r1 - *(float*)r2;
    case SPA_POD_TYPE_DOUBLE:
      return *(double*)r1 - *(double*)r2;
    case SPA_POD_TYPE_STRING:
      return strcmp (r1, r2);
    case SPA_POD_TYPE_RECTANGLE:
    {
      const SpaRectangle *rec1 = (SpaRectangle*)r1,
                         *rec2 = (SpaRectangle*)r2;
      if (rec1->width == rec2->width && rec1->height == rec2->height)
        return 0;
      else if (rec1->width < rec2->width || rec1->height < rec2->height)
        return -1;
      else
        return 1;
    }
    case SPA_POD_TYPE_FRACTION:
      break;
    default:
      break;
  }
  return 0;
}

SpaResult
spa_props_filter (SpaPODBuilder  *b,
                  const SpaPOD   *props,
                  uint32_t        props_size,
                  const SpaPOD   *filter,
                  uint32_t        filter_size)
{
  int j, k;
  const SpaPOD *pr;

  SPA_POD_FOREACH (props, props_size, pr) {
    SpaPODFrame f;
    SpaPODProp *p1, *p2, *np;
    int nalt1, nalt2;
    void *alt1, *alt2, *a1, *a2;
    uint32_t rt1, rt2;

    if (pr->type != SPA_POD_TYPE_PROP)
      continue;

    p1 = (SpaPODProp *) pr;

    if (filter == NULL || (p2 = spa_pod_contents_find_prop (filter, 0, p1->body.key)) == NULL) {
      /* no filter, copy the complete property */
      spa_pod_builder_raw (b, p1, SPA_POD_SIZE (p1), true);
      continue;
    }

    /* incompatible formats */
    if (p1->body.value.type != p2->body.value.type)
      return SPA_RESULT_NO_FORMAT;

    rt1 = p1->body.flags & SPA_POD_PROP_RANGE_MASK;
    rt2 = p2->body.flags & SPA_POD_PROP_RANGE_MASK;

    /* else we filter. start with copying the property */
    np = SPA_POD_BUILDER_DEREF (b,
                                spa_pod_builder_push_prop (b, &f, p1->body.key, SPA_POD_PROP_FLAG_READWRITE),
                                SpaPODProp);
    /* size and type */
    spa_pod_builder_raw (b, &p1->body.value, sizeof (p1->body.value), false);

    alt1 = SPA_MEMBER (p1, sizeof (SpaPODProp), void);
    nalt1 = SPA_POD_PROP_N_VALUES (p1);
    alt2 = SPA_MEMBER (p2, sizeof (SpaPODProp), void);
    nalt2 = SPA_POD_PROP_N_VALUES (p2);

    if (p1->body.flags & SPA_POD_PROP_FLAG_UNSET) {
      alt1 = SPA_MEMBER (alt1, p1->body.value.size, void);
      nalt1--;
    } else {
      nalt1 = 1;
    }

    if (p2->body.flags & SPA_POD_PROP_FLAG_UNSET) {
      alt2 = SPA_MEMBER (alt2, p2->body.value.size, void);
      nalt2--;
    } else {
      nalt2 = 1;
    }

    if ((rt1 == SPA_POD_PROP_RANGE_NONE && rt2 == SPA_POD_PROP_RANGE_NONE) ||
        (rt1 == SPA_POD_PROP_RANGE_NONE && rt2 == SPA_POD_PROP_RANGE_ENUM) ||
        (rt1 == SPA_POD_PROP_RANGE_ENUM && rt2 == SPA_POD_PROP_RANGE_NONE) ||
        (rt1 == SPA_POD_PROP_RANGE_ENUM && rt2 == SPA_POD_PROP_RANGE_ENUM)) {
      int n_copied = 0;
      /* copy all equal values */
      for (j = 0, a1 = alt1; j < nalt1; j++, a1 += p1->body.value.size) {
        for (k = 0, a2 = alt2; k < nalt2; k++, a2 += p2->body.value.size) {
          if (compare_value (p1->body.value.type, a1, a2) == 0) {
            spa_pod_builder_raw (b, a1, p1->body.value.size, false);
            n_copied++;
          }
        }
      }
      if (n_copied == 0)
        return SPA_RESULT_NO_FORMAT;
      if (n_copied == 1)
        np->body.flags |= SPA_POD_PROP_RANGE_NONE;
      else
        np->body.flags |= SPA_POD_PROP_RANGE_ENUM | SPA_POD_PROP_FLAG_UNSET;
    }

    if ((rt1 == SPA_POD_PROP_RANGE_NONE && rt2 == SPA_POD_PROP_RANGE_MIN_MAX) ||
        (rt1 == SPA_POD_PROP_RANGE_ENUM && rt2 == SPA_POD_PROP_RANGE_MIN_MAX)) {
      int n_copied = 0;
      /* copy all values inside the range */
      for (j = 0, a1 = alt1, a2 = alt2; j < nalt1; j++, a1 += p1->body.value.size) {
        if (compare_value (p1->body.value.type, a1, a2) < 0)
          continue;
        if (compare_value (p1->body.value.type, a1, a2 + p2->body.value.size) > 0)
          continue;
        spa_pod_builder_raw (b, a1, p1->body.value.size, false);
        n_copied++;
      }
      if (n_copied == 0)
        return SPA_RESULT_NO_FORMAT;
      if (n_copied == 1)
        np->body.flags |= SPA_POD_PROP_RANGE_NONE;
      else
        np->body.flags |= SPA_POD_PROP_RANGE_ENUM | SPA_POD_PROP_FLAG_UNSET;
    }

    if (rt1 == SPA_POD_PROP_RANGE_NONE && rt2 == SPA_POD_PROP_RANGE_STEP)
      return SPA_RESULT_NOT_IMPLEMENTED;

    if (rt1 == SPA_POD_PROP_RANGE_NONE && rt2 == SPA_POD_PROP_RANGE_FLAGS)
      return SPA_RESULT_NOT_IMPLEMENTED;

    if ((rt1 == SPA_POD_PROP_RANGE_MIN_MAX && rt2 == SPA_POD_PROP_RANGE_NONE) ||
        (rt1 == SPA_POD_PROP_RANGE_MIN_MAX && rt2 == SPA_POD_PROP_RANGE_ENUM)) {
      int n_copied = 0;
      /* copy all values inside the range */
      for (k = 0, a1 = alt2, a2 = alt2; k < nalt2; k++, a2 += p2->body.value.size) {
        if (compare_value (p1->body.value.type, a2, a1) < 0)
          continue;
        if (compare_value (p1->body.value.type, a2, a1 + p1->body.value.size) > 0)
          continue;
        spa_pod_builder_raw (b, a2, p2->body.value.size, false);
        n_copied++;
      }
      if (n_copied == 0)
        return SPA_RESULT_NO_FORMAT;
      if (n_copied == 1)
        np->body.flags |= SPA_POD_PROP_RANGE_NONE;
      else
        np->body.flags |= SPA_POD_PROP_RANGE_ENUM | SPA_POD_PROP_FLAG_UNSET;
    }

    if (rt1 == SPA_POD_PROP_RANGE_MIN_MAX && rt2 == SPA_POD_PROP_RANGE_MIN_MAX) {
      if (compare_value (p1->body.value.type, alt1, alt2) < 0)
        spa_pod_builder_raw (b, alt2, p2->body.value.size, false);
      else
        spa_pod_builder_raw (b, alt1, p1->body.value.size, false);

      alt1 += p1->body.value.size;
      alt2 += p2->body.value.size;

      if (compare_value (p1->body.value.type, alt1, alt2) < 0)
        spa_pod_builder_raw (b, alt1, p1->body.value.size, false);
      else
        spa_pod_builder_raw (b, alt2, p2->body.value.size, false);
    }
    if (rt1 == SPA_POD_PROP_RANGE_MIN_MAX && rt2 == SPA_POD_PROP_RANGE_STEP)
      return SPA_RESULT_NOT_IMPLEMENTED;

    if (rt1 == SPA_POD_PROP_RANGE_MIN_MAX && rt2 == SPA_POD_PROP_RANGE_FLAGS)
      return SPA_RESULT_NOT_IMPLEMENTED;

    if (rt1 == SPA_POD_PROP_RANGE_ENUM && rt2 == SPA_POD_PROP_RANGE_STEP)
      return SPA_RESULT_NOT_IMPLEMENTED;
    if (rt1 == SPA_POD_PROP_RANGE_ENUM && rt2 == SPA_POD_PROP_RANGE_FLAGS)
      return SPA_RESULT_NOT_IMPLEMENTED;

    if (rt1 == SPA_POD_PROP_RANGE_STEP && rt2 == SPA_POD_PROP_RANGE_NONE)
      return SPA_RESULT_NOT_IMPLEMENTED;
    if (rt1 == SPA_POD_PROP_RANGE_STEP && rt2 == SPA_POD_PROP_RANGE_MIN_MAX)
      return SPA_RESULT_NOT_IMPLEMENTED;

    if (rt1 == SPA_POD_PROP_RANGE_STEP && rt2 == SPA_POD_PROP_RANGE_STEP)
      return SPA_RESULT_NOT_IMPLEMENTED;
    if (rt1 == SPA_POD_PROP_RANGE_STEP && rt2 == SPA_POD_PROP_RANGE_ENUM)
      return SPA_RESULT_NOT_IMPLEMENTED;
    if (rt1 == SPA_POD_PROP_RANGE_STEP && rt2 == SPA_POD_PROP_RANGE_FLAGS)
      return SPA_RESULT_NOT_IMPLEMENTED;

    if (rt1 == SPA_POD_PROP_RANGE_FLAGS && rt2 == SPA_POD_PROP_RANGE_NONE)
      return SPA_RESULT_NOT_IMPLEMENTED;
    if (rt1 == SPA_POD_PROP_RANGE_FLAGS && rt2 == SPA_POD_PROP_RANGE_MIN_MAX)
      return SPA_RESULT_NOT_IMPLEMENTED;
    if (rt1 == SPA_POD_PROP_RANGE_FLAGS && rt2 == SPA_POD_PROP_RANGE_STEP)
      return SPA_RESULT_NOT_IMPLEMENTED;
    if (rt1 == SPA_POD_PROP_RANGE_FLAGS && rt2 == SPA_POD_PROP_RANGE_ENUM)
      return SPA_RESULT_NOT_IMPLEMENTED;
    if (rt1 == SPA_POD_PROP_RANGE_FLAGS && rt2 == SPA_POD_PROP_RANGE_FLAGS)
      return SPA_RESULT_NOT_IMPLEMENTED;

    spa_pod_builder_pop (b, &f);
  }
  return SPA_RESULT_OK;
}
