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

#include "serialize.h"

size_t
pinos_serialize_buffer_get_size (const SpaBuffer *buffer)
{
  size_t size;
  unsigned int i;

  if (buffer == NULL)
    return 0;

  size = sizeof (SpaBuffer);
  for (i = 0; i < buffer->n_metas; i++)
    size += sizeof (SpaMeta) + buffer->metas[i].size;
  for (i = 0; i < buffer->n_datas; i++)
    size += sizeof (SpaData);
  return size;
}

size_t
pinos_serialize_buffer_serialize (void *dest, const SpaBuffer *buffer)
{
  SpaBuffer *tb;
  SpaMeta *mp;
  SpaData *dp;
  void *p;
  unsigned int i;

  if (buffer == NULL)
    return 0;

  tb = dest;
  memcpy (tb, buffer, sizeof (SpaBuffer));
  mp = SPA_MEMBER (tb, sizeof(SpaBuffer), SpaMeta);
  dp = SPA_MEMBER (mp, sizeof(SpaMeta) * tb->n_metas, SpaData);
  p = SPA_MEMBER (dp, sizeof(SpaData) * tb->n_datas, void);

  tb->metas = SPA_INT_TO_PTR (SPA_PTRDIFF (mp, tb));
  tb->datas = SPA_INT_TO_PTR (SPA_PTRDIFF (dp, tb));

  for (i = 0; i < tb->n_metas; i++) {
    memcpy (&mp[i], &buffer->metas[i], sizeof (SpaMeta));
    memcpy (p, mp[i].data, mp[i].size);
    mp[i].data = SPA_INT_TO_PTR (SPA_PTRDIFF (p, tb));
    p += mp[i].size;
  }
  for (i = 0; i < tb->n_datas; i++)
    memcpy (&dp[i], &buffer->datas[i], sizeof (SpaData));

  return SPA_PTRDIFF (p, tb);
}

SpaBuffer *
pinos_serialize_buffer_deserialize (void *src, off_t offset)
{
  SpaBuffer *b;
  unsigned int i;

  b = SPA_MEMBER (src, offset, SpaBuffer);
  if (b->metas)
    b->metas = SPA_MEMBER (b, SPA_PTR_TO_INT (b->metas), SpaMeta);
  for (i = 0; i < b->n_metas; i++) {
    SpaMeta *m = &b->metas[i];
    if (m->data)
      m->data = SPA_MEMBER (b, SPA_PTR_TO_INT (m->data), void);
  }
  if (b->datas)
    b->datas = SPA_MEMBER (b, SPA_PTR_TO_INT (b->datas), SpaData);

  return b;
}


size_t
pinos_serialize_format_get_size (const SpaFormat *format)
{
  if (format == NULL)
    return 0;

  return pinos_serialize_props_get_size (&format->props) - sizeof (SpaProps) + sizeof (SpaFormat);
}

size_t
pinos_serialize_format_serialize (void *dest, const SpaFormat *format)
{
  SpaFormat *tf;
  size_t size;

  if (format == NULL)
    return 0;

  tf = dest;
  tf->media_type = format->media_type;
  tf->media_subtype = format->media_subtype;

  dest = SPA_MEMBER (tf, offsetof (SpaFormat, props), void);
  size = pinos_serialize_props_serialize (dest, &format->props) - sizeof (SpaProps) + sizeof (SpaFormat);

  return size;
}

SpaFormat *
pinos_serialize_format_deserialize (void *src, off_t offset)
{
  SpaFormat *f;

  f = SPA_MEMBER (src, offset, SpaFormat);
  pinos_serialize_props_deserialize (f, offsetof (SpaFormat, props));

  return f;
}

SpaFormat *
pinos_serialize_format_copy_into (void *dest, const SpaFormat *format)
{
  if (format == NULL)
    return NULL;

  pinos_serialize_format_serialize (dest, format);
  return pinos_serialize_format_deserialize (dest, 0);
}

