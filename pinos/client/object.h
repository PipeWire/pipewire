/* Pinos
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PINOS_OBJECT_H__
#define __PINOS_OBJECT_H__

#include <stdint.h>

#include <pinos/client/signal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _PinosObject PinosObject;

typedef enum {
  PINOS_OBJECT_FLAG_NONE          = 0,
  PINOS_OBJECT_FLAG_DESTROYING    = (1 << 0),
} PinosObjectFlags;

typedef void (*PinosDestroyFunc)  (PinosObject *object);

struct _PinosObject {
  uint32_t          type;
  uint32_t          id;
  void             *implementation;
  PinosObjectFlags  flags;
  PinosDestroyFunc  destroy;
  PinosSignal       destroy_signal;
};

static inline void
pinos_object_init (PinosObject      *object,
                   uint32_t          type,
                   void             *implementation,
                   PinosDestroyFunc  destroy)
{
  object->type = type;
  object->id = SPA_ID_INVALID;
  object->implementation = implementation;
  object->destroy = destroy;
  pinos_signal_init (&object->destroy_signal);
}


#ifdef __cplusplus
}
#endif

#endif /* __PINOS_OBJECT_H__ */
