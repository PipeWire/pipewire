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

#ifndef __PINOS_PROPERTIES_H__
#define __PINOS_PROPERTIES_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _PinosProperties PinosProperties;

#include <spa/include/spa/dict.h>

struct _PinosProperties {
  SpaDict dict;
};

PinosProperties * pinos_properties_new      (const char *key, ...);
PinosProperties * pinos_properties_new_dict (const SpaDict *dict);
PinosProperties * pinos_properties_copy     (PinosProperties *properties);
PinosProperties * pinos_properties_merge    (PinosProperties *oldprops,
                                             PinosProperties *newprops);
void              pinos_properties_free     (PinosProperties *properties);

void              pinos_properties_set      (PinosProperties *properties,
                                             const char      *key,
                                             const char      *value);
void              pinos_properties_setf     (PinosProperties *properties,
                                             const char      *key,
                                             const char      *format,
                                             ...) SPA_PRINTF_FUNC (3, 4);
const char *      pinos_properties_get      (PinosProperties *properties,
                                             const char      *key);

const char *      pinos_properties_iterate  (PinosProperties *properties,
                                             void           **state);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_PROPERTIES_H__ */
