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

#include <spa/port.h>
#include <spa/debug.h>


size_t
spa_port_info_get_size (const SpaPortInfo *info)
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
spa_port_info_serialize (void *p, const SpaPortInfo *info)
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
  pi->features = 0;

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
spa_port_info_deserialize (void *p, off_t offset)
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
spa_port_info_copy_into (void *dest, const SpaPortInfo *info)
{
  if (info == NULL)
    return NULL;

  spa_port_info_serialize (dest, info);
  return spa_port_info_deserialize (dest, 0);
}
