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

#ifndef __PINOS_SPA_MONITOR_H__
#define __PINOS_SPA_MONITOR_H__

#include <pinos/server/core.h>
#include <spa/include/spa/monitor.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _PinosSpaMonitor PinosSpaMonitor;

struct _PinosSpaMonitor {
  SpaMonitor *monitor;

  char *lib;
  char *factory_name;
  SpaHandle *handle;

  PINOS_SIGNAL (destroy_signal, (PinosListener *listener, PinosSpaMonitor *monitor));
};

PinosSpaMonitor *      pinos_spa_monitor_load     (PinosCore  *core,
                                                   const char *lib,
                                                   const char *factory_name,
                                                   const char *args);
void                   pinos_spa_monitor_destroy  (PinosSpaMonitor *monitor);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_SPA_MONITOR_H__ */
