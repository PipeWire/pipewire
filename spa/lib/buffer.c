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

#include <spa/buffer.h>
#include <spa/port.h>

static const size_t header_sizes[] = {
  0,
  sizeof (SpaMetaHeader),
  sizeof (SpaMetaPointer),
  sizeof (SpaMetaVideoCrop),
  sizeof (SpaMetaRingbuffer),
};

size_t
spa_meta_type_get_size (SpaMetaType  type)
{
  if (type <= 0 || type >= SPA_N_ELEMENTS (header_sizes))
    return 0;

  return header_sizes[type];
}

size_t
spa_buffer_get_size (const SpaBuffer *buffer)
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
spa_buffer_serialize (void *dest, const SpaBuffer *buffer)
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
spa_buffer_deserialize (void *src, off_t offset)
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
