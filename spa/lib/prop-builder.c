/* Simple Plugin API
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#include "prop-builder.h"

void
spa_prop_builder_init (SpaPropBuilder *b,
                       size_t          struct_size,
                       off_t           prop_offset)
{
  memset (b, 0, sizeof (*b));
  b->size = struct_size;
  b->struct_size = struct_size;
  b->prop_offset = prop_offset;
}

void
spa_prop_builder_add_info (SpaPropBuilder     *b,
                           SpaPropBuilderInfo *i)
{
  if (b->info)
    b->info->next = i;
  if (b->head == NULL)
    b->head = i;
  b->info = i;
  b->n_prop_info++;
  i->info.n_range_values = 0;
  i->value = NULL;
  i->ranges = NULL;
  i->next = NULL;

  b->size += sizeof (SpaPropInfo);
  b->size += i->info.name ? strlen (i->info.name) + 1 : 0;
}

void
spa_prop_builder_add_range (SpaPropBuilder      *b,
			    SpaPropBuilderRange *r)
{
  if (b->range)
    b->range->next = r;
  b->range = r;
  if (b->info->ranges == NULL)
    b->info->ranges = r;

  r->next = NULL;
  b->info->info.n_range_values++;
  b->n_range_info++;
  b->size += sizeof (SpaPropRangeInfo);
  b->size += r->info.name ? strlen (r->info.name) + 1 : 0;
  b->size += r->info.val.size;
}

void *
spa_prop_builder_finish (SpaPropBuilder *b)
{
  int i, j, c, slen;
  void *p;
  SpaProps *tp;
  SpaPropInfo *pi;
  SpaPropRangeInfo *ri;
  SpaPropBuilderInfo *ppi;
  SpaPropBuilderRange *pri;

  if (b->dest == NULL && b->finish)
    b->finish (b);
  if (b->dest == NULL)
    return NULL;

  tp = SPA_MEMBER (b->dest, b->prop_offset, SpaProps);
  pi = SPA_MEMBER (b->dest, b->struct_size, SpaPropInfo);
  ri = SPA_MEMBER (pi, sizeof(SpaPropInfo) * b->n_prop_info, SpaPropRangeInfo);
  p = SPA_MEMBER (ri, sizeof(SpaPropRangeInfo) * b->n_range_info, void);

  tp->n_prop_info = b->n_prop_info;
  tp->prop_info = pi;
  tp->unset_mask = 0;

  ppi = b->head;
  for (i = 0, c = 0; i < tp->n_prop_info; i++) {
    memcpy (&pi[i], &ppi->info, sizeof (SpaPropInfo));
    pi[i].range_values = &ri[c];

    if (pi[i].name) {
      slen = strlen (pi[i].name) + 1;
      memcpy (p, pi[i].name, slen);
      pi[i].name = p;
      p += slen;
    }

    if (pi[i].n_range_values == 1) {
      memcpy (SPA_MEMBER (tp, pi[i].offset, void), ppi->ranges[0].info.val.value, ppi->ranges[0].info.val.size);
      ppi->value = ppi->ranges[0].info.val.value;
    } else if (pi[i].n_range_values > 1) {
      tp->unset_mask |= (1 << i);
    }
    if (ppi->value) {
      memcpy (SPA_MEMBER (tp, pi[i].offset, void), ppi->value, ppi->info.maxsize);
    }

    pri = ppi->ranges;
    for (j = 0; j < pi[i].n_range_values; j++, c++) {
      memcpy (&ri[c], &pri->info, sizeof (SpaPropRangeInfo));

      if (ri[c].name) {
        slen = strlen (ri[c].name) + 1;
        memcpy (p, ri[c].name, slen);
        ri[c].name = p;
        p += slen;
      }
      if (ri[c].val.size) {
        memcpy (p, ri[c].val.value, ri[c].val.size);
        ri[c].val.value = p;
        p += ri[c].val.size;
      }
      pri = pri->next;
    }
    ppi = ppi->next;
  }
  return b->dest;
}
