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

#ifndef __PINOS_ARRAY_H__
#define __PINOS_ARRAY_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _PinosArray PinosArray;

#include <spa/defs.h>

struct _PinosArray {
  void    *data;
  size_t   size;
  size_t   alloc;
};

#define pinos_array_get_len_s(a,s)              ((a)->size / (s))
#define pinos_array_get_unchecked_s(a,idx,s,t)  SPA_MEMBER ((a)->data,(idx)*(s),t)
#define pinos_array_check_index_s(a,idx,s)      ((idx) < pinos_array_get_len(a,s))

#define pinos_array_get_len(a,t)                pinos_array_get_len_s(a,sizeof(t))
#define pinos_array_get_unchecked(a,idx,t)      pinos_array_get_unchecked_s(a,idx,sizeof(t),t)
#define pinos_array_check_index(a,idx,t)        pinos_array_check_index_s(a,idx,sizeof(t))

#define pinos_array_for_each(pos, array)                                                \
    for (pos = (array)->data;                                                           \
         (const uint8_t *) pos < ((const uint8_t *) (array)->data + (array)->size);     \
         (pos)++)

static inline void
pinos_array_init (PinosArray *arr)
{
  arr->data = NULL;
  arr->size = arr->alloc = 0;
}

static inline void
pinos_array_clear (PinosArray *arr)
{
  free (arr->data);
}

static inline bool
pinos_array_ensure_size (PinosArray *arr,
                         size_t size)
{
  size_t alloc, need;

  alloc = arr->alloc;
  need = arr->size + size;

  if (SPA_UNLIKELY (alloc < need)) {
    void *data;
    alloc = SPA_MAX (alloc, 16);
    while (alloc < need)
      alloc *= 2;
    if (SPA_UNLIKELY ((data = realloc (arr->data, alloc)) == NULL))
      return false;
    arr->data = data;
    arr->alloc = alloc;
  }
  return true;
}

static inline void *
pinos_array_add (PinosArray *arr,
                 size_t      size)
{
  void *p;

  if (!pinos_array_ensure_size (arr, size))
    return NULL;

  p = arr->data + arr->size;
  arr->size += size;

  return p;
}

static inline void *
pinos_array_add_fixed (PinosArray *arr,
                       size_t      size)
{
  void *p;

  if (SPA_UNLIKELY (arr->alloc < arr->size + size))
    return NULL;

  p = arr->data + arr->size;
  arr->size += size;

  return p;
}

#define pinos_array_add_ptr(a,p)                                \
  *((void**) pinos_array_add (a, sizeof (void*))) = (p)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PINOS_ARRAY_H__ */
