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

#ifndef __SPA_NODE_EVENT_H__
#define __SPA_NODE_EVENT_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaNodeEvent SpaNodeEvent;

#include <spa/defs.h>
#include <spa/node.h>

#define SPA_NODE_EVENT_URI             "http://spaplug.in/ns/node-event"
#define SPA_NODE_EVENT_PREFIX          SPA_NODE_EVENT_URI "#"

#define SPA_NODE_EVENT__AsyncComplete         SPA_NODE_EVENT_PREFIX "AsyncComplete"
#define SPA_NODE_EVENT__HaveOutput            SPA_NODE_EVENT_PREFIX "HaveOutput"
#define SPA_NODE_EVENT__NeedInput             SPA_NODE_EVENT_PREFIX "NeedInput"
#define SPA_NODE_EVENT__ReuseBuffer           SPA_NODE_EVENT_PREFIX "ReuseBuffer"
#define SPA_NODE_EVENT__Error                 SPA_NODE_EVENT_PREFIX "Error"
#define SPA_NODE_EVENT__Buffering             SPA_NODE_EVENT_PREFIX "Buffering"
#define SPA_NODE_EVENT__RequestRefresh        SPA_NODE_EVENT_PREFIX "RequestRefresh"
#define SPA_NODE_EVENT__RequestClockUpdate    SPA_NODE_EVENT_PREFIX "RequestClockUpdate"

/**
 * SpaEventType:
 * @SPA_NODE_EVENT_INVALID: invalid event, should be ignored
 * @SPA_NODE_EVENT_ASYNC_COMPLETE: an async operation completed
 * @SPA_NODE_EVENT_HAVE_OUTPUT: emited when an async node has output that can be pulled
 * @SPA_NODE_EVENT_NEED_INPUT: emited when more data can be pushed to an async node
 * @SPA_NODE_EVENT_REUSE_BUFFER: emited when a buffer can be reused
 * @SPA_NODE_EVENT_ERROR: emited when error occured
 * @SPA_NODE_EVENT_BUFFERING: emited when buffering is in progress
 * @SPA_NODE_EVENT_REQUEST_REFRESH: emited when a keyframe refresh is needed
 * @SPA_NODE_EVENT_REQUEST_CLOCK_UPDATE: the element asks for a clock update
 */
typedef enum {
  SPA_NODE_EVENT_INVALID                  = 0,
  SPA_NODE_EVENT_ASYNC_COMPLETE,
  SPA_NODE_EVENT_HAVE_OUTPUT,
  SPA_NODE_EVENT_NEED_INPUT,
  SPA_NODE_EVENT_REUSE_BUFFER,
  SPA_NODE_EVENT_ERROR,
  SPA_NODE_EVENT_BUFFERING,
  SPA_NODE_EVENT_REQUEST_REFRESH,
  SPA_NODE_EVENT_REQUEST_CLOCK_UPDATE,
} SpaNodeEventType;

#define SPA_NODE_EVENT_TYPE(ev)   ((ev)->body.body.type)

typedef struct {
  SpaPODObjectBody body;
} SpaNodeEventBody;

struct _SpaNodeEvent {
  SpaPOD           pod;
  SpaNodeEventBody body;
};

#define SPA_NODE_EVENT_INIT(type)                       \
  { { sizeof (SpaNodeEventBody), SPA_POD_TYPE_OBJECT }, \
    { { 0, type } } }                                   \

#define SPA_NODE_EVENT_INIT_COMPLEX(size,type,...)      \
  { { size, SPA_POD_TYPE_OBJECT },                      \
    { { 0, type }, __VA_ARGS__ } }                      \

typedef struct {
  SpaPODObjectBody body;
  SpaPODInt        seq         SPA_ALIGNED (8);
  SpaPODInt        res         SPA_ALIGNED (8);
} SpaNodeEventAsyncCompleteBody;

typedef struct {
  SpaPOD                        pod;
  SpaNodeEventAsyncCompleteBody body;
} SpaNodeEventAsyncComplete;

#define SPA_NODE_EVENT_ASYNC_COMPLETE_INIT(seq,res)                     \
  SPA_NODE_EVENT_INIT_COMPLEX (sizeof (SpaNodeEventAsyncCompleteBody),  \
                               SPA_NODE_EVENT_ASYNC_COMPLETE,           \
      SPA_POD_INT_INIT (seq),                                           \
      SPA_POD_INT_INIT (res))

typedef struct {
  SpaPODObjectBody body;
  SpaPODInt        port_id;
  SpaPODInt        buffer_id;
} SpaNodeEventReuseBufferBody;

typedef struct {
  SpaPOD                      pod;
  SpaNodeEventReuseBufferBody body;
} SpaNodeEventReuseBuffer;

#define SPA_NODE_EVENT_REUSE_BUFFER_INIT(port_id,buffer_id)             \
  SPA_NODE_EVENT_INIT_COMPLEX (sizeof (SpaNodeEventReuseBufferBody),    \
                               SPA_NODE_EVENT_REUSE_BUFFER,             \
      SPA_POD_INT_INIT (port_id),                                       \
      SPA_POD_INT_INIT (buffer_id))

typedef struct {
  SpaPODObjectBody body;
#define SPA_NODE_EVENT_REQUEST_CLOCK_UPDATE_TIME        (1 << 0)
#define SPA_NODE_EVENT_REQUEST_CLOCK_UPDATE_SCALE       (1 << 1)
#define SPA_NODE_EVENT_REQUEST_CLOCK_UPDATE_STATE       (1 << 2)
  SpaPODInt        update_mask  SPA_ALIGNED (8);
  SpaPODLong       timestamp    SPA_ALIGNED (8);
  SpaPODLong       offset       SPA_ALIGNED (8);
} SpaNodeEventRequestClockUpdateBody;

typedef struct {
  SpaPOD                             pod;
  SpaNodeEventRequestClockUpdateBody body;
} SpaNodeEventRequestClockUpdate;

#define SPA_NODE_EVENT_REQUEST_CLOCK_UPDATE_INIT(update_mask,timestamp,offset)  \
  SPA_NODE_EVENT_INIT_COMPLEX (sizeof (SpaNodeEventRequestClockUpdateBody),     \
                               SPA_NODE_EVENT_REQUEST_CLOCK_UPDATE,             \
      SPA_POD_INT_INIT (update_mask),                                           \
      SPA_POD_LONG_INIT (timestamp),                                            \
      SPA_POD_LONG_INIT (offset))

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_NODE_EVENT_H__ */
