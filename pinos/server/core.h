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

#ifndef __PINOS_CORE_H__
#define __PINOS_CORE_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PINOS_CORE_URI                            "http://pinos.org/ns/core"
#define PINOS_CORE_PREFIX                         PINOS_CORE_URI "#"

typedef struct _PinosCore PinosCore;

#include <spa/include/spa/log.h>
#include <pinos/server/main-loop.h>
#include <pinos/server/registry.h>

/**
 * PinosCore:
 *
 * Pinos core object class.
 */
struct _PinosCore {
  PinosObject object;

  PinosRegistry registry;

  PinosMainLoop *main_loop;

  SpaSupport *support;
  unsigned int n_support;
};

PinosCore *     pinos_core_new        (PinosMainLoop *main_loop);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_CORE_H__ */
