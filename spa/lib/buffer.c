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

SpaResult
spa_alloc_params_get_header_size (SpaAllocParam       **params,
                                  unsigned int          n_params,
                                  unsigned int          n_datas,
                                  size_t               *size)
{
  unsigned int i, n_metas = 0;

  *size = sizeof (SpaBuffer);

  for (i = 0; i < n_params; i++) {
    SpaAllocParam *p = params[i];

    switch (p->type) {
      case SPA_ALLOC_PARAM_TYPE_META_ENABLE:
      {
        SpaAllocParamMetaEnable *b = (SpaAllocParamMetaEnable *) p;

        if (b->type > 0 && b->type < SPA_N_ELEMENTS (header_sizes)) {
          n_metas++;
          *size += header_sizes[b->type];
        }
        break;
      }
      default:
        break;
    }
  }
  *size += n_metas * sizeof (SpaMeta);
  *size += n_datas * sizeof (SpaData);

  return SPA_RESULT_OK;
}

SpaResult
spa_buffer_init_headers (SpaAllocParam       **params,
                         unsigned int          n_params,
                         unsigned int          n_datas,
                         SpaBuffer           **buffers,
                         unsigned int          n_buffers)
{
  unsigned int i;
  int n_metas = 0;

  for (i = 0; i < n_params; i++) {
    SpaAllocParam *p = params[i];

    switch (p->type) {
      case SPA_ALLOC_PARAM_TYPE_META_ENABLE:
      {
        SpaAllocParamMetaEnable *b = (SpaAllocParamMetaEnable *) p;
        if (b->type > 0 && b->type <= SPA_N_ELEMENTS (header_sizes))
          n_metas++;
      }
      default:
        break;
    }
  }

  for (i = 0; i < n_buffers; i++) {
    int mi = 0, j;
    SpaBuffer *b;
    void *p;

    b = buffers[i];
    b->id = i;
    b->n_metas = n_metas;
    b->metas = SPA_MEMBER (b, sizeof (SpaBuffer), SpaMeta);
    b->n_datas = n_datas;
    b->datas = SPA_MEMBER (b->metas, sizeof (SpaMeta) * n_metas, SpaData);
    p = SPA_MEMBER (b->datas, sizeof (SpaData) * n_datas, void);

    for (j = 0, mi = 0; j < n_params; j++) {
      SpaAllocParam *prm = params[j];

      switch (prm->type) {
        case SPA_ALLOC_PARAM_TYPE_META_ENABLE:
        {
          SpaAllocParamMetaEnable *pme = (SpaAllocParamMetaEnable *) prm;

          if (pme->type > 0 && pme->type <= SPA_N_ELEMENTS (header_sizes)) {
            b->metas[mi].type = pme->type;
            b->metas[mi].data = p;
            b->metas[mi].size = header_sizes[pme->type];
            p = SPA_MEMBER (p, header_sizes[pme->type], void);
            mi++;
          }
          break;
        }
        default:
          break;
      }
    }
  }
  return SPA_RESULT_OK;
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
