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

#ifndef __SPA_EVENT_NODE_H__
#define __SPA_EVENT_NODE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/defs.h>
#include <spa/event.h>
#include <spa/id-map.h>
#include <spa/node.h>

#define SPA_TYPE_EVENT__Node             SPA_TYPE_EVENT_BASE "Node"
#define SPA_TYPE_EVENT_NODE_BASE         SPA_TYPE_EVENT__Node ":"

#define SPA_TYPE_EVENT_NODE__AsyncComplete         SPA_TYPE_EVENT_NODE_BASE "AsyncComplete"
#define SPA_TYPE_EVENT_NODE__HaveOutput            SPA_TYPE_EVENT_NODE_BASE "HaveOutput"
#define SPA_TYPE_EVENT_NODE__NeedInput             SPA_TYPE_EVENT_NODE_BASE "NeedInput"
#define SPA_TYPE_EVENT_NODE__ReuseBuffer           SPA_TYPE_EVENT_NODE_BASE "ReuseBuffer"
#define SPA_TYPE_EVENT_NODE__Error                 SPA_TYPE_EVENT_NODE_BASE "Error"
#define SPA_TYPE_EVENT_NODE__Buffering             SPA_TYPE_EVENT_NODE_BASE "Buffering"
#define SPA_TYPE_EVENT_NODE__RequestRefresh        SPA_TYPE_EVENT_NODE_BASE "RequestRefresh"
#define SPA_TYPE_EVENT_NODE__RequestClockUpdate    SPA_TYPE_EVENT_NODE_BASE "RequestClockUpdate"

typedef struct {
  uint32_t AsyncComplete;
  uint32_t HaveOutput;
  uint32_t NeedInput;
  uint32_t ReuseBuffer;
  uint32_t Error;
  uint32_t Buffering;
  uint32_t RequestRefresh;
  uint32_t RequestClockUpdate;
} SpaEventNode;

static inline void
spa_event_node_map (SpaIDMap *map, SpaEventNode *types)
{
  if (types->AsyncComplete == 0) {
    types->AsyncComplete        = spa_id_map_get_id (map, SPA_TYPE_EVENT_NODE__AsyncComplete);
    types->HaveOutput           = spa_id_map_get_id (map, SPA_TYPE_EVENT_NODE__HaveOutput);
    types->NeedInput            = spa_id_map_get_id (map, SPA_TYPE_EVENT_NODE__NeedInput);
    types->ReuseBuffer          = spa_id_map_get_id (map, SPA_TYPE_EVENT_NODE__ReuseBuffer);
    types->Error                = spa_id_map_get_id (map, SPA_TYPE_EVENT_NODE__Error);
    types->Buffering            = spa_id_map_get_id (map, SPA_TYPE_EVENT_NODE__Buffering);
    types->RequestRefresh       = spa_id_map_get_id (map, SPA_TYPE_EVENT_NODE__RequestRefresh);
    types->RequestClockUpdate   = spa_id_map_get_id (map, SPA_TYPE_EVENT_NODE__RequestClockUpdate);
  }
}

typedef struct {
  SpaPODObjectBody body;
  SpaPODInt        seq         SPA_ALIGNED (8);
  SpaPODInt        res         SPA_ALIGNED (8);
} SpaEventNodeAsyncCompleteBody;

typedef struct {
  SpaPOD                        pod;
  SpaEventNodeAsyncCompleteBody body;
} SpaEventNodeAsyncComplete;

#define SPA_EVENT_NODE_ASYNC_COMPLETE_INIT(type,seq,res)                \
  SPA_EVENT_INIT_COMPLEX (sizeof (SpaEventNodeAsyncCompleteBody), type, \
      SPA_POD_INT_INIT (seq),                                           \
      SPA_POD_INT_INIT (res))

typedef struct {
  SpaPODObjectBody body;
  SpaPODInt        port_id;
  SpaPODInt        buffer_id;
} SpaEventNodeReuseBufferBody;

typedef struct {
  SpaPOD                      pod;
  SpaEventNodeReuseBufferBody body;
} SpaEventNodeReuseBuffer;

#define SPA_EVENT_NODE_REUSE_BUFFER_INIT(type,port_id,buffer_id)        \
  SPA_EVENT_INIT_COMPLEX (sizeof (SpaEventNodeReuseBufferBody), type,   \
      SPA_POD_INT_INIT (port_id),                                       \
      SPA_POD_INT_INIT (buffer_id))

typedef struct {
  SpaPODObjectBody body;
#define SPA_EVENT_NODE_REQUEST_CLOCK_UPDATE_TIME        (1 << 0)
#define SPA_EVENT_NODE_REQUEST_CLOCK_UPDATE_SCALE       (1 << 1)
#define SPA_EVENT_NODE_REQUEST_CLOCK_UPDATE_STATE       (1 << 2)
  SpaPODInt        update_mask  SPA_ALIGNED (8);
  SpaPODLong       timestamp    SPA_ALIGNED (8);
  SpaPODLong       offset       SPA_ALIGNED (8);
} SpaEventNodeRequestClockUpdateBody;

typedef struct {
  SpaPOD                             pod;
  SpaEventNodeRequestClockUpdateBody body;
} SpaEventNodeRequestClockUpdate;

#define SPA_EVENT_NODE_REQUEST_CLOCK_UPDATE_INIT(type,update_mask,timestamp,offset)     \
  SPA_EVENT_INIT_COMPLEX (sizeof (SpaEventNodeRequestClockUpdateBody), type,            \
      SPA_POD_INT_INIT (update_mask),                                                   \
      SPA_POD_LONG_INIT (timestamp),                                                    \
      SPA_POD_LONG_INIT (offset))

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_EVENT_NODE_H__ */