size_t
pinos_serialize_port_info_get_size (const SpaPortInfo *info)
{
  size_t len;
  unsigned int i;

  if (info == NULL)
    return 0;

  len = sizeof (SpaPortInfo);
  len += info->n_params * sizeof (SpaAllocParam *);
  for (i = 0; i < info->n_params; i++)
    len += info->params[i]->size;

  return len;
}

size_t
pinos_serialize_port_info_serialize (void *p, const SpaPortInfo *info)
{
  SpaPortInfo *pi;
  SpaAllocParam **ap;
  int i;
  size_t len;

  if (info == NULL)
    return 0;

  pi = p;
  memcpy (pi, info, sizeof (SpaPortInfo));

  ap = SPA_MEMBER (pi, sizeof (SpaPortInfo), SpaAllocParam *);
  if (info->n_params)
    pi->params = SPA_INT_TO_PTR (SPA_PTRDIFF (ap, pi));
  else
    pi->params = 0;
  pi->extra = 0;

  p = SPA_MEMBER (ap, sizeof (SpaAllocParam*) * info->n_params, void);

  for (i = 0; i < info->n_params; i++) {
    len = info->params[i]->size;
    memcpy (p, info->params[i], len);
    ap[i] = SPA_INT_TO_PTR (SPA_PTRDIFF (p, pi));
    p = SPA_MEMBER (p, len, void);
  }
  return SPA_PTRDIFF (p, pi);
}

SpaPortInfo *
pinos_serialize_port_info_deserialize (void *p, off_t offset)
{
  SpaPortInfo *pi;
  unsigned int i;

  pi = SPA_MEMBER (p, offset, SpaPortInfo);
  if (pi->params)
    pi->params = SPA_MEMBER (pi, SPA_PTR_TO_INT (pi->params), SpaAllocParam *);
  for (i = 0; i < pi->n_params; i++) {
    pi->params[i] = SPA_MEMBER (pi, SPA_PTR_TO_INT (pi->params[i]), SpaAllocParam);
  }
  return pi;
}

SpaPortInfo *
pinos_serialize_port_info_copy_into (void *dest, const SpaPortInfo *info)
{
  if (info == NULL)
    return NULL;

  pinos_serialize_port_info_serialize (dest, info);
  return pinos_serialize_port_info_deserialize (dest, 0);
}

size_t
pinos_serialize_props_get_size (const SpaProps *props)
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
    /* for the value */
    len += pi->maxsize;
    for (j = 0; j < pi->n_range_values; j++) {
      ri = (SpaPropRangeInfo *)&pi->range_values[j];
      len += sizeof (SpaPropRangeInfo);
      len += ri->name ? strlen (ri->name) + 1 : 0;
      /* the size of the range value */
      len += ri->val.size;
    }
  }
  return len;
}

size_t
pinos_serialize_props_serialize (void *p, const SpaProps *props)
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
    for (j = 0; j < pi[i].n_range_values; j++, c++) {
      if (ri[c].name) {
        slen = strlen (ri[c].name) + 1;
        memcpy (p, ri[c].name, slen);
        ri[c].name = SPA_INT_TO_PTR (SPA_PTRDIFF (p, tp));
        p += slen;
      } else {
        ri[c].name = 0;
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
pinos_serialize_props_deserialize (void *p, off_t offset)
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
    if (pi->range_values)
      pi->range_values = SPA_MEMBER (tp, SPA_PTR_TO_INT (pi->range_values), SpaPropRangeInfo);

    for (j = 0; j < pi->n_range_values; j++) {
      ri = (SpaPropRangeInfo *) &pi->range_values[j];
      if (ri->name)
        ri->name = SPA_MEMBER (tp, SPA_PTR_TO_INT (ri->name), char);
      if (ri->val.value)
        ri->val.value = SPA_MEMBER (tp, SPA_PTR_TO_INT (ri->val.value), void);
    }
  }
  return tp;
}

SpaProps *
pinos_serialize_props_copy_into (void *dest, const SpaProps *props)
{
  if (props == NULL)
    return NULL;

  pinos_serialize_props_serialize (dest, props);
  return pinos_serialize_props_deserialize (dest, 0);
}
