/* Simple Plugin Interface
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

#ifndef __SPI_EVENT_H__
#define __SPI_EVENT_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpiEvent SpiEvent;

#include <spi/defs.h>

/**
 * SpiEventType:
 * @SPI_EVENT_TYPE_INVALID: invalid event, should be ignored
 * @SPI_EVENT_TYPE_ACTIVATED: emited when the ACTIVATE command completes
 * @SPI_EVENT_TYPE_DEACTIVATED: emited when the DEACTIVATE command completes
 * @SPI_EVENT_TYPE_CAN_PULL_OUTPUT: emited when an async node has output that can be pulled
 * @SPI_EVENT_TYPE_CAN_PUSH_INTPUT: emited when more data can be pushed to an async node
 * @SPI_EVENT_TYPE_PULL_INPUT: emited when data needs to be provided on an input
 * @SPI_EVENT_TYPE_ADD_POLL: emited when a pollfd should be added
 * @SPI_EVENT_TYPE_REMOVE_POLL: emited when a pollfd should be removed
 * @SPI_EVENT_TYPE_DRAINED: emited when DRAIN command completed
 * @SPI_EVENT_TYPE_MARKER: emited when MARK command completed
 * @SPI_EVENT_TYPE_ERROR: emited when error occured
 * @SPI_EVENT_TYPE_BUFFERING: emited when buffering is in progress
 */
typedef enum {
  SPI_EVENT_TYPE_INVALID                  = 0,
  SPI_EVENT_TYPE_ACTIVATED,
  SPI_EVENT_TYPE_DEACTIVATED,
  SPI_EVENT_TYPE_CAN_PULL_OUTPUT,
  SPI_EVENT_TYPE_CAN_PUSH_INTPUT,
  SPI_EVENT_TYPE_PULL_INPUT,
  SPI_EVENT_TYPE_ADD_POLL,
  SPI_EVENT_TYPE_REMOVE_POLL,
  SPI_EVENT_TYPE_DRAINED,
  SPI_EVENT_TYPE_MARKER,
  SPI_EVENT_TYPE_ERROR,
  SPI_EVENT_TYPE_BUFFERING,
} SpiEventType;

struct _SpiEvent {
  volatile int   refcount;
  SpiNotify      notify;
  SpiEventType   type;
  uint32_t       port_id;
  void          *data;
  size_t         size;
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPI_EVENT_H__ */
