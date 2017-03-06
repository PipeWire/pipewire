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
    size += sizeof (SpaMeta);
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

  for (i = 0; i < tb->n_metas; i++)
    memcpy (&mp[i], &buffer->metas[i], sizeof (SpaMeta));
  for (i = 0; i < tb->n_datas; i++)
    memcpy (&dp[i], &buffer->datas[i], sizeof (SpaData));

  return SPA_PTRDIFF (p, tb);
}

SpaBuffer *
pinos_serialize_buffer_deserialize (void *src, off_t offset)
{
  SpaBuffer *b;

  b = SPA_MEMBER (src, offset, SpaBuffer);
  if (b->metas)
    b->metas = SPA_MEMBER (b, SPA_PTR_TO_INT (b->metas), SpaMeta);
  if (b->datas)
    b->datas = SPA_MEMBER (b, SPA_PTR_TO_INT (b->datas), SpaData);

  return b;
}

SpaBuffer *
pinos_serialize_buffer_copy_into (void *dest, const SpaBuffer *buffer)
{
  if (buffer == NULL)
    return NULL;

  pinos_serialize_buffer_serialize (dest, buffer);
  return pinos_serialize_buffer_deserialize (dest, 0);
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
