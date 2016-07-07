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

#ifndef __SPA_EVENT_H__
#define __SPA_EVENT_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaEvent SpaEvent;

#include <spa/defs.h>

/**
 * SpaEventType:
 * @SPA_EVENT_TYPE_INVALID: invalid event, should be ignored
 * @SPA_EVENT_TYPE_STARTED: emited when the START command completes
 * @SPA_EVENT_TYPE_STOPPED: emited when the STOP command completes
 * @SPA_EVENT_TYPE_CAN_PULL_OUTPUT: emited when an async node has output that can be pulled
 * @SPA_EVENT_TYPE_CAN_PUSH_INTPUT: emited when more data can be pushed to an async node
 * @SPA_EVENT_TYPE_PULL_INPUT: emited when data needs to be provided on an input
 * @SPA_EVENT_TYPE_ALLOC_OUTPUT: emited when an output buffer needs to be allocated
 * @SPA_EVENT_TYPE_ADD_POLL: emited when a pollfd should be added
 * @SPA_EVENT_TYPE_REMOVE_POLL: emited when a pollfd should be removed
 * @SPA_EVENT_TYPE_DRAINED: emited when DRAIN command completed
 * @SPA_EVENT_TYPE_MARKER: emited when MARK command completed
 * @SPA_EVENT_TYPE_ERROR: emited when error occured
 * @SPA_EVENT_TYPE_BUFFERING: emited when buffering is in progress
 */
typedef enum {
  SPA_EVENT_TYPE_INVALID                  = 0,
  SPA_EVENT_TYPE_STARTED,
  SPA_EVENT_TYPE_STOPPED,
  SPA_EVENT_TYPE_CAN_PULL_OUTPUT,
  SPA_EVENT_TYPE_CAN_PUSH_INTPUT,
  SPA_EVENT_TYPE_PULL_INPUT,
  SPA_EVENT_TYPE_ALLOC_OUTPUT,
  SPA_EVENT_TYPE_ADD_POLL,
  SPA_EVENT_TYPE_REMOVE_POLL,
  SPA_EVENT_TYPE_DRAINED,
  SPA_EVENT_TYPE_MARKER,
  SPA_EVENT_TYPE_ERROR,
  SPA_EVENT_TYPE_BUFFERING,
} SpaEventType;

struct _SpaEvent {
  volatile int   refcount;
  SpaNotify      notify;
  SpaEventType   type;
  uint32_t       port_id;
  void          *data;
  size_t         size;
};

/**
 * SpaEventPoll:
 * @fd: a file descriptor to watch
 * @events: events to watch for
 * @revents: result events
 * @callback: callback called when there was activity on @fd
 * @user_data: user data to pass to @callback
 */
typedef struct {
  int   fd;
  short events;
  short revents;
  SpaNotify callback;
  void     *user_data;
} SpaEventPoll;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_EVENT_H__ */
