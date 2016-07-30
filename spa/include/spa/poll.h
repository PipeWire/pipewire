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

#ifndef __SPA_POLL_H__
#define __SPA_POLL_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaEvent SpaEvent;

#include <spa/defs.h>

/**
 * SpaPollFd:
 * @fd: a file descriptor
 * @events: events to watch
 * @revents: events after poll
 */
typedef struct {
  int   fd;
  short events;
  short revents;
} SpaPollFd;

/**
 * SpaPollNotifyData:
 * @user_data: user data
 * @fds: array of file descriptors
 * @n_fds: number of elements in @fds
 * @now: the current time
 * @timeout: the next desired wakeup time relative to @now
 *
 * Data passed to #SpaPollNotify.
 */
typedef struct {
  void *user_data;
  SpaPollFd *fds;
  unsigned int n_fds;
  uint64_t now;
  uint64_t timeout;
} SpaPollNotifyData;

typedef int (*SpaPollNotify) (SpaPollNotifyData *data);

/**
 * SpaPollItem:
 * @id: id of the poll item
 * @fds: array of file descriptors to watch
 * @n_fds: number of elements in @fds
 * @idle_cb: callback called when there is no other work
 * @idle_cb: callback called before starting the poll
 * @idle_cb: callback called after the poll loop
 * @user_data: user data pass to callbacks
 */
typedef struct {
  uint32_t       id;
  SpaPollFd     *fds;
  unsigned int   n_fds;
  SpaPollNotify  idle_cb;
  SpaPollNotify  before_cb;
  SpaPollNotify  after_cb;
  void          *user_data;
} SpaPollItem;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_POLL_H__ */
