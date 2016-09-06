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
#include <spa/poll.h>
#include <spa/node.h>

/**
 * SpaEventType:
 * @SPA_NODE_EVENT_TYPE_INVALID: invalid event, should be ignored
 * @SPA_NODE_EVENT_TYPE_PORT_ADDED: a new port is added
 * @SPA_NODE_EVENT_TYPE_PORT_REMOVED: a port is removed
 * @SPA_NODE_EVENT_TYPE_STATE_CHANGE: emited when the state changes
 * @SPA_NODE_EVENT_TYPE_HAVE_OUTPUT: emited when an async node has output that can be pulled
 * @SPA_NODE_EVENT_TYPE_NEED_INPUT: emited when more data can be pushed to an async node
 * @SPA_NODE_EVENT_TYPE_REUSE_BUFFER: emited when a buffer can be reused
 * @SPA_NODE_EVENT_TYPE_ADD_POLL: emited when a pollfd should be added. data points to #SpaPollItem
 * @SPA_NODE_EVENT_TYPE_UPDATE_POLL: update the pollfd item
 * @SPA_NODE_EVENT_TYPE_REMOVE_POLL: emited when a pollfd should be removed. data points to #SpaPollItem
 * @SPA_NODE_EVENT_TYPE_DRAINED: emited when DRAIN command completed
 * @SPA_NODE_EVENT_TYPE_MARKER: emited when MARK command completed
 * @SPA_NODE_EVENT_TYPE_ERROR: emited when error occured
 * @SPA_NODE_EVENT_TYPE_BUFFERING: emited when buffering is in progress
 * @SPA_NODE_EVENT_TYPE_REQUEST_REFRESH: emited when a keyframe refresh is needed
 */
typedef enum {
  SPA_NODE_EVENT_TYPE_INVALID                  = 0,
  SPA_NODE_EVENT_TYPE_PORT_ADDED,
  SPA_NODE_EVENT_TYPE_PORT_REMOVED,
  SPA_NODE_EVENT_TYPE_STATE_CHANGE,
  SPA_NODE_EVENT_TYPE_HAVE_OUTPUT,
  SPA_NODE_EVENT_TYPE_NEED_INPUT,
  SPA_NODE_EVENT_TYPE_REUSE_BUFFER,
  SPA_NODE_EVENT_TYPE_ADD_POLL,
  SPA_NODE_EVENT_TYPE_UPDATE_POLL,
  SPA_NODE_EVENT_TYPE_REMOVE_POLL,
  SPA_NODE_EVENT_TYPE_DRAINED,
  SPA_NODE_EVENT_TYPE_MARKER,
  SPA_NODE_EVENT_TYPE_ERROR,
  SPA_NODE_EVENT_TYPE_BUFFERING,
  SPA_NODE_EVENT_TYPE_REQUEST_REFRESH,
} SpaNodeEventType;

struct _SpaNodeEvent {
  SpaNodeEventType  type;
  void             *data;
  size_t            size;
};

typedef struct {
  uint32_t     port_id;
} SpaNodeEventPortAdded;

typedef struct {
  uint32_t     port_id;
} SpaNodeEventPortRemoved;

typedef struct {
  SpaNodeState state;
} SpaNodeEventStateChange;

typedef struct {
  uint32_t     port_id;
} SpaNodeEventHaveOutput;

typedef struct {
  uint32_t     port_id;
} SpaNodeEventNeedInput;

typedef struct {
  uint32_t port_id;
  uint32_t buffer_id;
} SpaNodeEventReuseBuffer;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_NODE_EVENT_H__ */
