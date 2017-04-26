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

#ifndef __PINOS_TYPE_H__
#define __PINOS_TYPE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <pinos/client/map.h>
#include <spa/include/spa/type-map.h>
#include <spa/include/spa/event-node.h>
#include <spa/include/spa/command-node.h>
#include <spa/include/spa/monitor.h>
#include <spa/include/spa/alloc-param.h>

typedef struct _PinosType PinosType;

/**
 * PinosType:
 *
 * Pinos Type support struct.
 */
struct _PinosType {
  SpaTypeMap *map;

  SpaType core;
  SpaType registry;
  SpaType node;
  SpaType node_factory;
  SpaType link;
  SpaType client;
  SpaType client_node;
  SpaType module;

  SpaType spa_node;
  SpaType spa_clock;
  SpaType spa_monitor;
  SpaType spa_format;
  SpaType spa_props;

  SpaTypeMeta meta;
  SpaTypeData data;
  SpaTypeEventNode event_node;
  SpaTypeCommandNode command_node;
  SpaTypeMonitor monitor;
  SpaTypeAllocParamBuffers alloc_param_buffers;
  SpaTypeAllocParamMetaEnable alloc_param_meta_enable;
  SpaTypeAllocParamVideoPadding alloc_param_video_padding;
};

void pinos_type_init (PinosType *type);

bool pinos_pod_remap_data  (uint32_t type, void *body, uint32_t size, PinosMap *types);

static inline bool
pinos_pod_remap (SpaPOD *pod, PinosMap *types)
{
  return pinos_pod_remap_data (pod->type, SPA_POD_BODY (pod), pod->size, types);
}

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_TYPE_H__ */
